# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0x9f8a7b6c5d4e3f2a;
using Cxx = import "/capnp/c++.capnp";
using Json = import "/capnp/compat/json.capnp";

$Cxx.namespace("workerd::server::log_schema");
$Cxx.allowCancellation;

struct LogEntry {
  # Structured log entry for workerd JSON logging

  timestamp @0 :UInt64 $Json.name("timestamp");
  # Unix timestamp in milliseconds when the log was generated

  level @1 :LogLevel $Json.name("level");
  # Severity level of the log message

  source @2 :Text $Json.name("source");
  # Source file and line number where the log originated (e.g., "server.c++:123")

  message @3 :Text $Json.name("message");
  # The actual log message content

  contextDepth @4 :UInt32 $Json.name("context_depth");
  # Context depth for nested operations (optional, only included if > 0)

  enum LogLevel {
    debug @0 $Json.name("debug");
    info @1 $Json.name("info");
    warning @2 $Json.name("warning");
    error @3 $Json.name("error");
    fatal @4 $Json.name("fatal");
  }
}
