// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "json-logger.h"

#include <workerd/server/log-schema.capnp.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/debug.h>
#include <kj/io.h>
#include <kj/main.h>
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

void JsonLogger::logMessage(
    kj::LogSeverity severity, const char* file, int line, int contextDepth, kj::String&& text) {
  // Prevent infinite recursion if logging code itself logs
  if (loggingInProgress) {
    return;
  }
  loggingInProgress = true;
  KJ_DEFER(loggingInProgress = false);

  auto json = buildJsonLogMessage(severity, file, line, contextDepth, text);

  // Write directly to stdout with no buffering.
  kj::FdOutputStream(STDOUT_FILENO).write({json.asBytes(), "\n"_kj.asBytes()});
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

// =======================================================================================
// StructuredLoggingProcessContext implementation

StructuredLoggingProcessContext::StructuredLoggingProcessContext(kj::StringPtr programName)
    : topLevelContext(programName) {}

void StructuredLoggingProcessContext::enableStructuredLogging() {
  useStructuredLogging = true;
  jsonLogger.emplace();
}

kj::StringPtr StructuredLoggingProcessContext::getProgramName() {
  return topLevelContext.getProgramName();
}

void StructuredLoggingProcessContext::exit() {
  topLevelContext.exit();
}

void StructuredLoggingProcessContext::warning(kj::StringPtr message) const {
  if (useStructuredLogging) {
    auto json = buildJsonLogMessage(kj::LogSeverity::WARNING, __FILE__, __LINE__, 0, message);
    topLevelContext.warning(json);
  } else {
    topLevelContext.warning(message);
  }
}

void StructuredLoggingProcessContext::error(kj::StringPtr message) const {
  if (useStructuredLogging) {
    auto json = buildJsonLogMessage(kj::LogSeverity::ERROR, __FILE__, __LINE__, 0, message);
    topLevelContext.error(json);
  } else {
    topLevelContext.error(message);
  }
}

void StructuredLoggingProcessContext::exitError(kj::StringPtr message) {
  if (useStructuredLogging) {
    auto json = buildJsonLogMessage(kj::LogSeverity::ERROR, __FILE__, __LINE__, 0, message);
    topLevelContext.exitError(json);
  } else {
    topLevelContext.exitError(message);
  }
}

void StructuredLoggingProcessContext::exitInfo(kj::StringPtr message) {
  if (useStructuredLogging) {
    auto json = buildJsonLogMessage(kj::LogSeverity::INFO, __FILE__, __LINE__, 0, message);
    topLevelContext.exitInfo(json);
  } else {
    topLevelContext.exitInfo(message);
  }
}

void StructuredLoggingProcessContext::increaseLoggingVerbosity() {
  topLevelContext.increaseLoggingVerbosity();
}

}  // namespace workerd::server
