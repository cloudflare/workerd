use std::pin::Pin;
use std::future::Future;
use std::task::Poll;
use std::task::Waker;

use crate::BoxFuture;

pub fn new_pending_future_void() -> BoxFuture<()> {
    Box::pin(std::future::pending()).into()
}
pub fn new_ready_future_void() -> BoxFuture<()> {
    Box::pin(std::future::ready(())).into()
}

use crate::ffi::CloningAction;
use crate::ffi::WakingAction;

struct WakingFuture {
    done: bool,
    cloning_action: CloningAction,
    waking_action: WakingAction,
}

impl WakingFuture {
    fn new(cloning_action: CloningAction, waking_action: WakingAction) -> Self {
        Self { done: false, cloning_action, waking_action }
    }
}

fn do_no_clone_wake(waker: &Waker, waking_action: WakingAction)  {
    match waking_action {
        WakingAction::None => {},
        WakingAction::WakeByRefSameThread => waker.wake_by_ref(),
        WakingAction::WakeByRefBackgroundThread => on_background_thread(|| waker.wake_by_ref()),
        WakingAction::WakeSameThread | WakingAction::WakeBackgroundThread => {
            panic!("cannot wake() without cloning");
        }
        _ => panic!("invalid WakingAction"),
    }
}

fn do_cloned_wake(waker: Waker, waking_action: WakingAction)  {
    match waking_action {
        WakingAction::None => {},
        WakingAction::WakeByRefSameThread => waker.wake_by_ref(),
        WakingAction::WakeByRefBackgroundThread => on_background_thread(|| waker.wake_by_ref()),
        WakingAction::WakeSameThread => waker.wake(),
        WakingAction::WakeBackgroundThread => on_background_thread(move || waker.wake()),
        _ => panic!("invalid WakingAction"),
    }
}

impl Future for WakingFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut std::task::Context) -> Poll<()> {
        if self.done {
            return Poll::Ready(());
        }

        let waker = cx.waker();

        match self.cloning_action {
            CloningAction::None => {
                do_no_clone_wake(waker, self.waking_action);
            }
            CloningAction::CloneSameThread => {
                let waker = waker.clone();
                do_cloned_wake(waker, self.waking_action);
            },
            CloningAction::CloneBackgroundThread => {
                let waker = on_background_thread(|| waker.clone());
                do_cloned_wake(waker, self.waking_action);
            },
            CloningAction::WakeByRefThenCloneSameThread => {
                waker.wake_by_ref();
                let waker = waker.clone();
                do_cloned_wake(waker, self.waking_action);
            }
            _ => panic!("invalid CloningAction"),
        }

        self.done = true;
        Poll::Pending
    }
}

pub fn new_waking_future_void(cloning_action: CloningAction, waking_action: WakingAction) -> BoxFuture<()> {
    Box::pin(WakingFuture::new(cloning_action, waking_action)).into()
}

struct ThreadedDelayFuture {
    handle: Option<std::thread::JoinHandle<()>>,
}

impl ThreadedDelayFuture {
    fn new() -> Self {
        Self { handle: None }
    }
}

/// Run a function, `f`, on a thread in the background and return its result.
fn on_background_thread<T: Send>(f: impl FnOnce() -> T + Send) -> T {
    std::thread::scope(|scope| scope.spawn(|| f()).join().unwrap())
}

impl Future for ThreadedDelayFuture {
    type Output = ();
    fn poll(mut self: Pin<&mut Self>, cx: &mut std::task::Context) -> Poll<()> {
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
            return Poll::Ready(());
        }

        let waker = cx.waker();
        let waker = on_background_thread(|| waker.clone());
        self.handle = Some(std::thread::spawn(|| {
            std::thread::sleep(std::time::Duration::from_millis(100));
            waker.wake();
        }));
        Poll::Pending
    }
}

pub fn new_threaded_delay_future_void() -> BoxFuture<()> {
    Box::pin(ThreadedDelayFuture::new()).into()
}

pub fn new_layered_ready_future_void() -> BoxFuture<()> {
    Box::pin(async {
        crate::ffi::new_ready_promise_node().await;
        crate::ffi::new_coroutine_promise_node().await;
    }).into()
}

// From example at https://doc.rust-lang.org/std/future/fn.poll_fn.html#capturing-a-pinned-state
fn naive_select<T>(
    a: impl Future<Output = T>,
    b: impl Future<Output = T>,
) -> impl Future<Output = T>
{
    let (mut a, mut b) = (Box::pin(a), Box::pin(b));
    future::poll_fn(move |cx| {
        if let Poll::Ready(r) = a.as_mut().poll(cx) {
            Poll::Ready(r)
        } else if let Poll::Ready(r) = b.as_mut().poll(cx) {
            Poll::Ready(r)
        } else {
            Poll::Pending
        }
    })
}

use std::future;
use std::future::IntoFuture;

// A Future which polls multiple OwnPromiseNodes at once.
pub fn new_naive_select_future_void() -> BoxFuture<()> {
    Box::pin(
        naive_select(
            crate::ffi::new_coroutine_promise_node().into_future(),
            crate::ffi::new_coroutine_promise_node().into_future()
        )).into()
}

// TODO(now): Similar as above, but poll() until all are ready.
