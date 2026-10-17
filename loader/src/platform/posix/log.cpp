#include "platform/posix/log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

namespace {

FILE *g_file;
bool g_trace;
char g_path[512];
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void timestamp(char *buf, size_t n)
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tm_buf;
    localtime_r(&tv.tv_sec, &tm_buf);
    snprintf(buf, n, "%02d:%02d:%02d.%03d", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             (int)(tv.tv_usec / 1000));
}

void emit(const char *fmt, va_list ap, bool console)
{
    char stamp[32];
    timestamp(stamp, sizeof stamp);

    pthread_mutex_lock(&g_lock);
    if (g_file) {
        fprintf(g_file, "[%s] ", stamp);
        vfprintf(g_file, fmt, ap);
        fputc('\n', g_file);
        fflush(g_file); // a crash must not take the interesting lines with it
    }
    if (console || !g_file) {
        fprintf(stderr, "[%s] ", stamp);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
    }
    pthread_mutex_unlock(&g_lock);
}

} // namespace

void log_init(const char *path, bool trace)
{
    g_trace = trace;
    g_path[0] = 0;
    if (path && path[0]) {
        FILE *f = fopen(path, "w");
        if (f) {
            g_file = f;
            snprintf(g_path, sizeof g_path, "%s", path);
        } else {
            fprintf(stderr, "log: cannot write %s, continuing without a log file\n", path);
        }
    }
}

void log_close()
{
    pthread_mutex_lock(&g_lock);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
    pthread_mutex_unlock(&g_lock);
}

void logf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(fmt, ap, g_trace);
    va_end(ap);
}

void logc(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(fmt, ap, true);
    va_end(ap);
}

bool log_trace() { return g_trace; }

void log_set_trace(bool on) { g_trace = on; }

const char *log_path() { return g_path[0] ? g_path : nullptr; }