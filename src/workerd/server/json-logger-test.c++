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

class OutputCapture {
 public:
  OutputCapture(int fd): targetFd(fd), originalFd(dup(fd)) {
    auto pipe = makePipeFds();
    KJ_SYSCALL(dup2(pipe.input.get(), targetFd));
    readFd = kj::mv(pipe.output);
  }

  ~OutputCapture() {
    KJ_SYSCALL(dup2(originalFd, targetFd));
    close(originalFd);
  }

  kj::String readOutput() {
    fflush(targetFd == STDOUT_FILENO ? stdout : stderr);

    char buffer[4096];
    ssize_t n;
    KJ_SYSCALL(n = read(readFd.get(), buffer, sizeof(buffer) - 1));
    KJ_ASSERT(n >= 0);
    buffer[n] = '\0';

    return kj::str(buffer, n);
  }

 private:
  int targetFd;
  int originalFd;
  kj::AutoCloseFd readFd;
};

kj::Maybe<kj::String> findJsonEntryContaining(kj::StringPtr output, kj::StringPtr searchText) {
  size_t start = 0;

  while (start < output.size()) {
    KJ_IF_SOME(nlPos, output.slice(start).findFirst('\n')) {
      auto line = output.slice(start, start + nlPos);
      kj::String lineStr = kj::str(line);
      if (lineStr.contains(searchText)) {
        return kj::mv(lineStr);
      }
      start = start + nlPos + 1;
    } else {
      auto line = output.slice(start);
      kj::String lineStr = kj::str(line);
      if (lineStr.contains(searchText)) {
        return kj::mv(lineStr);
      }
      break;
    }
  }

  return kj::none;
}

void validateJsonLogEntry(kj::StringPtr jsonString,
    log_schema::LogEntry::LogLevel expectedLevel,
    kj::StringPtr expectedMessage) {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<log_schema::LogEntry>();

  capnp::MallocMessageBuilder message;
  auto logEntry = message.initRoot<log_schema::LogEntry>();
  codec.decode(jsonString, logEntry);

  KJ_EXPECT(logEntry.getLevel() == expectedLevel);
  KJ_EXPECT(logEntry.getMessage() == expectedMessage);
  KJ_EXPECT(logEntry.getTimestamp() > 0);
}

KJ_TEST("JsonLogger stdout validation") {
  JsonLogger logger;
  OutputCapture capture(STDOUT_FILENO);

  KJ_LOG(ERROR, "Test JSON message");

  auto output = capture.readOutput();
  auto jsonEntry = KJ_ASSERT_NONNULL(findJsonEntryContaining(output, "Test JSON message"));
  validateJsonLogEntry(jsonEntry, log_schema::LogEntry::LogLevel::ERROR, "Test JSON message");

  capnp::JsonCodec codec;
  codec.handleByAnnotation<log_schema::LogEntry>();
  capnp::MallocMessageBuilder message;
  auto logEntry = message.initRoot<log_schema::LogEntry>();
  codec.decode(jsonEntry, logEntry);

  auto source = logEntry.getSource();
  KJ_EXPECT(kj::StringPtr(source.begin(), source.size()).contains("json-logger-test.c++"));
}

KJ_TEST("StructuredLoggingProcessContext - plain text mode by default") {
  StructuredLoggingProcessContext context("test-program");

  KJ_EXPECT(context.getProgramName() == "test-program");

  OutputCapture capture(STDERR_FILENO);
  context.warning("Test warning message");

  auto output = capture.readOutput();

  KJ_EXPECT(output.contains("Test warning message"));
  KJ_EXPECT(!output.contains("{"));
}

KJ_TEST("StructuredLoggingProcessContext - structured logging mode") {
  StructuredLoggingProcessContext context("test-program");
  context.enableStructuredLogging();

  OutputCapture capture(STDERR_FILENO);
  context.warning("Test structured warning");

  auto output = capture.readOutput();
  auto jsonEntry = KJ_ASSERT_NONNULL(findJsonEntryContaining(output, "Test structured warning"));
  validateJsonLogEntry(
      jsonEntry, log_schema::LogEntry::LogLevel::WARNING, "Test structured warning");
}

KJ_TEST("StructuredLoggingProcessContext - error handling in structured mode") {
  StructuredLoggingProcessContext context("test-program");
  context.enableStructuredLogging();

  OutputCapture capture(STDERR_FILENO);
  context.error("Test structured error");

  auto output = capture.readOutput();
  auto jsonEntry = KJ_ASSERT_NONNULL(findJsonEntryContaining(output, "Test structured error"));
  validateJsonLogEntry(jsonEntry, log_schema::LogEntry::LogLevel::ERROR, "Test structured error");
}

#endif  // __linux__

KJ_TEST("Blank test because KJ fails when 0 tests are enabled") {}

}  // namespace
}  // namespace workerd::server
