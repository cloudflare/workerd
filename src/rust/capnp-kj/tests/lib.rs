// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Rust half of capnp-kj-test: a Rust `TestEcho` server, and Rust clients of any `TestEcho`,
//! which capnp-kj-test.c++ calls.

mod container;
mod ffi;

use std::cell::Cell;
use std::cell::RefCell;

use capnp::Error;
use capnp::any_pointer;
use capnp::capability::Rc;
use cxx::KjError;
use kj_rs::KjOwn;
use test_capnp::test_echo;

thread_local! {
    static HANG_STARTS: Cell<u32> = const { Cell::new(0) };
    static HANG_DROPS: Cell<u32> = const { Cell::new(0) };
}

/// Counts, when dropped, a `hang()` call that never completed.
struct HangGuard;

impl Drop for HangGuard {
    fn drop(&mut self) {
        HANG_DROPS.with(|drops| drops.set(drops.get() + 1));
    }
}

struct EchoServer {
    prefix: String,
    written: RefCell<Vec<u8>>,
}

impl EchoServer {
    fn new(prefix: String) -> Self {
        Self {
            prefix,
            written: RefCell::new(Vec::new()),
        }
    }
}

impl test_echo::Server for EchoServer {
    // Not an `async fn`: a method returns its future, so it can panic before there is one.
    fn echo(
        self: Rc<Self>,
        params: test_echo::EchoParams,
        mut results: test_echo::EchoResults,
    ) -> impl Future<Output = Result<(), Error>> + 'static {
        let panics_at_once = params
            .get()
            .and_then(|params| Ok(params.get_text()?.to_str()? == "panic at once"))
            .unwrap_or(false);
        assert!(!panics_at_once, "echo panicked before returning its future");
        async move {
            let text = params.get()?.get_text()?.to_str()?;
            assert!(text != "panic", "echo panicked");
            results
                .get()
                .set_text(format!("{}{text}", self.prefix).as_str());
            Ok(())
        }
    }

    async fn fail(
        self: Rc<Self>,
        params: test_echo::FailParams,
        _results: test_echo::FailResults,
    ) -> Result<(), Error> {
        let description = params.get()?.get_description()?.to_str()?;
        Err(Error::failed(description.to_owned()))
    }

    async fn child(
        self: Rc<Self>,
        params: test_echo::ChildParams,
        mut results: test_echo::ChildResults,
    ) -> Result<(), Error> {
        let prefix = params.get()?.get_prefix()?.to_str()?;
        let child: test_echo::Client = capnp_kj::new_client(
            Self::new(format!("{}{prefix}", self.prefix)),
            capnp_kj::Cancellation::RunToCompletion,
        );
        results.get().set_child(child);
        Ok(())
    }

    async fn call_back(
        self: Rc<Self>,
        params: test_echo::CallBackParams,
        mut results: test_echo::CallBackResults,
    ) -> Result<(), Error> {
        let params = params.get()?;
        let callee = params.get_callee()?;
        let mut request = callee.echo_request();
        request.get().set_text(params.get_text()?);
        let response = request.send().promise.await?;
        results.get().set_text(response.get()?.get_text()?);
        Ok(())
    }

    async fn write(self: Rc<Self>, params: test_echo::WriteParams) -> Result<(), Error> {
        let data = params.get()?.get_data()?;
        match data {
            b"fail" => return Err(Error::failed("write failed".to_owned())),
            b"panic" => panic!("write panicked"),
            b"fail later" => {
                yield_once().await;
                return Err(Error::failed("write failed later".to_owned()));
            }
            b"panic later" => {
                yield_once().await;
                panic!("write panicked later");
            }
            _ => {}
        }
        self.written.borrow_mut().extend_from_slice(data);
        Ok(())
    }

    async fn written(
        self: Rc<Self>,
        _params: test_echo::WrittenParams,
        mut results: test_echo::WrittenResults,
    ) -> Result<(), Error> {
        results.get().set_data(&self.written.borrow());
        Ok(())
    }

    async fn noop(
        self: Rc<Self>,
        _params: test_echo::NoopParams,
        _results: test_echo::NoopResults,
    ) -> Result<(), Error> {
        Ok(())
    }

    async fn hang(
        self: Rc<Self>,
        _params: test_echo::HangParams,
        _results: test_echo::HangResults,
    ) -> Result<(), Error> {
        HANG_STARTS.with(|starts| starts.set(starts.get() + 1));
        let _guard = HangGuard;
        futures::future::pending::<()>().await;
        Ok(())
    }
}

fn new_echo_server(prefix: &str) -> KjOwn<capnp_kj::ClientHook> {
    let client: test_echo::Client = capnp_kj::new_client(
        EchoServer::new(prefix.to_owned()),
        capnp_kj::Cancellation::RunToCompletion,
    );
    capnp_kj::client_into_kj(client)
}

fn new_cancelable_echo_server(prefix: &str) -> KjOwn<capnp_kj::ClientHook> {
    let client: test_echo::Client = capnp_kj::new_client(
        EchoServer::new(prefix.to_owned()),
        capnp_kj::Cancellation::Allow,
    );
    capnp_kj::client_into_kj(client)
}

const CHILD_METHOD_ID: u16 = 2;
const HANG_METHOD_ID: u16 = 6;

/// Like `new_echo_server`, with cancellation decided per method: `hang()` may be canceled if
/// `cancel_hang` is set, and is the only method that may be.
fn new_per_method_echo_server(prefix: &str, cancel_hang: bool) -> KjOwn<capnp_kj::ClientHook> {
    use capnp::traits::HasTypeId;
    let cancellation = if cancel_hang {
        capnp_kj::Cancellation::PerMethod(|interface_id, method_id| {
            interface_id == test_echo::Client::TYPE_ID && method_id == HANG_METHOD_ID
        })
    } else {
        capnp_kj::Cancellation::PerMethod(|interface_id, method_id| {
            interface_id != test_echo::Client::TYPE_ID || method_id != HANG_METHOD_ID
        })
    };
    let client: test_echo::Client =
        capnp_kj::new_client(EchoServer::new(prefix.to_owned()), cancellation);
    capnp_kj::client_into_kj(client)
}

/// Like `new_echo_server`, but its `Cancellation` panics when asked about any method.
fn new_panicking_cancellation_echo_server(prefix: &str) -> KjOwn<capnp_kj::ClientHook> {
    let client: test_echo::Client = capnp_kj::new_client(
        EchoServer::new(prefix.to_owned()),
        capnp_kj::Cancellation::PerMethod(|_, _| panic!("the Cancellation panicked")),
    );
    capnp_kj::client_into_kj(client)
}

thread_local! {
    /// The results `ResultsKeeper` kept. Not in the server: the results hold the call, which holds
    /// the server's client, and so the server.
    static KEPT_RESULTS: RefCell<Vec<test_echo::NoopResults>> = const { RefCell::new(Vec::new()) };
}

/// A server whose `noop()` keeps its results, which outlive the call.
struct ResultsKeeper;

impl test_echo::Server for ResultsKeeper {
    async fn noop(
        self: Rc<Self>,
        _params: test_echo::NoopParams,
        results: test_echo::NoopResults,
    ) -> Result<(), Error> {
        KEPT_RESULTS.with_borrow_mut(|kept| kept.push(results));
        Ok(())
    }
}

fn drop_kept_results() {
    drop(KEPT_RESULTS.take());
}

fn new_results_keeping_server() -> KjOwn<capnp_kj::ClientHook> {
    let client: test_echo::Client =
        capnp_kj::new_client(ResultsKeeper, capnp_kj::Cancellation::RunToCompletion);
    capnp_kj::client_into_kj(client)
}

/// A server whose `callBack()` streams to `callee`, which is meant to be the server itself, so
/// that KJ dispatches one of the server's own `write()` calls inside the poll of its `callBack()`.
#[derive(Default)]
struct SelfStreamer {
    written: RefCell<Vec<u8>>,
    /// Signaled when a `write(b"hang")` call starts.
    hang_started: RefCell<Option<futures::channel::oneshot::Sender<()>>>,
    /// Set while `callBack()` drops a call.
    dropping: Cell<bool>,
    /// Whether a `write()` call ran while `callBack()` dropped a call.
    dispatched_while_dropping: Cell<bool>,
}

impl test_echo::Server for SelfStreamer {
    /// Streams `"hang"` then `"two"` to `callee.write()`, and once the first call has started,
    /// drops it. KJ queues the second call behind the first, a `-> stream` call to a local
    /// capability, and dispatches it while it destroys the first call's promise: inside this
    /// method's poll, when the callee is this server. Returns what `callee.written()` returns,
    /// and whether the second call ran inside the drop.
    async fn call_back(
        self: Rc<Self>,
        params: test_echo::CallBackParams,
        mut results: test_echo::CallBackResults,
    ) -> Result<(), Error> {
        let callee = params.get()?.get_callee()?;
        let (started, hang_started) = futures::channel::oneshot::channel();
        self.hang_started.replace(Some(started));
        let mut first = callee.write_request();
        first.get().set_data(b"hang");
        let first = first.send();
        let mut second = callee.write_request();
        second.get().set_data(b"two");
        let second = second.send();
        hang_started
            .await
            .map_err(|_| Error::failed("the first write() never started".to_owned()))?;
        // KJ queues the second call behind the first in a turn of its event loop that may come
        // after this wakes. A call to another local capability, sent now, is queued after it.
        let other: test_echo::Client = capnp_kj::new_client(
            EchoServer::new(String::new()),
            capnp_kj::Cancellation::RunToCompletion,
        );
        other.noop_request().send().promise.await?;
        self.dropping.set(true);
        drop(first);
        self.dropping.set(false);
        second.await?;
        let response = callee.written_request().send().promise.await?;
        let written = String::from_utf8_lossy(response.get()?.get_data()?).into_owned();
        let text = format!(
            "{written}, dispatched while dropping: {}",
            self.dispatched_while_dropping.get()
        );
        results.get().set_text(text.as_str());
        Ok(())
    }

    async fn write(self: Rc<Self>, params: test_echo::WriteParams) -> Result<(), Error> {
        let data = params.get()?.get_data()?;
        if data == b"hang" {
            if let Some(started) = self.hang_started.take() {
                // The receiver waits for this; it can't be gone.
                let _ = started.send(());
            }
            futures::future::pending::<()>().await;
        }
        if self.dropping.get() {
            self.dispatched_while_dropping.set(true);
        }
        self.written.borrow_mut().extend_from_slice(data);
        Ok(())
    }

    async fn written(
        self: Rc<Self>,
        _params: test_echo::WrittenParams,
        mut results: test_echo::WrittenResults,
    ) -> Result<(), Error> {
        results.get().set_data(&self.written.borrow());
        Ok(())
    }
}

fn new_self_streaming_server() -> KjOwn<capnp_kj::ClientHook> {
    // Canceling the hanging `write()` call drops it, rather than leaving it to hang forever.
    let client: test_echo::Client =
        capnp_kj::new_client(SelfStreamer::default(), capnp_kj::Cancellation::Allow);
    capnp_kj::client_into_kj(client)
}

fn hang_starts() -> u32 {
    HANG_STARTS.with(Cell::get)
}

fn hang_drops() -> u32 {
    HANG_DROPS.with(Cell::get)
}

async fn rust_echo(echo: KjOwn<capnp_kj::ClientHook>, text: String) -> Result<String, KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    async move {
        let mut request = echo.echo_request();
        request.get().set_text(text.as_str());
        let response = request.send().promise.await?;
        Ok(response.get()?.get_text()?.to_string()?)
    }
    .await
    .map_err(capnp_kj::to_kj)
}

/// Calls `echo.child(prefix).echo(text)` without waiting for `child()` to return.
async fn rust_child_echo(
    echo: KjOwn<capnp_kj::ClientHook>,
    prefix: String,
    text: String,
) -> Result<String, KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    async move {
        let mut child_request = echo.child_request();
        child_request.get().set_prefix(prefix.as_str());
        let child = child_request.send().pipeline.get_child();
        let mut request = child.echo_request();
        request.get().set_text(text.as_str());
        let response = request.send().promise.await?;
        Ok(response.get()?.get_text()?.to_string()?)
    }
    .await
    .map_err(capnp_kj::to_kj)
}

/// Calls `echo.child()`, and once it completes, calls `echo()` on the capability pipelined
/// from the first pointer field of the struct at the child's place in the results. There is a
/// capability there, not a struct, so the path leads nowhere. Returns the second call's error.
async fn rust_pipeline_through_cap(echo: KjOwn<capnp_kj::ClientHook>) -> Result<String, KjError> {
    use capnp::traits::HasTypeId;
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    async move {
        let mut request = echo
            .client
            .new_call::<test_echo::child_params::Owned, any_pointer::Owned>(
                test_echo::Client::TYPE_ID,
                CHILD_METHOD_ID,
                None,
            );
        request.get().set_prefix("child:");
        let remote = request.send();
        remote.promise.await?;
        let hook = remote
            .pipeline
            .get_pointer_field(0)
            .get_pointer_field(0)
            .as_cap();
        let nowhere: test_echo::Client = capnp::capability::FromClientHook::new(hook);
        let mut request = nowhere.echo_request();
        request.get().set_text("hello");
        match request.send().promise.await {
            Ok(_) => Err(Error::failed(
                "a call to a capability pipelined from nowhere succeeded".to_owned(),
            )),
            Err(error) => Ok(error.extra),
        }
    }
    .await
    .map_err(capnp_kj::to_kj)
}

/// Takes `echo`, a promise that C++ resolves to a `TestEcho`, and waits for it to resolve, then
/// calls `child(prefix).echo(text)` on what it resolved to, pipelining on `child()`. Fails unless
/// `echo` was unresolved, and resolved once it said so.
async fn rust_resolve_echo(
    echo: KjOwn<capnp_kj::ClientHook>,
    prefix: String,
    text: String,
) -> Result<String, KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    async move {
        if echo.client.hook.get_resolved().is_some()
            || echo.client.hook.when_more_resolved().is_none()
        {
            return Err(Error::failed("the promise was already resolved".to_owned()));
        }
        echo.client.when_resolved().await?;
        let Some(resolved) = echo.client.hook.get_resolved() else {
            return Err(Error::failed(
                "the promise had no resolution once resolved".to_owned(),
            ));
        };
        let resolved: test_echo::Client = capnp::capability::FromClientHook::new(resolved);
        let mut child_request = resolved.child_request();
        child_request.get().set_prefix(prefix.as_str());
        let child = child_request.send().pipeline.get_child();
        let mut request = child.echo_request();
        request.get().set_text(text.as_str());
        let response = request.send().promise.await?;
        Ok(response.get()?.get_text()?.to_string()?)
    }
    .await
    .map_err(capnp_kj::to_kj)
}

/// Calls `echo.noop()`, and reads its results.
async fn rust_noop(echo: KjOwn<capnp_kj::ClientHook>) -> Result<(), KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    async move {
        let response = echo.noop_request().send().promise.await?;
        response.get()?;
        Ok(())
    }
    .await
    .map_err(capnp_kj::to_kj)
}

async fn rust_tail_echo(
    echo: KjOwn<capnp_kj::ClientHook>,
    callee: KjOwn<capnp_kj::ClientHook>,
    text: String,
) -> Result<String, KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    let callee: test_echo::Client = capnp_kj::client_from_kj(callee);
    async move {
        let mut request = echo.tail_echo_request();
        request.get().set_callee(callee);
        request.get().set_text(text.as_str());
        let response = request.send().promise.await?;
        Ok(response.get()?.get_text()?.to_string()?)
    }
    .await
    .map_err(capnp_kj::to_kj)
}

async fn rust_call_back(
    echo: KjOwn<capnp_kj::ClientHook>,
    callee: KjOwn<capnp_kj::ClientHook>,
    text: String,
) -> Result<String, KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    let callee: test_echo::Client = capnp_kj::client_from_kj(callee);
    async move {
        let mut request = echo.call_back_request();
        request.get().set_callee(callee);
        request.get().set_text(text.as_str());
        let response = request.send().promise.await?;
        Ok(response.get()?.get_text()?.to_string()?)
    }
    .await
    .map_err(capnp_kj::to_kj)
}

/// Returns pending once, waking its waker, then completes.
async fn yield_once() {
    let mut yielded = false;
    std::future::poll_fn(|cx| {
        if yielded {
            std::task::Poll::Ready(())
        } else {
            yielded = true;
            cx.waker().wake_by_ref();
            std::task::Poll::Pending
        }
    })
    .await;
}

/// Streams `chunks` to `echo.write()`, then returns `echo.written()`.
async fn rust_write(
    echo: KjOwn<capnp_kj::ClientHook>,
    chunks: Vec<String>,
) -> Result<String, KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    async move {
        for chunk in chunks {
            let mut request = echo.write_request();
            request.get().set_data(chunk.as_bytes());
            request.send().await?;
        }
        let response = echo.written_request().send().promise.await?;
        let data = response.get()?.get_data()?;
        Ok(String::from_utf8_lossy(data).into_owned())
    }
    .await
    .map_err(capnp_kj::to_kj)
}

/// Streams `"one,"` then `"two"` to `echo.write()`, awaiting the two calls in the opposite order,
/// then returns `echo.written()`.
async fn rust_write_awaited_in_reverse(
    echo: KjOwn<capnp_kj::ClientHook>,
) -> Result<String, KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    async move {
        let mut first = echo.write_request();
        first.get().set_data(b"one,");
        let first = first.send();
        let mut second = echo.write_request();
        second.get().set_data(b"two");
        let second = second.send();
        second.await?;
        first.await?;
        let response = echo.written_request().send().promise.await?;
        let data = response.get()?.get_data()?;
        Ok(String::from_utf8_lossy(data).into_owned())
    }
    .await
    .map_err(capnp_kj::to_kj)
}

async fn rust_fail(echo: KjOwn<capnp_kj::ClientHook>, description: String) -> Result<(), KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    async move {
        let mut request = echo.fail_request();
        request.get().set_description(description.as_str());
        request.send().promise.await?;
        Ok(())
    }
    .await
    .map_err(capnp_kj::to_kj)
}

async fn rust_hang(echo: KjOwn<capnp_kj::ClientHook>) -> Result<(), KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    echo.hang_request()
        .send()
        .promise
        .await
        .map(drop)
        .map_err(capnp_kj::to_kj)
}
