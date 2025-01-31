use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll::Pending;
use std::task::Poll::Ready;
use std::task::Waker;

use cxx::ExternType;

use crate::ffi::CxxWaker;

use crate::Result;

// Expose Pin<Box<dyn Future<Output = ()>> to C++ as BoxFutureVoid.
//
// We want to allow C++ to own Rust Futures in a Box. At present, cxx-rs can easily expose Box<T>
// directly to C++ only if T implements Sized and Unpin. Dynamic trait types like `dyn Future` don't
// meet these requirements. One workaround is to pass Box<Box<dyn Future>> around. With a few more
// lines of boilerplate, we can avoid the extra Box:, as dtolnay showed in this demo PR:
// https://github.com/dtolnay/cxx/pull/672/files

pub struct BoxFuture<T>(Pin<Box<dyn Future<Output = T> + Send>>);

// TODO(now): Might as well implement Future for BoxFuture<T>.

// A From implementation to make it easier to convert from an arbitrary Future
// type into a BoxFuture<T>.
//
// Of interest: the `async-trait` crate contains a macro which seems like it could eliminate the
// `Box::pin(f).into()` boilerplate this currently requires. https://github.com/dtolnay/async-trait
//
// TODO(now): Understand why 'static is needed.
impl<T, F: Future<Output = T> + Send + 'static> From<Pin<Box<F>>> for BoxFuture<T> {
    fn from(value: Pin<Box<F>>) -> Self {
        BoxFuture(value)
    }
}

#[repr(transparent)]
pub struct PtrBoxFuture<T>(*mut BoxFuture<T>);

// =======================================================================================

// TODO(now): Define these trait implementations with a macro?

// Safety: The size of a Pin<P> is the size of P; the size of a Box<T> is the size of a reference to
// T, and references to `dyn Trait` types contain two pointers: one for the object, one for the
// vtable. So, the size of `Pin<Box<dyn Future<Output = T>>>` (a.k.a. `BoxFuture<T>`) is two
// pointers, and that is unlikely to change.
//
// https://doc.rust-lang.org/std/keyword.dyn.html
// - "As such, a dyn Trait reference contains two pointers."
unsafe impl ExternType for BoxFuture<()> {
    type Id = cxx::type_id!("workerd::rust::async::BoxFutureVoid");
    type Kind = cxx::kind::Trivial;
}

// Safety: Raw pointers are the same size in both languages.
unsafe impl ExternType for PtrBoxFuture<()> {
    type Id = cxx::type_id!("workerd::rust::async::PtrBoxFutureVoid");
    type Kind = cxx::kind::Trivial;
}

use crate::ffi::BoxFutureFulfillerVoid;

pub fn box_future_poll_void(
    future: &mut BoxFuture<()>,
    waker: &CxxWaker,
    fulfiller: Pin<&mut BoxFutureFulfillerVoid>,
) -> bool {
    let waker = Waker::from(waker);
    let mut cx = Context::from_waker(&waker);
    match future.0.as_mut().poll(&mut cx) {
        Ready(_v) => {
            fulfiller.fulfill();
            true
        }
        Pending => false,
    }
}

pub unsafe fn box_future_drop_in_place_void(ptr: PtrBoxFuture<()>) {
    std::ptr::drop_in_place(ptr.0);
}

// ---------------------------------------------------------

// Safety: The size of a Pin<P> is the size of P; the size of a Box<T> is the size of a reference to
// T, and references to `dyn Trait` types contain two pointers: one for the object, one for the
// vtable. So, the size of `Pin<Box<dyn Future<Output = T>>>` (a.k.a. `BoxFuture<T>`) is two
// pointers, and that is unlikely to change.
//
// https://doc.rust-lang.org/std/keyword.dyn.html
// - "As such, a dyn Trait reference contains two pointers."
unsafe impl ExternType for BoxFuture<Result<()>> {
    type Id = cxx::type_id!("workerd::rust::async::BoxFutureFallibleVoid");
    type Kind = cxx::kind::Trivial;
}

// Safety: Raw pointers are the same size in both languages.
unsafe impl ExternType for PtrBoxFuture<Result<()>> {
    type Id = cxx::type_id!("workerd::rust::async::PtrBoxFutureFallibleVoid");
    type Kind = cxx::kind::Trivial;
}

use crate::ffi::BoxFutureFulfillerFallibleVoid;

pub fn box_future_poll_fallible_void(
    future: &mut BoxFuture<Result<()>>,
    waker: &CxxWaker,
    fulfiller: Pin<&mut BoxFutureFulfillerFallibleVoid>,
) -> Result<bool> {
    let waker = Waker::from(waker);
    let mut cx = Context::from_waker(&waker);
    match future.0.as_mut().poll(&mut cx) {
        Ready(Ok(_v)) => {
            fulfiller.fulfill();
            Ok(true)
        }
        Ready(Err(e)) => Err(e),
        Pending => Ok(false),
    }
}

pub unsafe fn box_future_drop_in_place_fallible_void(ptr: PtrBoxFuture<Result<()>>) {
    std::ptr::drop_in_place(ptr.0);
}

// ---------------------------------------------------------

unsafe impl ExternType for BoxFuture<Result<i32>> {
    type Id = cxx::type_id!("workerd::rust::async::BoxFutureFallibleI32");
    type Kind = cxx::kind::Trivial;
}

// Safety: Raw pointers are the same size in both languages.
unsafe impl ExternType for PtrBoxFuture<Result<i32>> {
    type Id = cxx::type_id!("workerd::rust::async::PtrBoxFutureFallibleI32");
    type Kind = cxx::kind::Trivial;
}

use crate::ffi::BoxFutureFulfillerFallibleI32;

pub fn box_future_poll_fallible_i32(
    future: &mut BoxFuture<Result<i32>>,
    waker: &CxxWaker,
    fulfiller: Pin<&mut BoxFutureFulfillerFallibleI32>,
) -> Result<bool> {
    let waker = Waker::from(waker);
    let mut cx = Context::from_waker(&waker);
    match future.0.as_mut().poll(&mut cx) {
        Ready(Ok(v)) => {
            fulfiller.fulfill(v);
            Ok(true)
        }
        Ready(Err(e)) => Err(e),
        Pending => Ok(false),
    }
}

pub unsafe fn box_future_drop_in_place_fallible_i32(ptr: PtrBoxFuture<Result<i32>>) {
    std::ptr::drop_in_place(ptr.0);
}
