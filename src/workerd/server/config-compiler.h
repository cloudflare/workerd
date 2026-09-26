// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Compiles a workerd config written as a Cap'n Proto schema file (`import`, `embed`, typed
// constants) into an encoded message, for the Rust entry point (cli/lib.rs) through the cxx bridge
// in cli/bridge.rs. capnp::SchemaParser is the only compiler for that format, so this stays C++.
//
// Do not include workerd/server/cli/bridge.rs.h here: it includes this header.

#include <kj-rs/kj-rs.h>
#include <rust/cxx.h>

namespace workerd::server::cli {

struct CompiledConfig;
struct Process;

CompiledConfig compile_config(::rust::Str path,
    ::rust::Slice<const ::rust::String> importPaths,
    kj::Maybe<::rust::Str> constName,
    const Process& process);

}  // namespace workerd::server::cli
