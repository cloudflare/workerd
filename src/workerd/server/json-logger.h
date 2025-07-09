// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/exception.h>
#include <kj/function.h>
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

}  // namespace workerd::server
