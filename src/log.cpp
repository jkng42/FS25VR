#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <mutex>

static FILE* g_file = nullptr;
static std::mutex g_mutex;
static std::wstring g_dir;

void LogInit(const std::wstring& dir)
{
    g_dir = dir;
    g_file = _wfopen((dir + L"fs25vr.log").c_str(), L"w");
}

const std::wstring& ModuleDir() { return g_dir; }

void Log(const char* fmt, ...)
{
    if (!g_file) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lock(g_mutex);
    fprintf(g_file, "[%02d:%02d:%02d.%03d][%5lu] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId(), buf);
    fflush(g_file);
}
