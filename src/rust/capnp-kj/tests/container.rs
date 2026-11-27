// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A Rust `rpc::Container` server, the shape a Rust port of `server/container-client.c++` would
//! take: it serves `Container`, returns `Port` and `ProcessHandle` capabilities, and writes to
//! `ByteStream` capabilities C++ passes it. container-test.c++ calls it the way
//! `api/container.c++` does.

use std::cell::Cell;
use std::cell::RefCell;

use byte_stream_capnp::byte_stream;
use capnp::Error;
use capnp::capability::Rc;
use container_capnp::container;
use kj_rs::KjOwn;

/// `container.capnp` and `byte-stream.capnp` are `$Cxx.allowCancellation`, so these servers are
/// too.
fn new_client<C, S>(server: S) -> C
where
    C: capnp::capability::FromServer<S>,
{
    capnp_kj::new_client(server, capnp_kj::Cancellation::Allow)
}

#[derive(Default)]
struct ContainerServer {
    running: Cell<bool>,
    signals: RefCell<Vec<u32>>,
}

const SIGKILL: u32 = 9;

impl container::Server for ContainerServer {
    async fn status(
        self: Rc<Self>,
        _params: container::StatusParams,
        mut results: container::StatusResults,
    ) -> Result<(), Error> {
        results.get().set_running(self.running.get());
        Ok(())
    }

    async fn start(
        self: Rc<Self>,
        _params: container::StartParams,
        _results: container::StartResults,
    ) -> Result<(), Error> {
        if self.running.replace(true) {
            return Err(Error::failed("container is already running".to_owned()));
        }
        Ok(())
    }

    async fn signal(
        self: Rc<Self>,
        params: container::SignalParams,
        _results: container::SignalResults,
    ) -> Result<(), Error> {
        let signo = params.get()?.get_signo();
        self.signals.borrow_mut().push(signo);
        if signo == SIGKILL {
            self.running.set(false);
        }
        Ok(())
    }

    async fn get_tcp_port(
        self: Rc<Self>,
        params: container::GetTcpPortParams,
        mut results: container::GetTcpPortResults,
    ) -> Result<(), Error> {
        let port = params.get()?.get_port();
        results
            .get()
            .set_port(new_client::<container::port::Client, _>(PortServer {
                port,
            }));
        Ok(())
    }

    async fn exec(
        self: Rc<Self>,
        params: container::ExecParams,
        mut results: container::ExecResults,
    ) -> Result<(), Error> {
        let params = params.get()?;
        let mut line = Vec::new();
        for arg in params.get_cmd()? {
            line.push(arg?.to_str()?.to_owned());
        }
        let output = format!("{}\n", line.join(" "));
        if params.has_stdout_writer() {
            let stdout = params.get_stdout_writer()?;
            write_all(&stdout, output.as_bytes()).await?;
            stdout.end_request().send().promise.await?;
        }
        let mut process = results.get().init_process();
        process.set_pid(42);
        process.set_handle(new_client::<container::process_handle::Client, _>(
            ProcessHandleServer,
        ));
        Ok(())
    }
}

/// Writes `bytes` to `stream` in small chunks, each a `-> stream` call.
async fn write_all(stream: &byte_stream::Client, bytes: &[u8]) -> Result<(), Error> {
    for chunk in bytes.chunks(4) {
        let mut request = stream.write_request();
        request.get().set_bytes(chunk);
        request.send().await?;
    }
    Ok(())
}

struct PortServer {
    port: u16,
}

impl container::port::Server for PortServer {
    async fn connect(
        self: Rc<Self>,
        params: container::port::ConnectParams,
        mut results: container::port::ConnectResults,
    ) -> Result<(), Error> {
        let down = params.get()?.get_down()?;
        let prefix = format!("{}:", self.port);
        write_all(&down, prefix.as_bytes()).await?;
        results
            .get()
            .set_up(new_client::<byte_stream::Client, _>(EchoStream { down }));
        Ok(())
    }
}

/// A connection's upstream: writes everything it receives back down.
struct EchoStream {
    down: byte_stream::Client,
}

impl byte_stream::Server for EchoStream {
    async fn write(self: Rc<Self>, params: byte_stream::WriteParams) -> Result<(), Error> {
        write_all(&self.down, params.get()?.get_bytes()?).await
    }

    async fn end(
        self: Rc<Self>,
        _params: byte_stream::EndParams,
        _results: byte_stream::EndResults,
    ) -> Result<(), Error> {
        self.down.end_request().send().promise.await?;
        Ok(())
    }
}

struct ProcessHandleServer;

impl container::process_handle::Server for ProcessHandleServer {
    async fn wait(
        self: Rc<Self>,
        _params: container::process_handle::WaitParams,
        mut results: container::process_handle::WaitResults,
    ) -> Result<(), Error> {
        results.get().set_exit_code(0);
        Ok(())
    }
}

pub fn new_container_server() -> KjOwn<capnp_kj::ClientHook> {
    capnp_kj::client_into_kj(new_client::<container::Client, _>(
        ContainerServer::default(),
    ))
}
