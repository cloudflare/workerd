// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#if __linux__
#include "json-logger.h"

#include <workerd/server/log-schema.capnp.h>

#include <fcntl.h>
#include <unistd.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/async-unix.h>
#include <kj/io.h>

#include <cstdio>
#endif  // __linux__
#include <kj/test.h>

namespace workerd::server {
namespace {
#if __linux__
// This test uses pipe2 and dup2 to capture stdout which is far easier on linux.

struct FdPair {
  kj::AutoCloseFd output;
  kj::AutoCloseFd input;
};

auto makePipeFds() {
  int pipeFds[2];
  KJ_SYSCALL(pipe2(pipeFds, O_CLOEXEC));

  return FdPair{
    .output = kj::AutoCloseFd(pipeFds[0]),
    .input = kj::AutoCloseFd(pipeFds[1]),
  };
}

KJ_TEST("JsonLogger stdout validation") {
  JsonLogger logger;

  // Set up pipes to intercept stdout
  auto interceptorPipe = makePipeFds();
  int originalStdout = dup(STDOUT_FILENO);
  KJ_SYSCALL(dup2(interceptorPipe.input.get(), STDOUT_FILENO));
  interceptorPipe.input = nullptr;
  KJ_DEFER({
    // Restore stdout
    KJ_SYSCALL(dup2(originalStdout, STDOUT_FILENO));
    close(originalStdout);
  });

  // Log a test message
  KJ_LOG(ERROR, "Test JSON message");

  fflush(stdout);
  // Read the JSON output
  char buffer[4096];
  ssize_t n;
  KJ_SYSCALL(n = read(interceptorPipe.output.get(), buffer, sizeof(buffer) - 1));

  // Should end with newline
  KJ_ASSERT(buffer[--n] == '\n');
  buffer[n] = '\0';

  kj::StringPtr jsonOutput(buffer, n);

  // Parse and validate the JSON using Cap'n Proto codec
  capnp::JsonCodec codec;
  codec.handleByAnnotation<log_schema::LogEntry>();

  capnp::MallocMessageBuilder message;
  auto logEntry = message.initRoot<log_schema::LogEntry>();
  codec.decode(jsonOutput, logEntry);

  // Validate the parsed JSON structure
  KJ_EXPECT(logEntry.getLevel() == log_schema::LogEntry::LogLevel::ERROR);
  KJ_EXPECT(logEntry.getMessage() == "Test JSON message");
  KJ_EXPECT(logEntry.getTimestamp() > 0);
  auto source = logEntry.getSource();
  KJ_EXPECT(kj::StringPtr(source.begin(), source.size()).endsWith("json-logger-test.c++:49"));
}
#endif  // __linux__

KJ_TEST("Blank test because KJ fails when 0 tests are enabled") {}

}  // namespace
}  // namespace workerd::server
