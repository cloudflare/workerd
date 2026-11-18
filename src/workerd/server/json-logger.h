// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/exception.h>
#include <kj/function.h>
#include <kj/string.h>

#include <cstdio>

namespace workerd::server {

// A log line as JSON (a `LogEntry` of log-schema.capnp), without the newline that ends it.
kj::String buildJsonLogMessage(
    kj::LogSeverity severity, const char* file, int line, int contextDepth, kj::StringPtr text);

// Writes `json` and a newline to the descriptor of `stream` (stdout or stderr) as one
// uninterrupted line, however large. The write holds the stream's stdio lock, which is
// process-wide and recursive, after flushing what the stream has buffered: lines other threads
// write through the stream (a Worker's console output) and lines written here never interleave.
void writeJsonLogLine(FILE* stream, kj::StringPtr json);

class JsonLogger: public kj::ExceptionCallback {
 public:
  void logMessage(kj::LogSeverity severity,
      const char* file,
      int line,
      int contextDepth,
      kj::String&& text) override;

  kj::Function<void(kj::Function<void()>)> getThreadInitializer() override;

  StackTraceMode stackTraceMode() override {
    return StackTraceMode::ADDRESS_ONLY;
  }

 private:
  bool loggingInProgress = false;
};

}  // namespace workerd::server
