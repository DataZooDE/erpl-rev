#include "logging.hpp"

#if defined(_WIN32)
#  include <io.h>      // _isatty, _fileno
#  define isatty _isatty
#  define fileno _fileno
// POSIX *_r take (time_t*, tm*); MSVC *_s take (tm*, time_t*) — adapt by name.
#  define gmtime_r(t, tm)    gmtime_s((tm), (t))
#  define localtime_r(t, tm) localtime_s((tm), (t))
#else
#  include <unistd.h>  // isatty, fileno
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace erpl_rev {
namespace log {

namespace {

// ANSI SGR codes per level (console mode only).
const char *kReset = "\x1b[0m";
const char *LevelColor(Level lvl) {
    switch (lvl) {
        case Level::Error: return "\x1b[31m";  // red
        case Level::Warn:  return "\x1b[33m";  // yellow
        case Level::Info:  return "\x1b[32m";  // green
        case Level::Debug: return "\x1b[36m";  // cyan
        case Level::Trace: return "\x1b[2m";   // dim
    }
    return "";
}

const char *LevelName(Level lvl) {
    switch (lvl) {
        case Level::Error: return "ERROR";
        case Level::Warn:  return "WARN";
        case Level::Info:  return "INFO";
        case Level::Debug: return "DEBUG";
        case Level::Trace: return "TRACE";
    }
    return "?????";
}

// Left-padded to 5 chars so the columns line up in console output.
const char *LevelNamePadded(Level lvl) {
    switch (lvl) {
        case Level::Error: return "ERROR";
        case Level::Warn:  return "WARN ";
        case Level::Info:  return "INFO ";
        case Level::Debug: return "DEBUG";
        case Level::Trace: return "TRACE";
    }
    return "?????";
}

bool ParseLevel(const char *s, Level &out) {
    if (!s) return false;
    if (!std::strcmp(s, "error")) { out = Level::Error; return true; }
    if (!std::strcmp(s, "warn"))  { out = Level::Warn;  return true; }
    if (!std::strcmp(s, "info"))  { out = Level::Info;  return true; }
    if (!std::strcmp(s, "debug")) { out = Level::Debug; return true; }
    if (!std::strcmp(s, "trace")) { out = Level::Trace; return true; }
    return false;
}

// "2026-05-31 12:00:00.123" for console; ISO-8601 UTC for JSON.
std::string Timestamp(bool iso_utc) {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
    char buf[32];
    if (iso_utc) {
        gmtime_r(&t, &tm);
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    } else {
        localtime_r(&t, &tm);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    }
    char out[40];
    std::snprintf(out, sizeof(out), "%s.%03lld%s", buf, (long long)ms.count(),
                  iso_utc ? "Z" : "");
    return out;
}

// Escape a string for embedding in a JSON string literal.
void AppendJsonEscaped(std::string &dst, const std::string &s) {
    for (char c : s) {
        switch (c) {
            case '"':  dst += "\\\""; break;
            case '\\': dst += "\\\\"; break;
            case '\n': dst += "\\n";  break;
            case '\r': dst += "\\r";  break;
            case '\t': dst += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char u[8];
                    std::snprintf(u, sizeof(u), "\\u%04x", c);
                    dst += u;
                } else {
                    dst += c;
                }
        }
    }
}

}  // namespace

Logger &Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::Configure() {
    std::lock_guard<std::mutex> lk(mtx_);

    Level lvl;
    if (ParseLevel(std::getenv("ERPL_REV_LOG_LEVEL"), lvl)) level_ = lvl;

    const char *fmt = std::getenv("ERPL_REV_LOG_FORMAT");
    json_ = (fmt && !std::strcmp(fmt, "json"));

    // Colour: never in JSON mode; otherwise honour ERPL_REV_LOG_COLOR, then
    // NO_COLOR, then fall back to "colour only when stderr is a TTY".
    const char *col = std::getenv("ERPL_REV_LOG_COLOR");
    if (json_) {
        color_ = false;
    } else if (col && !std::strcmp(col, "always")) {
        color_ = true;
    } else if (col && !std::strcmp(col, "never")) {
        color_ = false;
    } else if (std::getenv("NO_COLOR")) {
        color_ = false;
    } else {
        color_ = isatty(fileno(stderr)) != 0;
    }
}

void Logger::Log(Level lvl, const char *component, const std::string &message, Fields fields) {
    if (!ShouldEmit(lvl)) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (json_) EmitJson(lvl, component, message, fields);
    else       EmitConsole(lvl, component, message, fields);
}

void Logger::EmitConsole(Level lvl, const char *component, const std::string &message,
                         Fields fields) {
    std::string line = Timestamp(false);
    line += ' ';
    if (color_) { line += LevelColor(lvl); line += LevelNamePadded(lvl); line += kReset; }
    else        { line += LevelNamePadded(lvl); }
    line += " [";
    line += component ? component : "?";
    line += "] ";
    line += message;
    for (const auto &f : fields) {
        line += ' ';
        line += f.key;
        line += '=';
        if (f.quote) { line += '"'; line += f.value; line += '"'; }
        else         { line += f.value; }
    }
    line += '\n';
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

void Logger::EmitJson(Level lvl, const char *component, const std::string &message,
                      Fields fields) {
    std::string line = "{\"ts\":\"";
    line += Timestamp(true);
    line += "\",\"level\":\"";
    line += LevelName(lvl);
    line += "\",\"component\":\"";
    AppendJsonEscaped(line, component ? component : "?");
    line += "\",\"msg\":\"";
    AppendJsonEscaped(line, message);
    line += "\"";
    for (const auto &f : fields) {
        line += ",\"";
        AppendJsonEscaped(line, f.key);
        line += "\":";
        if (f.quote) {
            line += '"';
            AppendJsonEscaped(line, f.value);
            line += '"';
        } else {
            line += f.value;
        }
    }
    line += "}\n";
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

}  // namespace log
}  // namespace erpl_rev
