// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `workerd` binary. See cli/lib.rs.

fn main() -> std::process::ExitCode {
    // Set when the binary is linked with tcmalloc, which happens only on Linux (see `:malloc`).
    #[cfg(target_os = "linux")]
    if option_env!("WORKERD_TCMALLOC").is_some() {
        workerd_cli::reexec_without_glibc_rseq();
    }
    #[cfg(target_os = "linux")]
    workerd_cli::remap_text_to_huge_pages();
    workerd_cli::main()
}
