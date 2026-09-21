// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ half of workerd's command line: the functions the Rust entry point (cli/lib.rs) calls
// through the cxx bridge in cli/bridge.rs. Rust parses the command line and produces the encoded config
// (config-compiler.h compiles schema files); these set up V8 and the Server and run it.
//
// Do not include workerd/server/cli/bridge.rs.h here: it includes this header.

#include <rust/cxx.h>

namespace workerd::server::cli {

struct CommonOptions;
struct ServeOrTestOptions;
struct ServeOptions;
struct TestOptions;
struct Process;

::rust::Slice<const uint8_t> release_version();
bool perfetto_supported();
bool fuzzilli_supported();
::rust::String pyodide_lock();

int32_t run_serve(const CommonOptions& common,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& serveOrTest,
    const ServeOptions& serve,
    ::rust::Box<Process> process);
int32_t run_test(const CommonOptions& common,
    ::rust::Vec<uint64_t> config,
    const ServeOrTestOptions& serveOrTest,
    const TestOptions& test,
    ::rust::Box<Process> process);

}  // namespace workerd::server::cli
