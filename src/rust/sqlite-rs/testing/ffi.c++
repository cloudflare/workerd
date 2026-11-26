// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/util/sqlite-metering.h>

#include <kj/exception.h>
#include <kj/string.h>

namespace workerd::rust::sqlite_rs::testing {

namespace {

// Records what KJ logs on this thread while it lives, as kj::_::LogExpectation does for
// KJ_EXPECT_LOG.
class LogCapture final: public kj::ExceptionCallback {
 public:
  void logMessage(kj::LogSeverity severity,
      const char* file,
      int line,
      int contextDepth,
      kj::String&& text) override {
    auto entry = kj::str(severity, ": ", text);
    messages.push_back(::rust::String::lossy(entry.begin(), entry.size()));
    next.logMessage(severity, file, line, contextDepth, kj::mv(text));
  }

  ::rust::Vec<::rust::String> messages;
};

}  // namespace

::rust::Vec<::rust::String> capture_kj_logs(size_t data, ::rust::Fn<void(size_t)> callback) {
  LogCapture capture;
  callback(data);
  return kj::mv(capture.messages);
}

void install_sqlite_custom_allocator() {
  installSqliteCustomAllocator();
}

}  // namespace workerd::rust::sqlite_rs::testing
