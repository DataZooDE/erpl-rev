// Configurable, structured logging for erpl-rev.
//
// Modeled on erpl's ErplTracer (level + component + timestamp, thread-safe),
// extended with ANSI colour highlighting per level and a JSON-lines output
// toggle so records can either be read by a human or shipped to a collector.
//
// Configured once from the environment (see Logger::Configure):
//   ERPL_REV_LOG_LEVEL   error|warn|info|debug|trace   (default info)
//   ERPL_REV_LOG_FORMAT  console|json                  (default console)
//   ERPL_REV_LOG_COLOR   auto|always|never             (default auto)
//
// All records go to stderr (keeps stdout free, conventional for server logs).
#pragma once

#include <initializer_list>
#include <mutex>
#include <string>

namespace erpl_rev {
namespace log {

enum class Level { Error = 0, Warn, Info, Debug, Trace };

// One structured key/value pair attached to a log record. `quote` controls
// whether the value is rendered as a string (quoted) or verbatim (numbers,
// bools) in both console and JSON output.
struct Field {
    std::string key;
    std::string value;
    bool quote = true;

    Field(std::string k, std::string v, bool q = true)
        : key(std::move(k)), value(std::move(v)), quote(q) {}
    // Convenience for integer-ish values rendered without quotes.
    Field(std::string k, long long v)
        : key(std::move(k)), value(std::to_string(v)), quote(false) {}
};

using Fields = std::initializer_list<Field>;

class Logger {
  public:
    static Logger &Instance();

    // Read the ERPL_REV_LOG_* environment once. Safe to call repeatedly.
    void Configure();

    bool ShouldEmit(Level lvl) const { return static_cast<int>(lvl) <= static_cast<int>(level_); }

    void Log(Level lvl, const char *component, const std::string &message, Fields fields = {});

    void Error(const char *component, const std::string &message, Fields fields = {}) {
        Log(Level::Error, component, message, fields);
    }
    void Warn(const char *component, const std::string &message, Fields fields = {}) {
        Log(Level::Warn, component, message, fields);
    }
    void Info(const char *component, const std::string &message, Fields fields = {}) {
        Log(Level::Info, component, message, fields);
    }
    void Debug(const char *component, const std::string &message, Fields fields = {}) {
        Log(Level::Debug, component, message, fields);
    }
    void Trace(const char *component, const std::string &message, Fields fields = {}) {
        Log(Level::Trace, component, message, fields);
    }

  private:
    Logger() = default;

    void EmitConsole(Level lvl, const char *component, const std::string &message, Fields fields);
    void EmitJson(Level lvl, const char *component, const std::string &message, Fields fields);

    mutable std::mutex mtx_;
    Level level_ = Level::Info;
    bool json_ = false;
    bool color_ = false;
};

// Shorthand accessor used at call sites: log::get().Info("rfc", ...).
inline Logger &get() { return Logger::Instance(); }

}  // namespace log
}  // namespace erpl_rev
