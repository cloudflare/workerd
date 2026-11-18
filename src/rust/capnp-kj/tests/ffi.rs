// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use crate::container::new_container_server;
use crate::hang_drops;
use crate::hang_starts;
use crate::new_cancelable_echo_server;
use crate::new_echo_server;
use crate::rust_bounce_n;
use crate::rust_call_back;
use crate::rust_child_echo;
use crate::rust_echo;
use crate::rust_fail;
use crate::rust_hang;
use crate::rust_write;

#[cxx::bridge(namespace = "workerd::rust::capnp_kj::test")]
pub mod bridge {
    unsafe extern "C++" {
        include!("workerd/rust/capnp-kj/ffi.h");

        #[namespace = "capnp"]
        type ClientHook = capnp_kj::ClientHook;
    }

    extern "Rust" {
        /// A Rust `TestEcho` server that prefixes what it echoes with `prefix`.
        fn new_echo_server(prefix: &str) -> KjOwn<ClientHook>;
        /// Like `new_echo_server`, but canceling a call drops the server's future.
        fn new_cancelable_echo_server(prefix: &str) -> KjOwn<ClientHook>;
        /// A Rust `rpc::Container` server; see container.rs.
        fn new_container_server() -> KjOwn<ClientHook>;
        /// How many Rust `hang()` calls have started.
        fn hang_starts() -> u32;
        /// How many Rust `hang()` calls have been dropped.
        fn hang_drops() -> u32;

        async fn rust_echo(echo: KjOwn<ClientHook>, text: String) -> Result<String>;
        async fn rust_child_echo(
            echo: KjOwn<ClientHook>,
            prefix: String,
            text: String,
        ) -> Result<String>;
        async fn rust_call_back(
            echo: KjOwn<ClientHook>,
            callee: KjOwn<ClientHook>,
            text: String,
        ) -> Result<String>;
        async fn rust_write(echo: KjOwn<ClientHook>, chunks: Vec<String>) -> Result<String>;
        async fn rust_fail(echo: KjOwn<ClientHook>, description: String) -> Result<()>;
        async fn rust_hang(echo: KjOwn<ClientHook>) -> Result<()>;
        /// Calls `echo.bounce()` `count` times with `size` bytes; see lib.rs.
        async fn rust_bounce_n(echo: KjOwn<ClientHook>, size: usize, count: u32) -> Result<()>;
    }
}
