// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "json-logger.h"

#include <workerd/server/log-schema.capnp.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/io.h>
#include <kj/miniposix.h>
#include <kj/string.h>

namespace workerd::server {

log_schema::LogEntry::LogLevel severityToLogLevel(kj::LogSeverity severity) {
  switch (severity) {
    case kj::LogSeverity::INFO:
      return log_schema::LogEntry::LogLevel::INFO;
    case kj::LogSeverity::WARNING:
      return log_schema::LogEntry::LogLevel::WARNING;
    case kj::LogSeverity::ERROR:
      return log_schema::LogEntry::LogLevel::ERROR;
    case kj::LogSeverity::FATAL:
      return log_schema::LogEntry::LogLevel::FATAL;
    case kj::LogSeverity::DBG:
      return log_schema::LogEntry::LogLevel::DEBUG_;
  }
}

kj::String buildJsonLogMessage(
    kj::LogSeverity severity, const char* file, int line, int contextDepth, kj::StringPtr text) {
  capnp::MallocMessageBuilder message;
  auto logEntry = message.initRoot<log_schema::LogEntry>();

  logEntry.setTimestamp(
      (kj::systemPreciseCalendarClock().now() - kj::UNIX_EPOCH) / kj::MILLISECONDS);

  logEntry.setLevel(severityToLogLevel(severity));

  auto location = kj::str(file, ":", line);
  logEntry.setSource(location);

  logEntry.setMessage(text);

  if (contextDepth > 0) {
    logEntry.setContextDepth(static_cast<uint32_t>(contextDepth));
  }

  capnp::JsonCodec codec;
  codec.handleByAnnotation<log_schema::LogEntry>();
  codec.setPrettyPrint(false);  // Compact JSON for logs
  return codec.encode(logEntry);
}

void writeJsonLogLine(FILE* stream, kj::StringPtr json) {
  // A record larger than PIPE_BUF can reach a pipe in several write() calls; the lock keeps
  // every other writer of the stream out between them. It is recursive, so a log emitted while
  // this thread holds it (an exception logged from inside the write) does not deadlock.
#if _WIN32
  _lock_file(stream);
  KJ_DEFER(_unlock_file(stream));
  int fd = _fileno(stream);
#else
  flockfile(stream);
  KJ_DEFER(funlockfile(stream));
  int fd = fileno(stream);
#endif
  // A line written through the stream may be partly in its buffer and partly on the descriptor.
  fflush(stream);
  kj::FdOutputStream(fd).write({json.asBytes(), "\n"_kj.asBytes()});
}

void JsonLogger::logMessage(
    kj::LogSeverity severity, const char* file, int line, int contextDepth, kj::String&& text) {
  // Prevent infinite recursion if logging code itself logs
  if (loggingInProgress) {
    return;
  }
  loggingInProgress = true;
  KJ_DEFER(loggingInProgress = false);

  auto json = buildJsonLogMessage(severity, file, line, contextDepth, text);
  writeJsonLogLine(stdout, json);
}

kj::Function<void(kj::Function<void()>)> JsonLogger::getThreadInitializer() {
  auto nextInit = next.getThreadInitializer();

  return [nextInit = kj::mv(nextInit)](kj::Function<void()> func) mutable {
    nextInit([&]() {
      JsonLogger logger;

      // Make sure func is destroyed before the context is destroyed.
      auto ownFunc = kj::mv(func);
      ownFunc();
    });
  };
}

}  // namespace workerd::server
