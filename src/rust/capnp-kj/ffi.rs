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
use crate::server::take_pending_call;

#[cxx::bridge(namespace = "workerd::rust::capnp_kj")]
#[expect(
    clippy::needless_lifetimes,
    clippy::elidable_lifetime_names,
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
        ) -> Result<SentCall>;

        /// Sends a `-> stream` call to `hook`, copying `params` into the request.
        async fn client_send_streaming(
            hook: KjOwn<ClientHook>,
            interface_id: u64,
            method_id: u16,
            params: Box<RustMessage>,
        ) -> Result<()>;

        /// Resolves to the context of a call that `client_call()` sent, once the call completes,
        /// holding its results. Rust calls it at once. The context, and those that the
        /// `context_*()` functions other than `context_set_results()` take, and that
        /// `dispatch_bridge_call()` and `dispatch_rust_call()` are given, are a `BridgeContext`
        /// (see ffi.h), which C++ checks; cxx can't hold one, which is reference counted, so they
        /// cross as the hook it implements.
        async fn call_response(context: KjOwn<CallContextHook>) -> Result<KjOwn<CallContextHook>>;

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

    /// A call that C++ has sent for Rust.
    struct SentCall {
        /// The call's context, which holds the call until `call_response()` takes it. The call
        /// holds the context in turn, so Rust must pass every context to `call_response()`.
        context: KjOwn<CallContextHook>,
        /// The call's pipeline, or none if the call was sent with `noPromisePipelining` (see
        /// `noPromisePipelining()` in ffi.h), as its results can't hold a capability.
        pipeline: KjMaybe<KjOwn<PipelineHook>>,
    }

    /// A call a Rust server was given, once dispatched.
    struct Dispatched {
        /// Whether the call succeeded when it was dispatched; if not, C++ takes the rest of the
        /// call, which waits or has failed, with `take_pending_call()`.
        complete: bool,
        /// Whether the method is a `-> stream` method.
        is_streaming: bool,
    }

    /// A segment of a Rust message.
    struct RustSegment<'a> {
        bytes: &'a [u8],
    }

    extern "Rust" {
        type RustMessage;
        fn segment_count(self: &RustMessage) -> usize;
        unsafe fn segment<'a>(self: &'a RustMessage, index: usize) -> &'a [u8];
        /// Every segment, for a message with more than one, whose segments `segment()` would
        /// look up afresh each time.
        unsafe fn segments<'a>(self: &'a RustMessage) -> Vec<RustSegment<'a>>;
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
        fn dispatch_bridge_call(
            server: &RustServer,
            interface_id: u64,
            method_id: u16,
            params: KjOwn<CxxMessage>,
            context: KjOwn<CallContextHook>,
        ) -> Dispatched;
        fn dispatch_rust_call(
            server: &RustServer,
            interface_id: u64,
            method_id: u16,
            params: Box<RustMessage>,
            context: KjOwn<CallContextHook>,
        ) -> Dispatched;
        /// The rest of the call that the server last dispatched, if `Dispatched::complete` was
        /// false; C++ takes it at once.
        async fn take_pending_call(server: &RustServer) -> Result<()>;
    }

    impl KjOwn<ClientHook> {}
    impl KjOwn<PipelineHook> {}
    impl KjOwn<CxxMessage> {}
    impl KjOwn<CallContextHook> {}
}
