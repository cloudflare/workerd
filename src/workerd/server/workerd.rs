// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `workerd` binary. See cli/lib.rs.

fn main() -> std::process::ExitCode {
    workerd_cli::main()
}
