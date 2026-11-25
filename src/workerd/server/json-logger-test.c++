// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "json-logger.h"

#include <workerd/server/log-schema.capnp.h>

#if __linux__
#include <fcntl.h>
#include <unistd.h>

#include <kj/io.h>
#include <kj/thread.h>
#include <kj/vector.h>

#include <cstdio>
#endif  // __linux__
#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::server {
namespace {

// The level names are a contract with consumers of workerd's structured output, so pin the encoded
// strings rather than round-tripping them through the codec that produced them.
KJ_TEST("JSON log level names") {
  capnp::JsonCodec codec;
  codec.handleByAnnotation<log_schema::LogEntry>();

  auto expectLevelName = [&](log_schema::LogEntry::LogLevel level, kj::StringPtr expected) {
    capnp::MallocMessageBuilder message;
    auto entry = message.initRoot<log_schema::LogEntry>();
    entry.setLevel(level);
    auto json = codec.encode(entry);
    KJ_EXPECT(json.contains(kj::str("\"level\":\"", expected, "\"")), json, expected);
  };

  expectLevelName(log_schema::LogEntry::LogLevel::DEBUG_, "debug");
  expectLevelName(log_schema::LogEntry::LogLevel::INFO, "info");
  expectLevelName(log_schema::LogEntry::LogLevel::WARNING, "warn");
  expectLevelName(log_schema::LogEntry::LogLevel::ERROR, "error");
  expectLevelName(log_schema::LogEntry::LogLevel::FATAL, "fatal");
}

#if __linux__
// This test uses pipe2 and dup2 to capture stdout which is far easier on linux.

struct FdPair {
  kj::AutoCloseFd output;
  kj::AutoCloseFd input;
};

auto makePipeFds() {
  int pipeFds[2]{};
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

    char buffer[4096]{};
    ssize_t n;
    KJ_SYSCALL(n = read(readFd.get(), buffer, sizeof(buffer) - 1));
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

// Records larger than a pipe's capacity reach it in several write() calls. Loop threads each log
// through a JsonLogger of their own while a Worker's console writes its lines through stdio; every
// line the reader sees must be one whole record.
KJ_TEST("JsonLogger records from several threads do not interleave") {
  constexpr size_t RECORD_SIZE = 96 * 1024;
  constexpr unsigned int RECORDS_PER_THREAD = 8;
  constexpr unsigned int LOGGER_THREADS = 4;

  fflush(stdout);
  int originalFd;
  KJ_SYSCALL(originalFd = dup(STDOUT_FILENO));
  KJ_DEFER(close(originalFd));
  auto pipe = makePipeFds();
  KJ_SYSCALL(dup2(pipe.input.get(), STDOUT_FILENO));
  pipe.input = nullptr;

  // Reads slowly, so writers find the pipe full and resume one at a time.
  kj::Vector<char> output;
  auto reader = kj::heap<kj::Thread>([&output, fd = pipe.output.get()]() {
    char buffer[4096]{};
    for (;;) {
      ssize_t n;
      KJ_SYSCALL(n = read(fd, buffer, sizeof(buffer)));
      if (n == 0) break;
      output.addAll(kj::arrayPtr(buffer, n));
      usleep(50);
    }
  });

  {
    kj::Vector<kj::Own<kj::Thread>> writers;
    for (auto i: kj::zeroTo(LOGGER_THREADS)) {
      writers.add(kj::heap<kj::Thread>([i]() {
        JsonLogger logger;
        auto text = kj::heapString(RECORD_SIZE);
        text.asArray().fill('a' + i);
        for (auto _ KJ_UNUSED: kj::zeroTo(RECORDS_PER_THREAD)) {
          KJ_LOG(ERROR, text);
        }
      }));
    }
    // What a Worker's console does under structured logging (`writeConsoleLine`, worker.c++).
    writers.add(kj::heap<kj::Thread>([]() {
      auto text = kj::heapString(RECORD_SIZE);
      text.asArray().fill('z');
      auto json = buildJsonLogMessage(kj::LogSeverity::INFO, "console", 0, 0, text);
      for (auto _ KJ_UNUSED: kj::zeroTo(RECORDS_PER_THREAD)) {
        fprintf(stdout, "%s\n", json.cStr());
        fflush(stdout);
      }
    }));
  }

  fflush(stdout);
  KJ_SYSCALL(dup2(originalFd, STDOUT_FILENO));
  reader = nullptr;

  capnp::JsonCodec codec;
  codec.handleByAnnotation<log_schema::LogEntry>();
  kj::ArrayPtr<const char> text = output.asPtr();
  unsigned int records = 0;
  size_t start = 0;
  while (start < text.size()) {
    auto nl = KJ_ASSERT_NONNULL(text.slice(start).findFirst('\n'), "unterminated line");
    auto line = text.slice(start, start + nl);
    start += nl + 1;
    capnp::MallocMessageBuilder message;
    auto entry = message.initRoot<log_schema::LogEntry>();
    codec.decode(line, entry);
    // `KJ_LOG(ERROR, text)` logs `text = ` and then the text.
    auto logText = entry.getMessage();
    KJ_ASSERT(logText.size() >= RECORD_SIZE, logText.size());
    auto logged = logText.slice(logText.size() - RECORD_SIZE);
    for (char c: logged) {
      KJ_ASSERT(c == logged[0], "a record holds another's text");
    }
    ++records;
  }
  KJ_EXPECT(records == (LOGGER_THREADS + 1) * RECORDS_PER_THREAD, records);
}

#endif  // __linux__

}  // namespace
}  // namespace workerd::server
