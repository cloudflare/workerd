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
    async fn echo(
        self: Rc<Self>,
        params: test_echo::EchoParams,
        mut results: test_echo::EchoResults,
    ) -> Result<(), Error> {
        let text = params.get()?.get_text()?.to_str()?;
        results
            .get()
            .set_text(format!("{}{text}", self.prefix).as_str());
        Ok(())
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
        let child: test_echo::Client =
            capnp_kj::new_client(Self::new(format!("{}{prefix}", self.prefix)));
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
            b"fail later" => {
                yield_once().await;
                return Err(Error::failed("write failed later".to_owned()));
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

    async fn bounce(
        self: Rc<Self>,
        params: test_echo::BounceParams,
        mut results: test_echo::BounceResults,
    ) -> Result<(), Error> {
        results.get().set_data(params.get()?.get_data()?);
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
    let client: test_echo::Client = capnp_kj::new_client(EchoServer::new(prefix.to_owned()));
    capnp_kj::client_into_kj(client)
}

fn new_cancelable_echo_server(prefix: &str) -> KjOwn<capnp_kj::ClientHook> {
    let client: test_echo::Client = capnp_kj::new_client_with_cancellation(
        EchoServer::new(prefix.to_owned()),
        capnp_kj::Cancellation::Allow,
    );
    capnp_kj::client_into_kj(client)
}

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
        capnp_kj::new_client_with_cancellation(EchoServer::new(prefix.to_owned()), cancellation);
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

/// Calls `echo.bounce()` `count` times in a row with `size` bytes, for benchmarks. Looping here
/// keeps the cost of entering Rust from C++ out of the per-call figure.
async fn rust_bounce_n(
    echo: KjOwn<capnp_kj::ClientHook>,
    size: usize,
    count: u32,
) -> Result<(), KjError> {
    let echo: test_echo::Client = capnp_kj::client_from_kj(echo);
    let data = vec![0x5a_u8; size];
    async move {
        for _ in 0..count {
            let mut request = echo.bounce_request();
            request.get().set_data(&data);
            let response = request.send().promise.await?;
            if response.get()?.get_data()?.len() != size {
                return Err(Error::failed("bounce() returned the wrong size".to_owned()));
            }
        }
        Ok(())
    }
    .await
    .map_err(capnp_kj::to_kj)
}
