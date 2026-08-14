#pragma once

// 极简日志（本仓库简化版，替代 NebulaX 的 MPSC 异步日志）：
// 原版依赖 mpsc_ring + eventfd + 消费线程，属网络/性能基础设施，仿真不需要。
// 仅保留 LOG_* 宏（签名兼容），同步写 stderr。单线程仿真下无并发问题。

#include <cstdarg>
#include <cstdio>

namespace simlog {
enum class Level { FATAL = 0, ERROR = 1, WARN = 2, INFO = 3 };

inline void log(Level lvl, const char* fmt, ...) {
    static const char* names[] = {"FATAL", "ERROR", "WARN ", "INFO "};
    std::fprintf(stderr, "[%s] ", names[static_cast<int>(lvl)]);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}
}  // namespace simlog

#define LOG_FATAL(...) ::simlog::log(::simlog::Level::FATAL, __VA_ARGS__)
#define LOG_ERROR(...) ::simlog::log(::simlog::Level::ERROR, __VA_ARGS__)
#define LOG_WARN(...)  ::simlog::log(::simlog::Level::WARN, __VA_ARGS__)
#define LOG_INFO(...)  ::simlog::log(::simlog::Level::INFO, __VA_ARGS__)
