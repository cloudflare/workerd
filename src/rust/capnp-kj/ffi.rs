// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge between capnp-rust's capability hooks and KJ's C++ RPC system.
//!
//! Every capability Rust holds is a C++ `capnp::ClientHook`. Messages cross as standalone
//! messages: the sending side copies params or results into a fresh message with its own
//! capability table, and the receiving side reads that message's segments in place.

#![allow(
    unsafe_code,
    reason = "holds the cxx bridge, which expands to unsafe FFI glue"
)]

use crate::message::RustMessage;
use crate::server::RustServer;
use crate::server::dispatch_bridge_call;
use crate::server::dispatch_call;
use crate::server::dispatch_rust_call;

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

        /// A call that C++ has sent: a promise for its response, and its pipeline.
        type CxxCall;

        fn client_add_ref(hook: &ClientHook) -> KjOwn<ClientHook>;
        fn client_ptr(hook: &ClientHook) -> usize;
        fn client_get_resolved(hook: &ClientHook) -> KjMaybe<KjOwn<ClientHook>>;
        fn client_is_settled(hook: &ClientHook) -> bool;
        async fn client_when_more_resolved(hook: KjOwn<ClientHook>) -> Result<KjOwn<ClientHook>>;
        async fn client_when_resolved(hook: KjOwn<ClientHook>) -> Result<()>;
        fn new_broken_cap(description: &str) -> KjOwn<ClientHook>;

        /// Sends a call to `hook` with `params`, which the capability reads in place.
        fn client_call(
            hook: &ClientHook,
            interface_id: u64,
            method_id: u16,
            params: Box<RustMessage>,
        ) -> Result<KjOwn<CxxCall>>;

        /// Sends a `-> stream` call to `hook`, copying `params` into the request.
        async fn client_send_streaming(
            hook: KjOwn<ClientHook>,
            interface_id: u64,
            method_id: u16,
            params: Box<RustMessage>,
        ) -> Result<()>;

        fn call_pipeline(call: &CxxCall) -> KjOwn<PipelineHook>;
        /// False if the call was sent with `noPromisePipelining`; see `noPromisePipelining()` in
        /// ffi.h.
        fn call_can_pipeline(call: &CxxCall) -> bool;
        /// Resolves to the call's context once the call completes, holding its results. The
        /// context, and those that the `context_*()` functions other than
        /// `context_set_results()` take, and that `dispatch_bridge_call()` and
        /// `dispatch_rust_call()` are given, are a `BridgeContext` (see ffi.h), which C++ checks;
        /// cxx can't hold one, which is reference counted, so they cross as the hook it
        /// implements.
        async fn call_response(call: KjOwn<CxxCall>) -> Result<KjOwn<CallContextHook>>;

        /// Whether a completed call's results are a Rust message, from a Rust server.
        fn context_has_rust_results(context: &CallContextHook) -> Result<bool>;
        /// A completed call's Rust results.
        fn context_rust_results<'a>(context: &'a CallContextHook) -> Result<&'a RustMessage>;
        /// A completed call's C++ results, which have no segments if the server wrote none.
        fn context_results_message(context: Pin<&mut CallContextHook>)
        -> Result<KjOwn<CxxMessage>>;
        /// Gives the call `context` a Rust server's results.
        fn context_set_rust_results(
            context: Pin<&mut CallContextHook>,
            results: Box<RustMessage>,
        ) -> Result<()>;

        fn pipeline_add_ref(pipeline: &PipelineHook) -> KjOwn<PipelineHook>;
        /// `pointer_indices` is the path through the results to the capability, one pointer
        /// field index per level.
        fn pipeline_get_cap(pipeline: &PipelineHook, pointer_indices: &[u16]) -> KjOwn<ClientHook>;

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

    extern "Rust" {
        type RustMessage;
        fn segment_count(self: &RustMessage) -> usize;
        unsafe fn segment<'a>(self: &'a RustMessage, index: usize) -> &'a [u8];
        fn cap_count(self: &RustMessage) -> usize;
        fn get_cap(self: &RustMessage, index: usize) -> KjMaybe<KjOwn<ClientHook>>;

        type RustServer;
        fn allows_cancellation(self: &RustServer, interface_id: u64, method_id: u16) -> bool;
        fn last_is_streaming(self: &RustServer) -> bool;
        async fn dispatch_call(
            server: &RustServer,
            interface_id: u64,
            method_id: u16,
            params: KjOwn<CxxMessage>,
            context: KjOwn<CallContextHook>,
        ) -> Result<()>;
        async fn dispatch_bridge_call(
            server: &RustServer,
            interface_id: u64,
            method_id: u16,
            params: KjOwn<CxxMessage>,
            context: KjOwn<CallContextHook>,
        ) -> Result<()>;
        async fn dispatch_rust_call(
            server: &RustServer,
            interface_id: u64,
            method_id: u16,
            params: Box<RustMessage>,
            context: KjOwn<CallContextHook>,
        ) -> Result<()>;
    }

    impl KjOwn<ClientHook> {}
    impl KjOwn<PipelineHook> {}
    impl KjOwn<CxxMessage> {}
    impl KjOwn<CxxCall> {}
    impl KjOwn<CallContextHook> {}
}
