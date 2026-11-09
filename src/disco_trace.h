#pragma once

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

// Opt-in trace of the delivery module's libp2p_module traffic to the file
// named by LD_DISCO_TRACE: logos-core discards a module's own stdout/stderr,
// so there is no other view. One sink per program.
namespace disco_trace {

inline std::atomic<FILE*>& sinkOverride()
{
    static std::atomic<FILE*> sink{nullptr};
    return sink;
}

// While set, the trace goes to `sink` instead of the env-named file; nullptr
// restores the env-named file.
inline void setTraceSinkForTest(FILE* sink)
{
    sinkOverride().store(sink);
}

inline FILE* traceFile()
{
    if (FILE* s = sinkOverride().load()) {
        return s;
    }
    static FILE* f = [] () -> FILE* {
        const char* path = getenv("LD_DISCO_TRACE");
        if (!path || !*path) {
            return nullptr;
        }
        FILE* h = fopen(path, "a");
        if (h) {
            setvbuf(h, nullptr, _IOLBF, 0); // line-buffered, so `tail -f` works
        }
        return h;
    }();
    return f;
}

inline void trace(const char* fmt, ...)
{
    FILE* f = traceFile();
    if (!f) {
        return;
    }
    char stamp[32] = "";
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    // MinGW declares localtime_r only under _POSIX_C_SOURCE; localtime_s is
    // the same call with the arguments swapped, returning 0 on success.
    if (localtime_s(&tm, &now) == 0) {
#else
    if (localtime_r(&now, &tm)) {
#endif
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    }
    fprintf(f, "[%s] ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
}

inline void traceLine(const std::string& line)
{
    trace("%s", line.c_str());
}

} // namespace disco_trace
