use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

// Expose Pin<Box<dyn Future<Output = ()>> to C++ as BoxFutureVoid.
//
// We want to allow C++ to own Rust Futures in a Box. At present, cxx-rs can easily expose Box<T>
// directly to C++ only if T implements Sized and Unpin. Dynamic trait types like `dyn Future` don't
// meet these requirements. One workaround is to pass Box<Box<dyn Future>> around. With a few more
// lines of boilerplate, we can avoid the extra Box:, as dtolnay showed in this demo PR:
// https://github.com/dtolnay/cxx/pull/672/files

pub struct BoxFuture<T>(Pin<Box<dyn Future<Output = T> + Send>>);

impl<T> Future for BoxFuture<T> {
    type Output = T;
    fn poll(mut self: Pin<&mut Self>, cx: &mut Context) -> Poll<T> {
        self.0.as_mut().poll(cx)
    }
}

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

impl<T> PtrBoxFuture<T> {
    pub unsafe fn drop_in_place(self) {
        std::ptr::drop_in_place(self.0);
    }
}
