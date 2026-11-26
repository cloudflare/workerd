// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge between capnp-rust's capability hooks and KJ's C++ RPC system.
//!
//! Every capability Rust holds is a C++ `capnp::ClientHook`, and a Rust server is wrapped in a C++
//! `capnp::Capability::Server` (`RustServerAdapter` in ffi.c++), so KJ delivers every call.
//! Messages cross as standalone copies, each with its own capability table, which the receiving
//! side reads in place:
//!
//! - A call from Rust copies its params into a KJ request (`client_send()`), and Rust reads a
//!   copy of its results (`call_response()`).
//! - A call to a Rust server gives the server a copy of its params (`dispatch_call()`), and the
//!   server's results are copied into the call's context (`context_set_results()`).
//!
//! `dispatch_call()` runs the Rust server's method until it first waits, as KJ runs a C++
//! server's until its first `co_await`; C++ takes the rest of a call that waits or fails with
//! `take_pending_call()`.

#![allow(
    unsafe_code,
    reason = "holds the cxx bridge, which expands to unsafe FFI glue"
)]

use crate::message::RustMessage;
use crate::server::RustServer;
use crate::server::dispatch_call;
use crate::server::take_pending_call;

#[cxx::bridge(namespace = "workerd::rust::capnp_kj")]
#[expect(
    clippy::needless_lifetimes,
    reason = "cxx needs explicit lifetimes on unsafe fns"
)]
pub mod bridge {
    unsafe extern "C++" {
        include!("workerd/rust/capnp-kj/ffi.h");

        /// A C++ `capnp::ClientHook`: a reference to a capability.
        #[namespace = "capnp"]
        type ClientHook;

        /// A C++ `capnp::PipelineHook`: the promised results of a call that is still running.
        #[namespace = "capnp"]
        type PipelineHook;

        /// A standalone C++ message with its own capability table.
        type CxxMessage;

        /// A call that C++ has sent for Rust: a promise for its response.
        type CxxCall;

        fn client_add_ref(hook: &ClientHook) -> KjOwn<ClientHook>;
        fn client_ptr(hook: &ClientHook) -> usize;
        fn client_get_resolved(hook: &ClientHook) -> KjMaybe<KjOwn<ClientHook>>;
        fn client_is_settled(hook: &ClientHook) -> bool;
        async fn client_when_more_resolved(hook: KjOwn<ClientHook>) -> Result<KjOwn<ClientHook>>;
        async fn client_when_resolved(hook: KjOwn<ClientHook>) -> Result<()>;
        fn new_broken_cap(description: &str) -> KjOwn<ClientHook>;

        /// Sends a call to `hook`, copying `params` into the request.
        fn client_send(
            hook: &ClientHook,
            interface_id: u64,
            method_id: u16,
            params: &RustMessage,
        ) -> Result<SentCall>;

        /// Sends a `-> stream` call to `hook`, copying `params` into the request.
        async fn client_send_streaming(
            hook: KjOwn<ClientHook>,
            interface_id: u64,
            method_id: u16,
            params: Box<RustMessage>,
        ) -> Result<()>;

        /// Resolves to a copy of the results of a call that `client_send()` sent, once the call
        /// completes.
        async fn call_response(call: KjOwn<CxxCall>) -> Result<KjOwn<CxxMessage>>;

        fn pipeline_add_ref(pipeline: &PipelineHook) -> KjOwn<PipelineHook>;
        /// `pointer_indices` is the path through the results to the capability, one pointer
        /// field index per level. Fails if the results are in and the path doesn't lead to a
        /// capability.
        fn pipeline_get_cap(
            pipeline: &PipelineHook,
            pointer_indices: &[u16],
        ) -> Result<KjOwn<ClientHook>>;

        fn message_segment_count(message: &CxxMessage) -> usize;
        fn message_segment<'a>(message: &'a CxxMessage, index: usize) -> &'a [u8];
        fn message_cap_count(message: &CxxMessage) -> usize;
        /// Returns a new reference to capability `index`.
        fn message_get_cap(
            message: Pin<&mut CxxMessage>,
            index: usize,
        ) -> KjMaybe<KjOwn<ClientHook>>;

        /// Wraps a Rust server in a C++ `capnp::Capability::Server`.
        fn new_rust_server_client(server: Box<RustServer>) -> KjOwn<ClientHook>;

        /// A C++ `capnp::CallContextHook`: a call that C++ made to a Rust server.
        #[namespace = "capnp"]
        type CallContextHook;

        /// Copies `results` into the results of the call `context`.
        fn context_set_results(
            context: Pin<&mut CallContextHook>,
            results: &RustMessage,
        ) -> Result<()>;
    }

    /// A call that C++ has sent for Rust.
    struct SentCall {
        /// The call, for `call_response()`.
        call: KjOwn<CxxCall>,
        /// The call's pipeline.
        pipeline: KjOwn<PipelineHook>,
    }

    /// A call a Rust server was given, once dispatched.
    struct Dispatched {
        /// Whether the call succeeded when it was dispatched; if not, C++ takes the rest of the
        /// call, which waits or has failed, with `take_pending_call()`.
        complete: bool,
        /// Whether the method is a `-> stream` method.
        is_streaming: bool,
    }

    extern "Rust" {
        type RustMessage;
        fn segment_count(self: &RustMessage) -> usize;
        /// Segment `index` of the message, or an empty slice past the last. The slice is valid
        /// while the message lives and isn't written to, which the C++ caller must uphold, as
        /// nothing checks the lifetime in C++. The function is `unsafe` only because cxx
        /// requires it of a Rust function whose signature names a lifetime, as one returning a
        /// reference must; calling it is safe.
        unsafe fn segment<'a>(self: &'a RustMessage, index: usize) -> &'a [u8];
        fn cap_count(self: &RustMessage) -> usize;
        fn get_cap(self: &RustMessage, index: usize) -> KjMaybe<KjOwn<ClientHook>>;

        type RustServer;
        fn allows_cancellation(self: &RustServer, interface_id: u64, method_id: u16) -> bool;
        fn dispatch_call(
            server: &RustServer,
            interface_id: u64,
            method_id: u16,
            params: KjOwn<CxxMessage>,
            context: KjOwn<CallContextHook>,
        ) -> Dispatched;
        /// The rest of the call that the server last dispatched, if `Dispatched::complete` was
        /// false; C++ takes it at once.
        async fn take_pending_call(server: &RustServer) -> Result<()>;
    }

    impl KjOwn<ClientHook> {}
    impl KjOwn<PipelineHook> {}
    impl KjOwn<CxxMessage> {}
    impl KjOwn<CxxCall> {}
    impl KjOwn<CallContextHook> {}
}
