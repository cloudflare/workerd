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
use crate::server::RustCall;
use crate::server::RustServer;
use crate::server::run_call;

#[cxx::bridge(namespace = "workerd::rust::capnp_kj")]
#[expect(
    clippy::needless_lifetimes,
    clippy::unnecessary_box_returns,
    reason = "cxx needs explicit lifetimes on unsafe fns, and C++ holds Rust types by Box"
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

        /// Sends a call to `hook`, copying `params` into the request.
        fn client_send(
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
        async fn call_response(call: KjOwn<CxxCall>) -> Result<KjOwn<CxxMessage>>;

        fn pipeline_add_ref(pipeline: &PipelineHook) -> KjOwn<PipelineHook>;
        /// `pointer_indices` is the path through the results to the capability, one pointer
        /// field index per level.
        fn pipeline_get_cap(pipeline: &PipelineHook, pointer_indices: &[u16]) -> KjOwn<ClientHook>;

        fn message_segment_count(message: &CxxMessage) -> usize;
        fn message_segment<'a>(message: &'a CxxMessage, index: usize) -> &'a [u8];
        fn message_cap_count(message: &CxxMessage) -> usize;
        fn message_take_cap(
            message: Pin<&mut CxxMessage>,
            index: usize,
        ) -> KjMaybe<KjOwn<ClientHook>>;

        /// Wraps a Rust server in a C++ `capnp::Capability::Server`.
        fn new_rust_server_client(server: Box<RustServer>) -> KjOwn<ClientHook>;
    }

    extern "Rust" {
        type RustMessage;
        fn segment_count(self: &RustMessage) -> usize;
        unsafe fn segment<'a>(self: &'a RustMessage, index: usize) -> &'a [u8];
        fn cap_count(self: &RustMessage) -> usize;
        fn take_cap(self: &mut RustMessage, index: usize) -> KjMaybe<KjOwn<ClientHook>>;

        type RustServer;
        type RustCall;
        fn dispatch(
            self: &RustServer,
            interface_id: u64,
            method_id: u16,
            params: KjOwn<CxxMessage>,
        ) -> Box<RustCall>;
        fn allows_cancellation(self: &RustServer) -> bool;
        fn is_streaming(self: &RustCall) -> bool;
        async fn run_call(call: Box<RustCall>) -> Result<Box<RustMessage>>;
    }

    impl KjOwn<ClientHook> {}
    impl KjOwn<PipelineHook> {}
    impl KjOwn<CxxMessage> {}
    impl KjOwn<CxxCall> {}
}
