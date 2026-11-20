// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Calling a C++ capability from Rust.
//!
//! [`KjClient`] is the only `ClientHook` this crate creates. Promise resolution, embargoes,
//! pipelining and local calls are KJ's; these hooks forward to it.

use capnp::Error;
use capnp::any_pointer;
use capnp::capability::Promise;
use capnp::capability::RemotePromise;
use capnp::capability::Request;
use capnp::capability::Response;
use capnp::private::capability::ClientHook;
use capnp::private::capability::ParamsHook;
use capnp::private::capability::PipelineHook;
use capnp::private::capability::PipelineOp;
use capnp::private::capability::RequestHook;
use capnp::private::capability::ResponseHook;
use capnp::private::capability::ResultsHook;
use kj_rs::KjOwn;

use crate::error::from_kj;
use crate::ffi::bridge;
use crate::message::CxxMessageReader;
use crate::message::RustMessage;

/// Identifies a [`KjClient`] among `ClientHook`s (`ClientHook::get_brand`).
static KJ_BRAND: u8 = 0;

fn kj_brand() -> usize {
    std::ptr::from_ref(&KJ_BRAND) as usize
}

/// A Rust `ClientHook` over a C++ capability.
pub struct KjClient {
    hook: KjOwn<bridge::ClientHook>,
}

impl KjClient {
    pub fn new(hook: KjOwn<bridge::ClientHook>) -> Self {
        Self { hook }
    }

    /// Returns the C++ capability behind `hook`.
    ///
    /// Every `ClientHook` that reaches this crate's messages is a `KjClient`, since this crate
    /// creates no other kind; a foreign one becomes a broken capability.
    ///
    /// The downcast trusts `get_brand()`, a safe trait method, so its soundness rests on an
    /// invariant: no `ClientHook` but `KjClient` returns `kj_brand()`. A hook that wraps a
    /// `KjClient` must return a brand of its own rather than forward the inner one. `kj_brand()`
    /// is private to this module, so only code here can break the invariant.
    pub fn into_kj(hook: Box<dyn ClientHook>) -> KjOwn<bridge::ClientHook> {
        if hook.get_brand() != kj_brand() {
            return bridge::new_broken_cap("capability is not backed by KJ");
        }
        #[expect(
            clippy::cast_ptr_alignment,
            reason = "the pointer came from a Box<KjClient>, so it has KjClient's alignment"
        )]
        let raw = Box::into_raw(hook).cast::<Self>();
        #[expect(
            unsafe_code,
            reason = "downcasting a trait object identified by its brand"
        )]
        // Safety: by the invariant above, only `KjClient` returns `kj_brand()` from `get_brand()`,
        // so the box holds a `KjClient`, allocated by `Box::new` with its layout.
        let client = unsafe { Box::from_raw(raw) };
        client.hook
    }

    fn add_ref_kj(&self) -> KjOwn<bridge::ClientHook> {
        bridge::client_add_ref(&self.hook)
    }
}

impl ClientHook for KjClient {
    fn add_ref(&self) -> Box<dyn ClientHook> {
        Box::new(Self::new(self.add_ref_kj()))
    }

    fn new_call(
        &self,
        interface_id: u64,
        method_id: u16,
        _size_hint: Option<capnp::MessageSize>,
    ) -> Request<any_pointer::Owned, any_pointer::Owned> {
        Request::new(Box::new(KjRequest::new(
            self.add_ref_kj(),
            interface_id,
            method_id,
        )))
    }

    fn call(
        &self,
        interface_id: u64,
        method_id: u16,
        params: Box<dyn ParamsHook>,
        mut results: Box<dyn ResultsHook>,
    ) -> Promise<(), Error> {
        let mut request = KjRequest::new(self.add_ref_kj(), interface_id, method_id);
        if let Err(e) = params.get().and_then(|p| request.get().set_as(p)) {
            return Promise::err(e);
        }
        drop(params);
        let remote = Box::new(request).send();
        Promise::from_future(async move {
            let response = remote.promise.await?;
            results.get()?.set_as(response.get()?)?;
            Ok(())
        })
    }

    fn get_brand(&self) -> usize {
        kj_brand()
    }

    fn get_ptr(&self) -> usize {
        bridge::client_ptr(&self.hook)
    }

    fn get_resolved(&self) -> Option<Box<dyn ClientHook>> {
        Option::<KjOwn<bridge::ClientHook>>::from(bridge::client_get_resolved(&self.hook))
            .map(|hook| Box::new(Self::new(hook)) as Box<dyn ClientHook>)
    }

    fn when_more_resolved(&self) -> Option<Promise<Box<dyn ClientHook>, Error>> {
        if bridge::client_is_settled(&self.hook) {
            return None;
        }
        let hook = self.add_ref_kj();
        Some(Promise::from_future(async move {
            let resolved = bridge::client_when_more_resolved(hook)
                .await
                .map_err(|e| from_kj(&e))?;
            Ok(Box::new(Self::new(resolved)) as Box<dyn ClientHook>)
        }))
    }

    fn when_resolved(&self) -> Promise<(), Error> {
        let hook = self.add_ref_kj();
        Promise::from_future(async move {
            bridge::client_when_resolved(hook)
                .await
                .map_err(|e| from_kj(&e))
        })
    }
}

/// A call being built in Rust, to a C++ capability.
struct KjRequest {
    client: KjOwn<bridge::ClientHook>,
    interface_id: u64,
    method_id: u16,
    // Boxed, for C++ to take.
    params: Box<RustMessage>,
}

impl KjRequest {
    fn new(client: KjOwn<bridge::ClientHook>, interface_id: u64, method_id: u16) -> Self {
        Self {
            client,
            interface_id,
            method_id,
            params: Box::new(RustMessage::new()),
        }
    }
}

impl RequestHook for KjRequest {
    fn get(&mut self) -> any_pointer::Builder<'_> {
        match self.params.root() {
            Ok(root) => root,
            // `new` initialized the root, and an `AnyPointer` root reads as any pointer.
            Err(_) => unreachable!(),
        }
    }

    fn get_brand(&self) -> usize {
        0
    }

    fn send(self: Box<Self>) -> RemotePromise<any_pointer::Owned> {
        let Self {
            client,
            interface_id,
            method_id,
            params,
        } = *self;
        match bridge::client_call(&client, interface_id, method_id, params) {
            Ok(call) => {
                // A call whose results can't hold a capability has nothing to pipeline on; its
                // pipeline needs no allocation.
                let pipeline: Box<dyn PipelineHook> = if bridge::call_can_pipeline(&call) {
                    Box::new(KjPipeline {
                        hook: bridge::call_pipeline(&call),
                    })
                } else {
                    Box::new(NoPipeline)
                };
                RemotePromise {
                    promise: Promise::from_future(async move {
                        let context = bridge::call_response(call).await.map_err(|e| from_kj(&e))?;
                        Ok(Response::new(
                            Box::new(KjResponse::new(context)?) as Box<dyn ResponseHook>
                        ))
                    }),
                    pipeline: any_pointer::Pipeline::new(pipeline),
                }
            }
            Err(e) => {
                let e = from_kj(&e);
                RemotePromise {
                    pipeline: any_pointer::Pipeline::new(Box::new(BrokenPipeline {
                        description: e.to_string(),
                    })),
                    promise: Promise::err(e),
                }
            }
        }
    }

    fn send_streaming(self: Box<Self>) -> Promise<(), Error> {
        let Self {
            client,
            interface_id,
            method_id,
            params,
        } = *self;
        // Sent now rather than when the promise is first polled, so that streaming calls reach
        // the server in the order they were sent, as `send()`'s do.
        let sent = bridge::client_send_streaming(client, interface_id, method_id, params);
        Promise::from_future(async move { sent.await.map_err(|e| from_kj(&e)) })
    }

    fn tail_send(self: Box<Self>) -> Option<(u32, Promise<(), Error>, Box<dyn PipelineHook>)> {
        // Only the two-party RPC system can turn a call into a tail call.
        None
    }
}

/// The response to a call from Rust, read in place.
enum KjResponse {
    /// Results a Rust server built.
    Rust(Box<RustMessage>),
    /// Results a C++ server built.
    Cxx(CxxMessageReader),
    /// The server wrote no results: a message whose root is null.
    Empty(capnp::message::Reader<capnp::message::SegmentArray<'static>>),
}

/// The segments of a message whose root is a null pointer.
#[repr(align(8))]
struct NullRoot([u8; 8]);
static NULL_ROOT: NullRoot = NullRoot([0; 8]);
static NULL_ROOT_SEGMENTS: [&[u8]; 1] = [&NULL_ROOT.0];

impl KjResponse {
    fn new(mut context: KjOwn<bridge::CallContextHook>) -> capnp::Result<Self> {
        if bridge::context_has_rust_results(&context) {
            let results =
                bridge::context_take_rust_results(context.pin_mut()).map_err(|e| from_kj(&e))?;
            return Ok(Self::Rust(results));
        }
        let message = bridge::context_into_message(context);
        if bridge::message_segment_count(&message) == 0 {
            return Ok(Self::Empty(capnp::message::Reader::new(
                capnp::message::SegmentArray::new(&NULL_ROOT_SEGMENTS),
                capnp::message::ReaderOptions::new(),
            )));
        }
        Ok(Self::Cxx(CxxMessageReader::new(message)))
    }
}

impl ResponseHook for KjResponse {
    fn get(&self) -> capnp::Result<any_pointer::Reader<'_>> {
        match self {
            Self::Rust(message) => message.root_as_reader(),
            Self::Cxx(message) => message.root(),
            Self::Empty(message) => message.get_root(),
        }
    }
}

/// The pipeline of a call from Rust to C++.
struct KjPipeline {
    hook: KjOwn<bridge::PipelineHook>,
}

impl PipelineHook for KjPipeline {
    fn add_ref(&self) -> Box<dyn PipelineHook> {
        Box::new(Self {
            hook: bridge::pipeline_add_ref(&self.hook),
        })
    }

    fn get_pipelined_cap(&self, ops: &[PipelineOp]) -> Box<dyn ClientHook> {
        let pointer_indices: Vec<u16> = ops
            .iter()
            .filter_map(|op| match op {
                PipelineOp::Noop => None,
                PipelineOp::GetPointerField(index) => Some(*index),
            })
            .collect();
        Box::new(KjClient::new(bridge::pipeline_get_cap(
            &self.hook,
            &pointer_indices,
        )))
    }
}

/// The pipeline of a call whose results can't contain a capability.
struct NoPipeline;

impl PipelineHook for NoPipeline {
    fn add_ref(&self) -> Box<dyn PipelineHook> {
        Box::new(Self)
    }

    fn get_pipelined_cap(&self, _ops: &[PipelineOp]) -> Box<dyn ClientHook> {
        // The results have no capabilities, so generated code has no pipelined field to ask for.
        Box::new(KjClient::new(bridge::new_broken_cap(
            "the method's results contain no capabilities to pipeline on",
        )))
    }
}

/// The pipeline of a call that failed before it was sent.
pub struct BrokenPipeline {
    description: String,
}

pub fn broken_pipeline(description: &str) -> BrokenPipeline {
    BrokenPipeline {
        description: description.to_owned(),
    }
}

impl PipelineHook for BrokenPipeline {
    fn add_ref(&self) -> Box<dyn PipelineHook> {
        Box::new(Self {
            description: self.description.clone(),
        })
    }

    fn get_pipelined_cap(&self, _ops: &[PipelineOp]) -> Box<dyn ClientHook> {
        Box::new(KjClient::new(bridge::new_broken_cap(&self.description)))
    }
}
