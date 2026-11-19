// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ half of workerd's command line: the functions the Rust entry point (cli/lib.rs) calls
// through the cxx bridges in cli/bridge.rs (the build's capabilities) and
// server/bridge.rs (the process's logging, the bootstrap, the exit). Rust parses the command
// line, produces the encoded config (config-compiler.h compiles schema files), owns the event loops
// (kj_rs_tokio::Runtime, or kj_rs_tokio::LoopUnit) and runs the server (server/entry.rs); the
// bootstrap sets up the process
// and V8 around it once (`bootstrap_process`) and builds a worker factory on each loop thread's
// loop (`bootstrap_loop`; `bootstrap` does both for the first), or on each loop unit's
// (`bootstrap_unit`) when the loops are tasks of one tokio runtime (server/units.rs).
//
// Do not include a generated bridge header here: both bridges include this header.

#include <rust/cxx.h>

#include <kj/memory.h>
#include <kj/refcount.h>

namespace kj_rs_tokio {
struct TokioAsyncIoContext;
class LoopUnitContext;
}  // namespace kj_rs_tokio

namespace workerd::server {
class SharedFactory;
class WorkerFactory;
struct PendingCommand;
struct LoopThread;
struct LoopEntry;
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
void with_loop_thread_context(const SharedFactory& shared, ::rust::Box<LoopThread> thread);
kj::Arc<SharedFactory> bootstrap_process(::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& options,
    kj::Maybe<const TestOptions&> test,
    kj::Maybe<const BenchOptions&> bench);
kj::Own<WorkerFactory> bootstrap_loop(
    const SharedFactory& shared, kj_rs_tokio::TokioAsyncIoContext& loop);
bool structured_logging(const SharedFactory& shared);
void with_loop_entry_context(bool structured, LoopEntry& entry);
kj::Own<WorkerFactory> bootstrap_unit(const SharedFactory& shared,
    kj_rs_tokio::LoopUnitContext& unit,
    bool primary,
    uint32_t isolateCopy);
kj::Own<WorkerFactory> bootstrap(kj_rs_tokio::TokioAsyncIoContext& loop,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& options,
    kj::Maybe<const TestOptions&> test,
    kj::Maybe<const BenchOptions&> bench);
BuildInfo build_info();
::rust::String newest_compat_date();
[[noreturn]] void cli_exit(int32_t code);
void kj_log(uint8_t severity, ::rust::Str file, uint32_t line, ::rust::Str message);
void json_log_to_stderr(uint8_t severity, ::rust::Str file, uint32_t line, ::rust::Str message);

}  // namespace workerd::server::cli
