#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace krkrspeed {

enum class LogLevel { Debug, Info, Warn, Error };

#ifndef KRKR_ENABLE_LOGGING
#define KRKR_ENABLE_LOGGING 1
#endif

#if KRKR_ENABLE_LOGGING
void SetLoggingEnabled(bool enabled);
bool IsLoggingOn();
void SetLogDirectory(const std::wstring &path);
void logMessage(LogLevel level, const std::string &message);
void logMessage(LogLevel level, const std::wstring &message);
#else
inline void SetLoggingEnabled(bool) {}
inline bool IsLoggingOn() { return false; }
inline void SetLogDirectory(const std::wstring &) {}
inline void logMessage(LogLevel, const std::string &) {}
inline void logMessage(LogLevel, const std::wstring &) {}
#endif

inline std::string formatHex(std::uint64_t value) {
    char buf[24] = {};
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
    return buf;
}

} // namespace krkrspeed

#if KRKR_ENABLE_LOGGING
// The message expression is only evaluated when logging is on, so building strings costs nothing otherwise.
#define KRKR_LOG_AT(level, msg)                                                                                        \
    do {                                                                                                               \
        if (::krkrspeed::IsLoggingOn()) {                                                                              \
            ::krkrspeed::logMessage((level), (msg));                                                                   \
        }                                                                                                              \
    } while (0)
#define KRKR_LOG_DEBUG(msg) KRKR_LOG_AT(::krkrspeed::LogLevel::Debug, msg)
#define KRKR_LOG_INFO(msg) KRKR_LOG_AT(::krkrspeed::LogLevel::Info, msg)
#define KRKR_LOG_WARN(msg) KRKR_LOG_AT(::krkrspeed::LogLevel::Warn, msg)
#define KRKR_LOG_ERROR(msg) KRKR_LOG_AT(::krkrspeed::LogLevel::Error, msg)
#else
#define KRKR_LOG_DEBUG(msg) ((void)0)
#define KRKR_LOG_INFO(msg) ((void)0)
#define KRKR_LOG_WARN(msg) ((void)0)
#define KRKR_LOG_ERROR(msg) ((void)0)
#endif
