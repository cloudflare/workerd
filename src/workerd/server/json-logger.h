// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/exception.h>
#include <kj/function.h>
#include <kj/main.h>
#include <kj/string.h>

namespace workerd::server {

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

class StructuredLoggingProcessContext final: public kj::ProcessContext {
  // A ProcessContext implementation that supports both plain text and structured JSON logging.
  // This context wraps TopLevelProcessContext and adds the ability to emit log messages in
  // JSON format when structured logging is enabled.

 public:
  explicit StructuredLoggingProcessContext(kj::StringPtr programName);

  // Enable structured JSON logging. This can only be called once and cannot be reversed.
  // When enabled: Log messages are formatted as JSON and sent to stdout or stderr
  //               This also enables an ExceptionCallback to replace KJ_LOGs with structured logs.
  //               To reduce code duplication from TopLevelProcessContext, while JsonLogger sends
  //               all logs to stdout, StructuredLoggingProcessContext sends all to the fd that
  //               TopLevelProcessContext would have sent to.
  // When disabled: Log messages are sent as plain text to stdout or stderr (like
  //                TopLevelProcessContext)
  void enableStructuredLogging();

  kj::StringPtr getProgramName() override;
  KJ_NORETURN(void exit() override);
  void warning(kj::StringPtr message) const override;
  void error(kj::StringPtr message) const override;
  KJ_NORETURN(void exitError(kj::StringPtr message) override);
  KJ_NORETURN(void exitInfo(kj::StringPtr message) override);
  void increaseLoggingVerbosity() override;

 private:
  kj::TopLevelProcessContext topLevelContext;
  kj::Maybe<JsonLogger> jsonLogger;
  bool useStructuredLogging = false;
};

}  // namespace workerd::server
