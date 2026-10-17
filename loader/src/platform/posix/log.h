// Logging for the Linux loader: one file, and the console when it is worth looking at.
//
// The Windows loader writes through log.cpp with two entry points -- logf for detail, logc for the
// milestones a person watching the boot wants to see -- and this is the same split, because "the boot
// printed 400 lines" tells you nothing while "the boot printed 12 lines and then stopped" does.
//
// Threads: the boot thread logs from inside game code, so every line is taken under one mutex. Two
// processes appending to the same log would interleave mid-line; nothing here does that, and the port
// runs one image per process.
#pragma once

#include <stdarg.h>

// Open the log. An empty path means console only (or file-only when trace is off and the path is bad --
// see below). A path that cannot be opened is reported once on stderr and logging continues without a
// file, because a missing log is not a reason to refuse to boot.
void log_init(const char *path, bool trace);
void log_close();

// Detail: the log file always, the console only when tracing.
void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Milestone: log file and console. These are the lines that describe the boot to someone reading it.
void logc(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

bool log_trace();
void log_set_trace(bool on);

// Where the log went, for the "--log" argument's own report. Empty when there is no file.
const char *log_path();