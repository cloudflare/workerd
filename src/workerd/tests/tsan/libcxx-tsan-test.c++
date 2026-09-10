// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/debug.h>
#include <kj/test.h>

#include <charconv>
#include <cstdlib>
#include <thread>

// TSan invokes this hook before applying halt_on_error. Reaching it proves that the concurrent
// writes performed by libc++'s out-of-line floating-point formatter were instrumented.
extern "C" void __tsan_on_report(void*) {
  std::_Exit(EXIT_SUCCESS);
}

KJ_TEST("libc++ is instrumented by ThreadSanitizer") {
  char buffer[64];

  auto format = [&]() {
    for (auto i = 0; i < 10000; ++i) {
      std::to_chars(buffer, buffer + sizeof(buffer), 1.23456789);
    }
  };

  std::thread first(format);
  std::thread second(format);
  first.join();
  second.join();

  KJ_FAIL_ASSERT("ThreadSanitizer did not detect libc++'s concurrent writes");
}
