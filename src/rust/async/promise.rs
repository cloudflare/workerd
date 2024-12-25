use std::future::Future;
use std::future::IntoFuture;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use cxx::ExternType;

use crate::waker::deref_await_waker;

// OwnPromiseNode

// The inner pointer is never read on Rust's side, so Rust thinks it's dead code.
#[allow(dead_code)]
pub struct OwnPromiseNode(*const ());

#[repr(transparent)]
pub struct PtrOwnPromiseNode(*mut OwnPromiseNode);

unsafe impl ExternType for OwnPromiseNode {
    type Id = cxx::type_id!("workerd::rust::async::OwnPromiseNode");
    type Kind = cxx::kind::Trivial;
}

unsafe impl ExternType for PtrOwnPromiseNode {
    type Id = cxx::type_id!("workerd::rust::async::PtrOwnPromiseNode");
    type Kind = cxx::kind::Trivial;
}

impl Drop for OwnPromiseNode {
    fn drop(&mut self) {
        // The pin crate suggests implementing drop traits for address-sensitive types with an inner
        // function which accepts a `Pin<&mut Type>` parameter, to help uphold pinning guarantees.
        // However, since our drop function is actually a C++ destructor to which we must pass a raw
        // pointer, there is no benefit in creating a Pin from `self`.
        unsafe {
            crate::ffi::own_promise_node_drop_in_place(PtrOwnPromiseNode(self));
        }
    }
}

impl IntoFuture for OwnPromiseNode {
    type Output = ();
    type IntoFuture = OwnPromiseNodeFuture;

    fn into_future(self) -> Self::IntoFuture {
        // Idea: We could return an `async { ... }.await` expression here, and use the `moveit!`
        // macro to emplace a kj::_::Event on "the stack" (actually the async block's frame), pass
        // that Event to `self->onReady()`. When the Event fires, it sets a boolean flag that tells
        // us this OwnPromiseNode is ready for `get()`ing.
        Self::IntoFuture::new(self)
    }
}

enum OwnPromiseNodeFutureState {
    OnReady,
    Get,
    Complete,
}

pub struct OwnPromiseNodeFuture {
    node: OwnPromiseNode,
    state: OwnPromiseNodeFutureState,
}

impl OwnPromiseNodeFuture {
    pub fn new(node: OwnPromiseNode) -> Self {
        Self {
            node: node,
            state: OwnPromiseNodeFutureState::OnReady,
        }
    }
}

impl Future for OwnPromiseNodeFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<()> {
        match self.state {
            OwnPromiseNodeFutureState::OnReady => {
                if let Some(await_waker) = deref_await_waker(cx.waker()) {
                    let node = unsafe { self.as_mut().map_unchecked_mut(|s| &mut s.node) };
                    await_waker.wake_after(node);
                    self.state = OwnPromiseNodeFutureState::Get;
                    Poll::Pending
                } else {
                    unreachable!("unimplemented");
                    // TODO(now): Store a clone of the waker, then replace self.node with the result
                    //   of wake_after(&waker, node), which will be implemented like
                    //   node.attach(kj::defer([&waker]() { waker.wake_by_ref(); }))
                    //       .eagerlyEvaluate(nullptr)
                    // self.state = OwnPromiseNodeFutureState::Get;
                    // Poll::Pending
                }
            }
            OwnPromiseNodeFutureState::Get => {
                // TODO(now): Get the result from the node.
                self.state = OwnPromiseNodeFutureState::Complete;
                Poll::Ready(())
            }
            OwnPromiseNodeFutureState::Complete => {
                unreachable!("OwnPromiseNode polled after completion");
            }
        }
    }
}
