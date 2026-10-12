// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ half of workerd's command line: the functions the Rust entry point (cli/lib.rs) calls
// through the cxx bridges in cli/bridge.rs (the build's capabilities, the Pyodide lock) and
// server/bridge.rs (the process's logging, the bootstrap, the exit). Rust parses the command
// line, produces the encoded config (config-compiler.h compiles schema files), owns the event loop
// (kj_rs_tokio::Runtime) and runs the server (server/entry.rs); the bootstrap sets up the process
// and V8 around it and builds the worker factory.
//
// Do not include a generated bridge header here: both bridges include this header.

#include <rust/cxx.h>

#include <kj/memory.h>

namespace kj_rs_tokio {
struct TokioAsyncIoContext;
}  // namespace kj_rs_tokio

namespace workerd::server {
class WorkerFactory;
struct PendingCommand;
}  // namespace workerd::server

namespace workerd::server::cli {

struct ServeOrTestOptions;
struct TestOptions;

::rust::Slice<const uint8_t> release_version();
bool perfetto_supported();
bool fuzzilli_supported();
::rust::String pyodide_lock();

// See the bridge (server/bridge.rs).
int32_t with_process_context(
    bool verbose, ::rust::Vec<uint64_t> config, ::rust::Box<PendingCommand> command);
kj::Own<WorkerFactory> bootstrap(kj_rs_tokio::TokioAsyncIoContext& loop,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& options,
    kj::Maybe<const TestOptions&> test);
[[noreturn]] void cli_exit(int32_t code);
void kj_log(uint8_t severity, ::rust::Str file, uint32_t line, ::rust::Str message);
void json_log_to_stderr(uint8_t severity, ::rust::Str file, uint32_t line, ::rust::Str message);

}  // namespace workerd::server::cli
