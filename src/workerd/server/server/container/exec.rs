// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `exec()` and the process it starts.
//!
//! Docker hijacks the connection of `POST /exec/<id>/start`: stdin goes up it, and the output
//! comes down it in frames (an 8-byte header naming the stream and the payload's length), or raw
//! with a PTY, which merges stderr into stdout. The output is always read, to its end, which is
//! how `wait()` knows the process ended; output the Worker ignores is dropped.
//!
//! The command runs under a shell that first records its PID in the container, in a file named
//! by a random token, so that `kill()` signals the process by the PID it has in the
//! container's own namespace (Docker reports only the host's).

use std::cell::Cell;
use std::cell::RefCell;
use std::io;
use std::pin::Pin;
use std::rc::Rc;
use std::task::Context;
use std::task::Poll;
use std::time::Duration;

use futures::FutureExt;
use futures::future::RemoteHandle;
use futures::future::Shared;
use http::Method;
use kj_hyper::io_kj_error;
use kj_rs::KjOwn;
use tokio::io::AsyncRead;
use tokio::io::AsyncReadExt;
use tokio::io::AsyncWrite;
use tokio::io::AsyncWriteExt;
use tokio::io::ReadBuf;
use tokio::io::ReadHalf;
use tokio::io::WriteHalf;

use super::api;
use super::client::ContainerClient;
use super::client::EXEC_POLL_ATTEMPTS;
use super::client::EXEC_POLL_INTERVAL;
use super::client::js_error;
use super::client::random_uuid;
use super::docker::Hijacked;
use super::docker::Upgrade;
use crate::Error;
use crate::Result;
use crate::bridge::ffi;
use crate::config::Factory;

/// A stream the process's output goes to: a Worker's `ByteStream`.
pub type Output = KjOwn<ffi::ExecOutput>;

/// The exit code of the kill helper when the PID file is still empty. `kill` itself exits with 1
/// or 2 when it fails.
const PID_NOT_RECORDED_EXIT_CODE: i32 = 3;
/// How long `kill()` waits for a process to record its PID, and the backoff between its tries.
const PID_RECORD_TIMEOUT: Duration = Duration::from_secs(5);
const PID_RETRY_INITIAL_DELAY: Duration = Duration::from_millis(10);
const PID_RETRY_MAX_DELAY: Duration = Duration::from_millis(500);

/// What `exec()` is given.
pub struct ExecParams {
    pub cmd: Vec<String>,
    pub env: Option<Vec<String>>,
    pub working_directory: Option<String>,
    pub user: Option<String>,
    pub combined_output: bool,
    /// The terminal's columns and rows, with a PTY.
    pub pty: Option<(u16, u16)>,
}

/// Where a process records its PID in the container.
#[must_use]
pub fn pid_file(kill_token: &str) -> String {
    format!("/tmp/.workerd-exec-{kill_token}.pid")
}

/// The command Docker runs for `cmd`: a shell that records its PID and becomes `cmd`, with
/// `cmd`'s argv unchanged.
#[must_use]
pub fn wrapped_command(cmd: &[String], kill_token: &str) -> Vec<String> {
    let mut wrapped = vec![
        "/bin/sh".to_owned(),
        "-c".to_owned(),
        format!("echo $$ > {}; exec \"$@\"", pid_file(kill_token)),
        // argv[0] of the shell; "$@" starts after it.
        "sh".to_owned(),
    ];
    wrapped.extend(cmd.iter().cloned());
    wrapped
}

struct ExecState {
    exit_code: i32,
    running: bool,
    pid: u32,
}

impl ContainerClient {
    async fn create_exec(&self, params: &ExecParams, kill_token: &str) -> Result<String> {
        let request = api::ExecCreateRequest {
            attach_stdin: Some(true),
            // Attached even when the Worker ignores them: the hijacked stream then lasts as long
            // as the process.
            attach_stdout: Some(true),
            attach_stderr: Some(true),
            tty: Some(params.pty.is_some()),
            cmd: wrapped_command(&params.cmd, kill_token),
            env: params.env.clone(),
            working_dir: params.working_directory.clone(),
            user: params.user.clone(),
        };
        let body = serde_json::to_string(&request).map_err(|e| kj::failed!("{e}"))?;
        let response = self
            .docker
            .json(
                Method::POST,
                &format!("/containers/{}/exec", self.name),
                Some(body),
            )
            .await?;
        if response.status != 201 {
            return Err(js_error(format_args!(
                "Creating Docker exec failed with [{}] {}",
                response.status,
                response.text()
            )));
        }
        Ok(response.json::<api::ExecCreateResponse>()?.id)
    }

    async fn start_exec(&self, id: &str, tty: bool, cols: u16, rows: u16) -> Result<Hijacked> {
        let request = api::ExecStartRequest {
            detach: Some(false),
            tty: Some(tty),
            // A dimension of 0 is Docker's default, so either may be given alone. Docker takes
            // [height, width].
            console_size: (tty && (cols != 0 || rows != 0)).then_some([rows, cols]),
        };
        let body = serde_json::to_string(&request).map_err(|e| kj::failed!("{e}"))?;
        match self
            .docker
            .upgrade(&format!("/exec/{id}/start"), body)
            .await?
        {
            Upgrade::Hijacked(io) => Ok(io),
            Upgrade::Refused {
                status,
                status_text,
                body,
            } => Err(js_error(format_args!(
                "Starting Docker exec failed with [{status}] {status_text} {body}"
            ))),
        }
    }

    async fn inspect_exec(&self, id: &str) -> Result<ExecState> {
        let response = self.docker.get(&format!("/exec/{id}/json")).await?;
        if response.status != 200 {
            return Err(js_error(format_args!(
                "Inspecting Docker exec failed with [{}] {}",
                response.status,
                response.text()
            )));
        }
        let root: api::ExecInspectResponse = response.json()?;
        #[expect(
            clippy::cast_possible_truncation,
            reason = "an exit code is a small integer"
        )]
        let exit_code = root
            .exit_code
            .and_then(|code| code.as_f64())
            .map_or(0, |code| code as i32);
        Ok(ExecState {
            exit_code,
            running: root.running,
            pid: root.pid,
        })
    }

    async fn resize_exec(&self, id: &str, cols: u16, rows: u16) -> Result<()> {
        if cols == 0 || rows == 0 {
            kj::fail_require!("PTY resize dimensions must be non-zero.");
        }
        let response = self
            .docker
            .json(
                Method::POST,
                &format!("/exec/{id}/resize?h={rows}&w={cols}"),
                None,
            )
            .await?;
        if matches!(response.status, 200 | 201) {
            return Ok(());
        }
        Err(js_error(format_args!(
            "Resizing Docker exec failed with [{}] {}",
            response.status,
            response.text()
        )))
    }

    /// Runs `cmd` in the container, detached, and waits for its exit code.
    async fn run_simple_exec(&self, cmd: Vec<String>) -> Result<i32> {
        let request = api::ExecCreateRequest {
            attach_stdin: Some(false),
            attach_stdout: Some(false),
            attach_stderr: Some(false),
            tty: Some(false),
            cmd,
            ..api::ExecCreateRequest::default()
        };
        let body = serde_json::to_string(&request).map_err(|e| kj::failed!("{e}"))?;
        let created = self
            .docker
            .json(
                Method::POST,
                &format!("/containers/{}/exec", self.name),
                Some(body),
            )
            .await?;
        if created.status != 201 {
            return Err(js_error(format_args!(
                "Creating helper Docker exec failed with [{}] {}",
                created.status,
                created.text()
            )));
        }
        let id = created.json::<api::ExecCreateResponse>()?.id;

        let start = api::ExecStartRequest {
            detach: Some(true),
            tty: Some(false),
            console_size: None,
        };
        let body = serde_json::to_string(&start).map_err(|e| kj::failed!("{e}"))?;
        let started = self
            .docker
            .json(Method::POST, &format!("/exec/{id}/start"), Some(body))
            .await?;
        if started.status != 200 {
            return Err(js_error(format_args!(
                "Starting helper Docker exec failed with [{}] {}",
                started.status,
                started.text()
            )));
        }

        loop {
            let state = self.inspect_exec(&id).await?;
            if !state.running {
                return Ok(state.exit_code);
            }
            self.factory().sleep(EXEC_POLL_INTERVAL).await;
        }
    }

    pub async fn exec(
        &self,
        params: ExecParams,
        stdout: Option<Output>,
        stderr: Option<Output>,
    ) -> Result<Process> {
        let _turn = self.turn().await;
        if !self.is_started() {
            return Err(js_error("exec() requires a running container."));
        }

        let kill_token = random_uuid();
        let id = self.create_exec(&params, &kill_token).await?;
        let (cols, rows) = params.pty.unwrap_or((0, 0));
        let connection = self
            .start_exec(&id, params.pty.is_some(), cols, rows)
            .await?;

        // Docker can report an exec as not running before it has spawned the process: the start
        // returns first.
        let mut state = ExecState {
            exit_code: 0,
            running: false,
            pid: 0,
        };
        for attempt in 0..EXEC_POLL_ATTEMPTS {
            state = self.inspect_exec(&id).await?;
            if state.pid != 0 || !state.running || attempt + 1 == EXEC_POLL_ATTEMPTS {
                break;
            }
            self.factory().sleep(EXEC_POLL_INTERVAL).await;
        }

        let Some(client) = self.rc() else {
            kj::fail_require!("container client dropped during exec()");
        };
        #[expect(
            clippy::cast_possible_wrap,
            reason = "Docker's pid fits in rpc::Container::Process's Int32 pid"
        )]
        let pid = state.pid as i32;
        Ok(Process::new(
            client,
            id,
            kill_token,
            pid,
            connection,
            stdout,
            stderr,
            params.combined_output,
            params.pty.is_some(),
        ))
    }
}

// =======================================================================================
// Output

/// Ends `output` in the background: the Worker may never read it.
fn detach_end(factory: &Factory, output: Option<Output>) {
    if let Some(output) = output {
        factory.spawn_detached(async move {
            let _ = ffi::exec_output_end(output).await;
        });
    }
}

fn abort(output: Option<&mut Output>, error: &Error) {
    if let Some(output) = output {
        ffi::exec_output_abort(output.as_mut(), cxx::KjException::from(error.clone()));
    }
}

/// Reads until `buffer` is full or the stream ends; the count read.
async fn read_full(input: &mut (impl AsyncRead + Unpin), buffer: &mut [u8]) -> Result<usize> {
    let mut filled = 0;
    while filled < buffer.len() {
        let n = input
            .read(&mut buffer[filled..])
            .await
            .map_err(|e| io_kj_error(&e))?;
        if n == 0 {
            break;
        }
        filled += n;
    }
    Ok(filled)
}

/// Docker's frame header: the stream (1 stdout, 2 stderr), 3 reserved bytes, the payload's
/// length (big-endian).
const FRAME_HEADER_SIZE: usize = 8;
/// The most of a frame's payload `demux` holds at once.
const DEMUX_CHUNK_SIZE: usize = 64 * 1024;

/// Where the copies of a process's output write it.
trait Sink {
    async fn write(&mut self, data: &[u8]) -> Result<()>;
}

impl Sink for Output {
    async fn write(&mut self, data: &[u8]) -> Result<()> {
        ffi::exec_output_write(self.as_mut(), data).await?;
        Ok(())
    }
}

/// Copies Docker's framed output to `stdout` and `stderr` (stderr to stdout when
/// `combined`), then ends them; a failure aborts them.
pub async fn demux(
    factory: &Factory,
    input: impl AsyncRead + Unpin,
    mut stdout: Option<Output>,
    mut stderr: Option<Output>,
    combined: bool,
) -> Result<()> {
    match copy_frames(input, &mut stdout, &mut stderr, combined).await {
        Ok(()) => {
            detach_end(factory, stdout);
            detach_end(factory, stderr);
            Ok(())
        }
        Err(e) => {
            abort(stdout.as_mut(), &e);
            abort(stderr.as_mut(), &e);
            Err(e)
        }
    }
}

/// Copies Docker's framed output until it ends: each frame's payload to the sink of its stream,
/// in pieces of at most `DEMUX_CHUNK_SIZE` bytes. A frame of an unknown stream, or of one with
/// no sink, is read and dropped.
async fn copy_frames<S: Sink>(
    mut input: impl AsyncRead + Unpin,
    stdout: &mut Option<S>,
    stderr: &mut Option<S>,
    combined: bool,
) -> Result<()> {
    let mut header = [0u8; FRAME_HEADER_SIZE];
    let mut buffer = vec![0u8; DEMUX_CHUNK_SIZE];
    loop {
        let n = read_full(&mut input, &mut header).await?;
        if n == 0 {
            return Ok(());
        }
        if n < FRAME_HEADER_SIZE {
            kj::fail_require!("Docker exec raw stream ended with a truncated frame header");
        }
        let mut remaining =
            u32::from_be_bytes([header[4], header[5], header[6], header[7]]) as usize;
        // The payload goes out in bounded pieces, whatever length the header claims.
        while remaining > 0 {
            let piece = &mut buffer[..remaining.min(DEMUX_CHUNK_SIZE)];
            if read_full(&mut input, piece).await? < piece.len() {
                kj::fail_require!("Docker exec raw stream ended in the middle of a frame");
            }
            remaining -= piece.len();
            let target = match header[0] {
                1 => stdout.as_mut(),
                2 if combined => stdout.as_mut(),
                2 => stderr.as_mut(),
                _ => None,
            };
            if let Some(sink) = target {
                sink.write(piece).await?;
            }
        }
    }
}

/// Copies a PTY's raw output to `stdout`, then ends it; a failure aborts it.
pub async fn copy_raw(
    factory: &Factory,
    input: impl AsyncRead + Unpin,
    mut stdout: Option<Output>,
) -> Result<()> {
    match copy_bytes(input, &mut stdout).await {
        Ok(()) => {
            detach_end(factory, stdout);
            Ok(())
        }
        Err(e) => {
            abort(stdout.as_mut(), &e);
            Err(e)
        }
    }
}

/// Copies `input` to `sink` (or drops it, without one) until it ends.
async fn copy_bytes<S: Sink>(
    mut input: impl AsyncRead + Unpin,
    sink: &mut Option<S>,
) -> Result<()> {
    let mut buffer = vec![0u8; 16384];
    loop {
        let n = input.read(&mut buffer).await.map_err(|e| io_kj_error(&e))?;
        if n == 0 {
            return Ok(());
        }
        if let Some(sink) = sink.as_mut() {
            sink.write(&buffer[..n]).await?;
        }
    }
}

// =======================================================================================
// The process

/// A process's stdin, as the Worker writes it: the write half of the hijacked connection, which
/// reads nothing.
struct Stdin(WriteHalf<Hijacked>);

impl AsyncRead for Stdin {
    fn poll_read(
        self: Pin<&mut Self>,
        _: &mut Context<'_>,
        _: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        Poll::Ready(Ok(()))
    }
}

impl AsyncWrite for Stdin {
    fn poll_write(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buf: &[u8],
    ) -> Poll<io::Result<usize>> {
        Pin::new(&mut self.get_mut().0).poll_write(cx, buf)
    }

    fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.get_mut().0).poll_flush(cx)
    }

    fn poll_shutdown(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<io::Result<()>> {
        Pin::new(&mut self.get_mut().0).poll_shutdown(cx)
    }
}

/// A process `exec()` started: `rpc::Container::ProcessHandle`. It keeps its client alive, and
/// reading its output ends when it is dropped.
pub struct Process {
    client: Rc<ContainerClient>,
    id: String,
    kill_token: String,
    pid: i32,
    pty: bool,
    /// Until the Worker takes it, or `wait()` ends it.
    stdin: RefCell<Option<WriteHalf<Hijacked>>>,
    stdin_opened: Cell<bool>,
    wait_started: Cell<bool>,
    /// Reading the output, to its end.
    output: Shared<RemoteHandle<Result<()>>>,
}

impl Process {
    #[expect(
        clippy::too_many_arguments,
        reason = "the parts of a process, from exec() alone"
    )]
    fn new(
        client: Rc<ContainerClient>,
        id: String,
        kill_token: String,
        pid: i32,
        connection: Hijacked,
        stdout: Option<Output>,
        stderr: Option<Output>,
        combined: bool,
        pty: bool,
    ) -> Self {
        let (read, write): (ReadHalf<Hijacked>, WriteHalf<Hijacked>) = tokio::io::split(connection);
        let factory = Rc::clone(client.factory());
        let keep = Rc::clone(&client);
        let output = factory
            .spawn(async move {
                // The client outlives the copy.
                let client = keep;
                let factory = Rc::clone(client.factory());
                if pty {
                    copy_raw(&factory, read, stdout).await
                } else {
                    demux(&factory, read, stdout, stderr, combined).await
                }
            })
            .shared();
        Self {
            client,
            id,
            kill_token,
            pid,
            pty,
            stdin: RefCell::new(Some(write)),
            stdin_opened: Cell::new(false),
            wait_started: Cell::new(false),
            output,
        }
    }

    #[must_use]
    pub fn pid(&self) -> i32 {
        self.pid
    }

    pub async fn wait(&self) -> Result<i32> {
        self.wait_started.set(true);
        // stdin the Worker never took ends here.
        let stdin = self.stdin.borrow_mut().take();
        if let Some(mut stdin) = stdin {
            let _ = stdin.shutdown().await;
        }

        self.output.clone().await?;

        // Docker's exec state can lag behind the stream's end.
        for attempt in 0..EXEC_POLL_ATTEMPTS {
            let state = self.client.inspect_exec(&self.id).await?;
            if !state.running {
                return Ok(state.exit_code);
            }
            if attempt + 1 < EXEC_POLL_ATTEMPTS {
                self.client.factory().sleep(EXEC_POLL_INTERVAL).await;
            }
        }
        Err(js_error(
            "Docker exec stream closed before exit status became available.",
        ))
    }

    /// The process's stdin, as a stream whose `shutdownWrite()` ends it.
    pub fn stdin_writer(&self) -> Result<KjOwn<ffi::AsyncIoStream>> {
        if self.wait_started.get() {
            return Err(js_error(
                "Process stdinWriter() cannot be called after wait().",
            ));
        }
        if self.stdin_opened.replace(true) {
            return Err(js_error("Process stdinWriter() can only be called once."));
        }
        let Some(stdin) = self.stdin.borrow_mut().take() else {
            kj::fail_require!("the process's stdin is gone");
        };
        Ok(kj_hyper::into_kj_stream(Stdin(stdin)))
    }

    /// Signals the process by the PID it recorded, waiting for it to record one: `exec()` returns
    /// once Docker started the exec, which can be before the shell wrote the PID. The shell
    /// creates the file empty and writes the PID with one `write()`, so a non-empty file holds
    /// all of it.
    pub async fn kill(&self, signo: u32) -> Result<()> {
        let pid_file = pid_file(&self.kill_token);
        let command = vec![
            "/bin/sh".to_owned(),
            "-c".to_owned(),
            format!(
                "[ -s {pid_file} ] || exit {PID_NOT_RECORDED_EXIT_CODE}; kill -{signo} \"$(cat \
                 {pid_file})\""
            ),
        ];
        let factory = self.client.factory();
        let deadline = factory.now() + PID_RECORD_TIMEOUT;
        let mut delay = PID_RETRY_INITIAL_DELAY;
        loop {
            let exit_code = self.client.run_simple_exec(command.clone()).await?;
            if exit_code == 0 {
                return Ok(());
            }
            if exit_code != PID_NOT_RECORDED_EXIT_CODE {
                return Err(js_error(format_args!(
                    "Helper Docker exec failed with exit code {exit_code}"
                )));
            }
            let now = factory.now();
            if now >= deadline {
                return Err(js_error(
                    "Cannot signal the process: it did not record its PID within 5s.",
                ));
            }
            factory.sleep(delay.min(deadline.saturating_sub(now))).await;
            delay = (delay * 2).min(PID_RETRY_MAX_DELAY);
        }
    }

    pub async fn resize(&self, cols: u16, rows: u16) -> Result<()> {
        if !self.pty {
            return Err(js_error(
                "resize() can only be called on a process that was started with a PTY.",
            ));
        }
        self.client.resize_exec(&self.id, cols, rows).await
    }
}

#[cfg(test)]
#[path = "exec-test.rs"]
mod tests;
