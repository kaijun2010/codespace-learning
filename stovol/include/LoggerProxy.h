#ifndef OPT_STOVOL_TEMPLATE_H
#define OPT_STOVOL_TEMPLATE_H

#include <chrono>
#include <iomanip> // 新增時間格式用
#include <iostream>
#include <sstream>
#include <string>

#ifdef DEV_TEST_MODE
// 測試模式下，COUTX 使用 std::cout
#define COUTX std::cout
#define ENDLX std::endl

// 新增：測試模式下的 INFO 輸出（同 std::cout）
#define CINFOX std::cout

#else
#include <fmt/printf.h>
#include <spdlog/spdlog.h>

extern std::shared_ptr<spdlog::logger> g_ActiveLogger;

class LogLine {
  std::ostringstream m_ss;

public:
  typedef std::ostream &(*ManipFn)(std::ostream &);
  typedef std::ios_base &(*FlagsFn)(std::ios_base &);

  LogLine() {}
  template <typename T> LogLine &operator<<(const T &t) {
    m_ss << t;
    return *this;
  }
  LogLine &operator<<(ManipFn manip) {
    manip(m_ss);
    return *this;
  }
  LogLine &operator<<(FlagsFn manip) {
    manip(m_ss);
    return *this;
  }
  std::string str() const { return m_ss.str(); }
};

class LoggerProxy {
  std::shared_ptr<spdlog::logger> m_log;

public:
  LoggerProxy(std::shared_ptr<spdlog::logger> log) : m_log(std::move(log)) {}
  bool operator==(const LogLine &line) {
    m_log->info("{}", line.str());
    return true;
  }
};

// 新增：有時間戳與 [I] 標記的 INFO LoggerProxy
class LoggerProxyInfo {
  std::shared_ptr<spdlog::logger> m_log;

public:
  LoggerProxyInfo(std::shared_ptr<spdlog::logger> log) : m_log(std::move(log)) {}
  bool operator==(const LogLine &line) {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto itt = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream ts;
    ts << std::put_time(std::localtime(&itt), "%H:%M:%S") << "." << std::setw(3)
       << std::setfill('0') << ms.count();

    m_log->info("{} [I] {}", ts.str(), line.str());
    return true;
  }
};

#define LOGTEXT(log) log &&LoggerProxy(log) == LogLine()
#define LOGTEXTINFO(log) log &&LoggerProxyInfo(log) == LogLine()

#define COUTX LOGTEXT(g_ActiveLogger)
#define ENDLX ""

// 新增：正式環境下 CINFOX / ENDLX
#define CINFOX LOGTEXTINFO(g_ActiveLogger)

#endif

#endif /* OPT_STOVOL_TEMPLATE_H */
