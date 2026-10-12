#pragma once

// Captures every log line so tests can assert on what would reach the serial console.
#include <cstdarg>
#include <cstdio>
#include <string>

inline std::string capturedLog;

inline void captureLog(const char* format, ...) {
  char line[512];
  va_list args;
  va_start(args, format);
  vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  capturedLog += line;
  capturedLog += '\n';
}

#define LOG_DBG(tag, ...) captureLog(__VA_ARGS__)
#define LOG_INF(tag, ...) captureLog(__VA_ARGS__)
#define LOG_ERR(tag, ...) captureLog(__VA_ARGS__)
