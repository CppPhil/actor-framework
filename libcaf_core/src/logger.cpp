/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright 2011-2018 Dominik Charousset                                     *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#include "caf/logger.hpp"

#include <cassert>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <thread>
#include <unordered_map>
#include <utility>

#include "caf/actor_proxy.hpp"
#include "caf/actor_system.hpp"
#include "caf/actor_system_config.hpp"
#include "caf/config.hpp"
#include "caf/defaults.hpp"
#include "caf/detail/get_process_id.hpp"
#include "caf/detail/pretty_type_name.hpp"
#include "caf/detail/set_thread_name.hpp"
#include "caf/intrusive/task_result.hpp"
#include "caf/local_actor.hpp"
#include "caf/locks.hpp"
#include "caf/message.hpp"
#include "caf/string_algorithms.hpp"
#include "caf/term.hpp"
#include "caf/timestamp.hpp"

namespace caf {

namespace {

// Stores the ID of the currently running actor.
thread_local actor_id current_actor_id;

// Stores a pointer to the system-wide logger.
thread_local intrusive_ptr<logger> current_logger_ptr;

constexpr string_view log_level_name[] = {
  "QUIET", "",     "", "ERROR", "",      "", "WARN", "",
  "",      "INFO", "", "",      "DEBUG", "", "",     "TRACE",
};

constexpr string_view fun_prefixes[] = {
  "virtual ", "static ", "const ", "signed ", "unsigned ",
};

// Various spellings of the anonymous namespace as reported by CAF_PRETTY_FUN.
constexpr string_view anon_ns[] = {
  "(anonymous namespace)", // Clang
  "{anonymous}",           // GCC
  "`anonymous-namespace'", // MSVC
};

/// Converts a verbosity level to its integer counterpart.
unsigned to_level_int(string_view x) {
  if (x == "error")
    return CAF_LOG_LEVEL_ERROR;
  if (x == "warning")
    return CAF_LOG_LEVEL_WARNING;
  if (x == "info")
    return CAF_LOG_LEVEL_INFO;
  if (x == "debug")
    return CAF_LOG_LEVEL_DEBUG;
  if (x == "trace")
    return CAF_LOG_LEVEL_TRACE;
  return CAF_LOG_LEVEL_QUIET;
}

// Reduces symbol by printing all prefixes to `out` and returning the
// remainder. For example, "ns::foo::bar" prints "ns.foo" to `out` and returns
// "bar".
string_view reduce_symbol(std::ostream& out, string_view symbol) {
  auto skip = [&](string_view str) {
    if (starts_with(symbol, str))
      symbol.remove_prefix(str.size());
  };
  // MSVC adds `struct` to symbol names. For example:
  // void __cdecl `anonymous-namespace'::foo::tpl<struct T>::run(void)
  //                                              ^~~~~~
  skip("struct ");
  string_view last = "";
  bool printed = false;
  // Prints the content of `last` and then replaces it with `y`.
  auto set_last = [&](string_view y) {
    if (!last.empty()) {
      if (printed)
        out << ".";
      else
        printed = true;
      for (auto ch : last)
        if (ch == ' ')
          out << "%20";
        else
          out << ch;
    }
    last = y;
  };
  size_t pos = 0;
  auto advance = [&](size_t n) {
    set_last(symbol.substr(0, pos));
    symbol.remove_prefix(pos + n);
    pos = 0;
  };
  auto flush = [&] {
    advance(1);
    // Some compilers put a whitespace after nested templates that we wish to
    // ignore here, e.g.,
    // foo::tpl<foo::tpl<int> >::fun(int)
    //                       ^
    if (last != " ")
      set_last("");
  };
  while (pos < symbol.size()) {
    switch (symbol[pos]) {
      // A colon can only appear as scope separator, i.e., "::".
      case ':':
        advance(2);
        break;
      // These characters are invalid in function names, unless they indicate
      // an anonymous namespace or the beginning of the argument list.
      case '`':
      case '{':
      case '(': {
        auto pred = [&](string_view x) { return starts_with(symbol, x); };
        auto i = std::find_if(std::begin(anon_ns), std::end(anon_ns), pred);
        if (i != std::end(anon_ns)) {
          set_last("$");
          // The anonymous namespace is always followed by "::".
          symbol.remove_prefix(i->size() + 2);
          pos = 0;
          break;
        }
        // We reached the end of the function name. Print "GLOBAL" if we didn't
        // print anything yet as "global namespace".
        set_last("");
        if (!printed)
          out << "GLOBAL";
        return symbol;
      }
      case '<':
        flush();
        out << '<';
        symbol = reduce_symbol(out, symbol);
        break;
      case '>':
        flush();
        out << '>';
        return symbol;
      default:
        ++pos;
    }
  }
  return symbol;
}

} // namespace

logger::config::config()
  : verbosity(CAF_LOG_LEVEL),
    file_verbosity(CAF_LOG_LEVEL),
    console_verbosity(CAF_LOG_LEVEL),
    inline_output(false),
    console_coloring(false) {
  // nop
}

logger::event::event(unsigned lvl, unsigned line, string_view cat,
                     string_view full_fun, string_view fun, string_view fn,
                     std::string msg, std::thread::id t, actor_id a,
                     timestamp ts)
  : level(lvl),
    line_number(line),
    category_name(cat),
    pretty_fun(full_fun),
    simple_fun(fun),
    file_name(fn),
    message(std::move(msg)),
    tid(std::move(t)),
    aid(a),
    tstamp(ts) {
  // nop
}

logger::line_builder::line_builder() {
  // nop
}

logger::line_builder&
logger::line_builder::operator<<(const local_actor* self) {
  return *this << self->name();
}

logger::line_builder& logger::line_builder::operator<<(const std::string& str) {
  return *this << str.c_str();
}

logger::line_builder& logger::line_builder::operator<<(string_view str) {
  if (!str_.empty() && str_.back() != ' ')
    str_ += " ";
  str_.insert(str_.end(), str.begin(), str.end());
  return *this;
}

logger::line_builder& logger::line_builder::operator<<(const char* str) {
  if (!str_.empty() && str_.back() != ' ')
    str_ += " ";
  str_ += str;
  return *this;
}

logger::line_builder& logger::line_builder::operator<<(char x) {
  const char buf[] = {x, '\0'};
  return *this << buf;
}

std::string logger::line_builder::get() const {
  return std::move(str_);
}

// returns the actor ID for the current thread

actor_id logger::thread_local_aid() {
  return current_actor_id;
}

actor_id logger::thread_local_aid(actor_id aid) {
  std::swap(current_actor_id, aid);
  return aid;
}

void logger::log(event&& x) {
  if (cfg_.inline_output)
    handle_event(x);
  else
    queue_.push_back(std::move(x));
}

void logger::set_current_actor_system(actor_system* x) {
  current_logger_ptr.reset(x != nullptr ? &x->logger() : nullptr);
}

logger* logger::current_logger() {
  return current_logger_ptr.get();
}

bool logger::accepts(unsigned level, string_view cname) {
  if (level > cfg_.verbosity)
    return false;
  return !std::any_of(component_blacklist.begin(), component_blacklist.end(),
                      [=](string_view name) { return name == cname; });
}

logger::logger(actor_system& sys) : system_(sys), t0_(make_timestamp()) {
  // nop
}

logger::~logger() {
  stop();
  // tell system our dtor is done
  std::unique_lock<std::mutex> guard{system_.logger_dtor_mtx_};
  system_.logger_dtor_done_ = true;
  system_.logger_dtor_cv_.notify_one();
}

void logger::init(actor_system_config& cfg) {
  CAF_IGNORE_UNUSED(cfg);
  namespace lg = defaults::logger;
  using string_list = std::vector<std::string>;
  auto blacklist = get_if<string_list>(&cfg, "logger.component-blacklist");
  if (blacklist)
    component_blacklist = move_if_optional(blacklist);
  // Parse the configured log level. We only store a string_view to the
  // verbosity levels, so we make sure we actually get a string pointer here
  // (and not an optional<string>).
  const std::string* verbosity = get_if<std::string>(&cfg, "logger.verbosity");
  auto set_level = [&](auto& var, auto var_default, string_view var_key) {
    // User-provided overrides have the highest priority.
    if (const std::string* var_override = get_if<std::string>(&cfg, var_key)) {
      var = *var_override;
      return;
    }
    // If present, "logger.verbosity" overrides the defaults for both file and
    // console verbosity level.
    if (verbosity) {
      var = *verbosity;
      return;
    }
    var = var_default;
  };
  string_view file_str;
  string_view console_str;
  set_level(file_str, lg::file_verbosity, "logger.file-verbosity");
  set_level(console_str, lg::console_verbosity, "logger.console-verbosity");
  cfg_.file_verbosity = to_level_int(file_str);
  cfg_.console_verbosity = to_level_int(console_str);
  cfg_.verbosity = std::max(cfg_.file_verbosity, cfg_.console_verbosity);
  // Parse the format string.
  file_format_
    = parse_format(get_or(cfg, "logger.file-format", lg::file_format));
  console_format_
    = parse_format(get_or(cfg, "logger.console-format", lg::console_format));
  // Set flags.
  if (get_or(cfg, "logger.inline-output", false))
    cfg_.inline_output = true;
  auto con = get_or(cfg, "logger.console", lg::console);
  if (con == "colored") {
    cfg_.console_coloring = true;
  } else if (con != "uncolored") {
    // Disable console output if neither 'colored' nor 'uncolored' are present.
    cfg_.console_verbosity = CAF_LOG_LEVEL_QUIET;
    cfg_.verbosity = cfg_.file_verbosity;
  }
}

bool logger::open_file() {
  if (file_verbosity() == CAF_LOG_LEVEL_QUIET || file_name_.empty())
    return false;
  file_.open(file_name_, std::ios::out | std::ios::app);
  if (!file_) {
    std::cerr << "unable to open log file " << file_name_ << std::endl;
    return false;
  }
  return true;
}

void logger::render_fun_prefix(std::ostream& out, const event& x) {
  // Extract the prefix of a function name. For example:
  // virtual std::vector<int> my::namespace::foo(int);
  //                          ^~~~~~~~~~~~~
  // Here, we output Java-style "my.namespace" to `out`.
  auto reduced = x.pretty_fun;
  // Skip all prefixes that can precede the return type.
  auto skip = [&](string_view str) {
    if (starts_with(reduced, str)) {
      reduced.remove_prefix(str.size());
      return true;
    }
    return false;
  };
  // Remove any type of the return type.
  while (std::any_of(std::begin(fun_prefixes), std::end(fun_prefixes), skip))
    ; // Repeat.
  // Skip the return type.
  auto skip_return_type = [&] {
    size_t template_nesting = 0;
    size_t pos = 0;
    for (size_t pos = 0; pos < reduced.size(); ++pos) {
      switch (reduced[pos]) {
        case ' ':
          if (template_nesting == 0) {
            // Skip any pointers and references. We need to loop, because each
            // pointer/reference can be const-qualified.
            do {
              pos = reduced.find_first_not_of(" *&", pos);
              reduced.remove_prefix(pos);
              pos = 0;
            } while (skip("const"));
            return;
          }
          break;
        case '<':
          ++template_nesting;
          break;
        case '>':
          --template_nesting;
          break;
        default:
          break;
      }
    }
    reduced.remove_prefix(pos);
  };
  skip_return_type();
  // MSVC puts '__cdecl' between the return type and the function name.
  skip("__cdecl ");
  // We reached the function name itself and can recursively print the prefix.
  reduce_symbol(out, reduced);
}

void logger::render_fun_name(std::ostream& out, const event& e) {
  out << e.simple_fun;
}

void logger::render_date(std::ostream& out, timestamp x) {
  out << deep_to_string(x);
}

namespace {
struct logger_id {
  actor_id aid;
  std::thread::id tid;

  friend bool operator<(const logger_id& x, const logger_id& y) noexcept {
    return x.aid == 0 && y.aid == 0 ? x.tid < y.tid : x.aid < y.aid;
  }
};

class monitor {
  template <class Invocable>
  decltype(auto) do_it(Invocable&& invocable) {
    std::lock_guard<std::mutex> lock_guard(mutex_);
    (void) lock_guard;
    return std::invoke(std::forward<Invocable>(invocable));
  }

  template <class Invocable>
  decltype(auto) do_it(Invocable&& invocable) const {
    std::lock_guard<std::mutex> lock_guard(mutex_);
    (void) lock_guard;
    return std::invoke(std::forward<Invocable>(invocable));
  }

public:
  std::vector<size_t> accept(logger_id lid) {
    return do_it([this, lid = std::move(lid)] {
      auto it = data_.find(lid);

      if (it == data_.end()) {
        vector_timestamp_.push_back(0);

        if (lid.aid == 0)
          name_.push_back("thread" + std::to_string(++thread_count_));
        else
          name_.push_back("actor" + std::to_string(lid.aid));

        auto [iter, always_true] = data_.emplace(std::move(lid),
                                                 vector_timestamp_.size() - 1);
        it = iter;
      }

      auto& [logger_id, vid] = *it;
      ++vector_timestamp_.at(vid);

      return vector_timestamp_;
    });
  }

  size_t vid(logger_id lid) const {
    return do_it([this, &lid] {
      auto it = data_.find(lid);
      assert(it != data_.end());
      const auto& [logger_id, vid] = *it;
      return vid;
    });
  }

  std::vector<size_t> get() const {
    return do_it([this] { return vector_timestamp_; });
  }

  std::string name(size_t vid) const {
    return do_it([this, vid] { return name_.at(vid); });
  }

private:
  std::map<logger_id, size_t> data_;
  std::vector<std::string> name_;
  std::vector<size_t> vector_timestamp_;
  size_t thread_count_ = 0;
  mutable std::mutex mutex_;
} monitor_instance;

std::string json_vector_timestamp(const std::vector<size_t>& vstamp) {
  // create ShiViz compatible JSON-formatted vector timestamp
  std::ostringstream oss;
  oss << '{';
  bool need_comma = false;

  for (size_t i = 0; i < vstamp.size(); ++i) {
    auto x = vstamp[i];
    if (x > 0) {
      if (need_comma)
        oss << ',';
      else
        need_comma = true;
      oss << '"' << monitor_instance.name(i) << '"' << ':' << x;
    }
  }

  oss << '}';
  return oss.str();
}
} // namespace

// TODO: HERE: The log entry is actually written here.
void logger::render(std::ostream& out, const line_format& lf,
                    const event& x) const {
  auto ms_time_diff = [](timestamp t0, timestamp tn) {
    using namespace std::chrono;
    return duration_cast<milliseconds>(tn - t0).count();
  };

  const logger_id lid = {x.aid, x.tid};
  const auto vstamp = monitor_instance.accept(lid);
  const auto vid = monitor_instance.vid(lid);
  const auto actor_name = monitor_instance.name(vid);
  out << json_vector_timestamp(vstamp) << ' ';

  // clang-format off
  for (auto& f : lf)
    switch (f.kind) {
      case category_field:     out << x.category_name;             break;
      case class_name_field:   render_fun_prefix(out, x);          break;
      case date_field:         render_date(out, x.tstamp);         break;
      case file_field:         out << x.file_name;                 break;
      case line_field:         out << x.line_number;               break;
      case message_field:      out << x.message;                   break;
      case method_field:       render_fun_name(out, x);            break;
      case newline_field:      out << std::endl;                   break;
      case priority_field:     out << log_level_name[x.level];     break;
      case runtime_field:      out << ms_time_diff(t0_, x.tstamp); break;
      case thread_field:       out << x.tid;                       break;
      case actor_field:        out << actor_name;                  break;
      case percent_sign_field: out << '%';                         break;
      case plain_text_field:   out << f.text;                      break;
      default: ; // nop
    }
  // clang-format on
}

logger::line_format logger::parse_format(const std::string& format_str) {
  std::vector<field> res;
  auto plain_text_first = format_str.begin();
  bool read_percent_sign = false;
  auto i = format_str.begin();
  for (; i != format_str.end(); ++i) {
    if (read_percent_sign) {
      field_type ft;
      // clang-format off
      switch (*i) {
        case 'c': ft = category_field;     break;
        case 'C': ft = class_name_field;   break;
        case 'd': ft = date_field;         break;
        case 'F': ft =  file_field;        break;
        case 'L': ft = line_field;         break;
        case 'm': ft = message_field;      break;
        case 'M': ft = method_field;       break;
        case 'n': ft = newline_field;      break;
        case 'p': ft = priority_field;     break;
        case 'r': ft = runtime_field;      break;
        case 't': ft = thread_field;       break;
        case 'a': ft = actor_field;        break;
        case '%': ft = percent_sign_field; break;
        default:
          ft = invalid_field;
          std::cerr << "invalid field specifier in format string: "
                    << *i << std::endl;
      }
      // clang-format on
      if (ft != invalid_field)
        res.emplace_back(field{ft, std::string{}});
      plain_text_first = i + 1;
      read_percent_sign = false;
    } else {
      if (*i == '%') {
        if (plain_text_first != i)
          res.emplace_back(
            field{plain_text_field, std::string{plain_text_first, i}});
        read_percent_sign = true;
      }
    }
  }
  if (plain_text_first != i)
    res.emplace_back(field{plain_text_field, std::string{plain_text_first, i}});
  return res;
}

string_view logger::skip_path(string_view path) {
  auto find_slash = [&] { return path.find('/'); };
  for (auto p = find_slash(); p != string_view::npos; p = find_slash())
    path.remove_prefix(p + 1);
  return path;
}

void logger::run() {
  // Bail out without printing anything if the first event we receive is the
  // shutdown (empty) event.
  queue_.wait_nonempty();
  if (queue_.front().message.empty())
    return;
  if (!open_file() && console_verbosity() == CAF_LOG_LEVEL_QUIET)
    return;
  log_first_line();
  // Loop until receiving an empty message.
  for (;;) {
    // Handle current head of the queue.
    auto& e = queue_.front();
    if (e.message.empty()) {
      log_last_line();
      return;
    }
    handle_event(e);
    // Prepare next iteration.
    queue_.pop_front();
    queue_.wait_nonempty();
  }
}

void logger::handle_file_event(const event& x) {
  // Print to file if available.
  if (file_ && x.level <= file_verbosity())
    render(file_, file_format_, x);
}

void logger::handle_console_event(const event& x) {
  if (x.level > console_verbosity())
    return;
  if (cfg_.console_coloring) {
    switch (x.level) {
      default:
        break;
      case CAF_LOG_LEVEL_ERROR:
        std::clog << term::red;
        break;
      case CAF_LOG_LEVEL_WARNING:
        std::clog << term::yellow;
        break;
      case CAF_LOG_LEVEL_INFO:
        std::clog << term::green;
        break;
      case CAF_LOG_LEVEL_DEBUG:
        std::clog << term::cyan;
        break;
      case CAF_LOG_LEVEL_TRACE:
        std::clog << term::blue;
        break;
    }
    render(std::clog, console_format_, x);
    std::clog << term::reset_endl;
  } else {
    render(std::clog, console_format_, x);
    std::clog << std::endl;
  }
}

void logger::handle_event(const event& x) {
  handle_file_event(x);
  handle_console_event(x);
}

void logger::log_first_line() {
  auto e = CAF_LOG_MAKE_EVENT(0, CAF_LOG_COMPONENT, CAF_LOG_LEVEL_DEBUG, "");
  auto make_message = [&](string_view config_name, std::string default_value) {
    std::string msg = "level = ";
    msg += get_or(system_.config(), config_name, default_value);
    msg += ", node = ";
    msg += to_string(system_.node());
    msg += ", component-blacklist = ";
    msg += deep_to_string(component_blacklist);
    return msg;
  };
  namespace lg = defaults::logger;
  e.message = make_message("logger.file-verbosity",
                           to_string(lg::file_verbosity));
  handle_file_event(e);
  e.message = make_message("logger.console-verbosity",
                           to_string(lg::console_verbosity));
  handle_console_event(e);
}

void logger::log_last_line() {
  auto e = CAF_LOG_MAKE_EVENT(0, CAF_LOG_COMPONENT, CAF_LOG_LEVEL_DEBUG, "");
  handle_event(e);
}

void logger::start() {
  parent_thread_ = std::this_thread::get_id();
  if (verbosity() == CAF_LOG_LEVEL_QUIET)
    return;
  file_name_ = get_or(system_.config(), "logger.file-name",
                      defaults::logger::file_name);
  if (file_name_.empty()) {
    // No need to continue if console and log file are disabled.
    if (console_verbosity() == CAF_LOG_LEVEL_QUIET)
      return;
  } else {
    // Replace placeholders.
    const char pid[] = "[PID]";
    auto i = std::search(file_name_.begin(), file_name_.end(), std::begin(pid),
                         std::end(pid) - 1);
    if (i != file_name_.end()) {
      auto id = std::to_string(detail::get_process_id());
      file_name_.replace(i, i + sizeof(pid) - 1, id);
    }
    const char ts[] = "[TIMESTAMP]";
    i = std::search(file_name_.begin(), file_name_.end(), std::begin(ts),
                    std::end(ts) - 1);
    if (i != file_name_.end()) {
      auto t0_str = timestamp_to_string(t0_);
      file_name_.replace(i, i + sizeof(ts) - 1, t0_str);
    }
    const char node[] = "[NODE]";
    i = std::search(file_name_.begin(), file_name_.end(), std::begin(node),
                    std::end(node) - 1);
    if (i != file_name_.end()) {
      auto nid = to_string(system_.node());
      file_name_.replace(i, i + sizeof(node) - 1, nid);
    }
  }
  if (cfg_.inline_output) {
    // Open file immediately for inline output.
    open_file();
    log_first_line();
  } else {
    thread_ = std::thread{[this] {
      detail::set_thread_name("caf.logger");
      this->system_.thread_started();
      this->run();
      this->system_.thread_terminates();
    }};
  }
}

void logger::stop() {
  if (cfg_.inline_output) {
    log_last_line();
    return;
  }
  if (!thread_.joinable())
    return;
  // A default-constructed event causes the logger to shutdown.
  queue_.push_back(event{});
  thread_.join();
}

std::string to_string(logger::field_type x) {
  static constexpr const char* names[]
    = {"invalid", "category", "class_name", "date",         "file",
       "line",    "message",  "method",     "newline",      "priority",
       "runtime", "thread",   "actor",      "percent_sign", "plain_text"};
  return names[static_cast<size_t>(x)];
}

std::string to_string(const logger::field& x) {
  std::string result = "field{";
  result += to_string(x.kind);
  if (x.kind == logger::plain_text_field) {
    result += ", \"";
    result += x.text;
    result += '\"';
  }
  result += "}";
  return result;
}

bool operator==(const logger::field& x, const logger::field& y) {
  return x.kind == y.kind && x.text == y.text;
}

} // namespace caf
