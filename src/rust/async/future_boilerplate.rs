use std::future::Future;

use std::pin::Pin;

use std::task::Context;
use std::task::Poll::Pending;
use std::task::Poll::Ready;
use std::task::Waker;

use cxx::ExternType;

use crate::ffi::KjWaker;

use crate::Result;

use crate::future::BoxFuture;
use crate::future::PtrBoxFuture;

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

pub fn box_future_poll_void(
    future: Pin<&mut BoxFuture<()>>,
    waker: &KjWaker,
    fulfiller: Pin<&mut crate::ffi::BoxFutureFulfillerVoid>,
) -> bool {
    let waker = Waker::from(waker);
    let mut cx = Context::from_waker(&waker);
    match future.poll(&mut cx) {
        Ready(_v) => {
            fulfiller.fulfill();
            true
        }
        Pending => false,
    }
}

pub unsafe fn box_future_drop_in_place_void(ptr: PtrBoxFuture<()>) {
    ptr.drop_in_place();
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

pub fn box_future_poll_fallible_void(
    future: Pin<&mut BoxFuture<Result<()>>>,
    waker: &KjWaker,
    fulfiller: Pin<&mut crate::ffi::BoxFutureFulfillerFallibleVoid>,
) -> Result<bool> {
    let waker = Waker::from(waker);
    let mut cx = Context::from_waker(&waker);
    match future.poll(&mut cx) {
        Ready(Ok(_v)) => {
            fulfiller.fulfill();
            Ok(true)
        }
        Ready(Err(e)) => Err(e),
        Pending => Ok(false),
    }
}

pub unsafe fn box_future_drop_in_place_fallible_void(ptr: PtrBoxFuture<Result<()>>) {
    ptr.drop_in_place();
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

pub fn box_future_poll_fallible_i32(
    future: Pin<&mut BoxFuture<Result<i32>>>,
    waker: &KjWaker,
    fulfiller: Pin<&mut crate::ffi::BoxFutureFulfillerFallibleI32>,
) -> Result<bool> {
    let waker = Waker::from(waker);
    let mut cx = Context::from_waker(&waker);
    match future.poll(&mut cx) {
        Ready(Ok(v)) => {
            fulfiller.fulfill(v);
            Ok(true)
        }
        Ready(Err(e)) => Err(e),
        Pending => Ok(false),
    }
}

pub unsafe fn box_future_drop_in_place_fallible_i32(ptr: PtrBoxFuture<Result<i32>>) {
    ptr.drop_in_place();
}
