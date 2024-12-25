use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Waker;

use cxx::ExternType;

use crate::ffi::RootWaker;

// Expose Pin<Box<dyn Future<Output = ()>> to C++ as BoxFutureVoid.
//
// We want to allow C++ to own Rust Futures in a Box. At present, cxx-rs can easily expose Box<T>
// directly to C++ only if T implements Sized and Unpin. Dynamic trait types like `dyn Future` don't
// meet these requirements. One workaround is to pass Box<Box<dyn Future>> around. With a few more
// lines of boilerplate, we can avoid the extra Box:, as dtolnay showed in this demo PR:
// https://github.com/dtolnay/cxx/pull/672/files

pub struct BoxFuture<T>(Pin<Box<dyn Future<Output = T>>>);

#[repr(transparent)]
pub struct PtrBoxFuture<T>(*mut BoxFuture<T>);

// A From implementation to make it easier to convert from an arbitrary Future
// type into a BoxFuture<T>.
//
// TODO(now): Understand why 'static is needed.
impl<T, F: Future<Output = T> + 'static> From<Pin<Box<F>>> for BoxFuture<T> {
    fn from(value: Pin<Box<F>>) -> Self {
        BoxFuture(value)
    }
}

// We must manually implement the ExternType trait, poll, and drop functions for each possible T of
// BoxFuture<T> and PtrBoxFuture<T>.
//
// TODO(now): Make this a macro so we can define them easier?
unsafe impl ExternType for BoxFuture<()> {
    type Id = cxx::type_id!("workerd::rust::async::BoxFutureVoid");
    type Kind = cxx::kind::Trivial;
}

unsafe impl ExternType for PtrBoxFuture<()> {
    type Id = cxx::type_id!("workerd::rust::async::PtrBoxFutureVoid");
    type Kind = cxx::kind::Trivial;
}

pub fn box_future_void_poll(future: &mut BoxFuture<()>, waker: &RootWaker) -> bool {
    let waker = Waker::from(waker);
    let mut cx = Context::from_waker(&waker);
    // TODO(now): Figure out how to propagate value-or-exception.
    future.0.as_mut().poll(&mut cx).is_ready()
}

pub unsafe fn box_future_void_drop_in_place(ptr: PtrBoxFuture<()>) {
    std::ptr::drop_in_place(ptr.0);
}
