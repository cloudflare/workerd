// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ half of workerd's command line: the functions the Rust entry point (cli/lib.rs) calls
// through the cxx bridges in cli/bridge.rs (the build's capabilities) and server/bridge.rs (the
// process's logging, the process, the factory, the exit). Rust parses the command line, produces
// the encoded config (config-compiler.h compiles schema files), owns the event loop
// (kj_rs_tokio::Runtime) and runs the server (server/entry.rs); `process_start` sets up the process
// and V8 around it (per-process.h) and `factory_new` builds the worker factory.
//
// Do not include a generated bridge header here: both bridges include this header.

#include <rust/cxx.h>

#include <kj/function.h>
#include <kj/memory.h>
#include <kj/refcount.h>

namespace kj_rs_tokio {
struct TokioAsyncIoContext;
}  // namespace kj_rs_tokio

namespace workerd::server {
class ProcessState;
class WorkerFactory;
struct PendingCommand;
struct ThreadBody;
}  // namespace workerd::server

namespace workerd::server::cli {

struct ServeOrTestOptions;
struct TestOptions;
struct BenchOptions;
struct BuildInfo;

::rust::Slice<const uint8_t> release_version();
bool perfetto_supported();
bool fuzzilli_supported();

// See the bridge (server/bridge.rs).
int32_t with_process_context(
    bool verbose, ::rust::Vec<uint64_t> config, ::rust::Box<PendingCommand> command);
kj::Arc<ProcessState> process_start(::rust::Slice<const uint64_t> config,
    const ServeOrTestOptions& options,
    kj::Maybe<const TestOptions&> test,
    kj::Maybe<const BenchOptions&> bench);
kj::Own<WorkerFactory> factory_new(const ProcessState& process,
    kj_rs_tokio::TokioAsyncIoContext& loop,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& options,
    kj::Maybe<const TestOptions&> test,
    kj::Maybe<const BenchOptions&> bench);
BuildInfo build_info();
::rust::String newest_compat_date();
[[noreturn]] void cli_exit(int32_t code);
void kj_log(uint8_t severity, ::rust::Str file, uint32_t line, ::rust::Str message);
void json_log_to_stderr(uint8_t severity, ::rust::Str file, uint32_t line, ::rust::Str message);
void write_stderr_nonblocking(::rust::Str text);

struct ThreadLogger {
  kj::Function<void(kj::Function<void()>)> initializer;
};
kj::Own<ThreadLogger> thread_logger();
void run_with_logger(kj::Own<ThreadLogger> logger, ::rust::Box<ThreadBody> body);

}  // namespace workerd::server::cli
