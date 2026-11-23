// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use futures::executor::block_on;

use super::*;

#[test]
fn command_records_its_pid() {
    let cmd = wrapped_command(&["ls".to_owned(), "-la".to_owned()], "token");
    assert_eq!(
        cmd,
        [
            "/bin/sh",
            "-c",
            "echo $$ > /tmp/.workerd-exec-token.pid; exec \"$@\"",
            "sh",
            "ls",
            "-la"
        ]
    );
}

/// Records what it is written, and how.
#[derive(Default)]
struct Recorded {
    data: Vec<u8>,
    writes: Vec<usize>,
}

impl Sink for Recorded {
    async fn write(&mut self, data: &[u8]) -> Result<()> {
        self.data.extend_from_slice(data);
        self.writes.push(data.len());
        Ok(())
    }
}

/// A Docker frame of `stream` carrying `payload`.
fn frame(stream: u8, payload: &[u8]) -> Vec<u8> {
    let mut frame = vec![stream, 0, 0, 0];
    frame.extend_from_slice(&u32::try_from(payload.len()).unwrap().to_be_bytes());
    frame.extend_from_slice(payload);
    frame
}

/// Yields its bytes a few at a time, as a socket may.
struct Trickle<'a>(&'a [u8]);

impl AsyncRead for Trickle<'_> {
    fn poll_read(
        mut self: Pin<&mut Self>,
        _: &mut Context<'_>,
        buf: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let n = self.0.len().min(3).min(buf.remaining());
        buf.put_slice(&self.0[..n]);
        self.0 = &self.0[n..];
        Poll::Ready(Ok(()))
    }
}

struct Demuxed {
    result: Result<()>,
    stdout: Recorded,
    stderr: Recorded,
}

fn demux_bytes(input: &[u8], combined: bool) -> Demuxed {
    let mut stdout = Some(Recorded::default());
    let mut stderr = Some(Recorded::default());
    let result = block_on(copy_frames(input, &mut stdout, &mut stderr, combined));
    Demuxed {
        result,
        stdout: stdout.unwrap(),
        stderr: stderr.unwrap(),
    }
}

#[test]
fn demux_splits_stdout_and_stderr() {
    let input = [frame(1, b"out1"), frame(2, b"err"), frame(1, b"out2")].concat();
    let demuxed = demux_bytes(&input, false);
    demuxed.result.unwrap();
    assert_eq!(demuxed.stdout.data, b"out1out2");
    assert_eq!(demuxed.stderr.data, b"err");
}

#[test]
fn demux_combines_stderr_into_stdout() {
    let input = [frame(1, b"out1"), frame(2, b"err"), frame(1, b"out2")].concat();
    let demuxed = demux_bytes(&input, true);
    demuxed.result.unwrap();
    assert_eq!(demuxed.stdout.data, b"out1errout2");
    assert!(demuxed.stderr.writes.is_empty());
}

#[test]
fn demux_drops_unknown_streams_and_empty_frames() {
    // Stream 0 is stdin, which Docker does not send; 3 is not a stream at all.
    let input = [
        frame(0, b"in"),
        frame(1, b""),
        frame(3, b"other"),
        frame(1, b"out"),
    ]
    .concat();
    let demuxed = demux_bytes(&input, false);
    demuxed.result.unwrap();
    assert_eq!(demuxed.stdout.data, b"out");
    assert_eq!(demuxed.stdout.writes, [3]);
    assert!(demuxed.stderr.writes.is_empty());
}

#[test]
fn demux_writes_a_large_frame_in_chunks() {
    let payload: Vec<u8> = (0..DEMUX_CHUNK_SIZE * 2 + 5)
        .map(|i| (i % 251) as u8)
        .collect();
    let input = [frame(1, &payload), frame(2, b"after")].concat();
    let demuxed = demux_bytes(&input, false);
    demuxed.result.unwrap();
    assert_eq!(demuxed.stdout.data, payload);
    assert_eq!(
        demuxed.stdout.writes,
        [DEMUX_CHUNK_SIZE, DEMUX_CHUNK_SIZE, 5]
    );
    assert_eq!(demuxed.stderr.data, b"after");
}

#[test]
fn demux_writes_an_exact_multiple_of_the_chunk_size() {
    let payload = vec![7u8; DEMUX_CHUNK_SIZE * 2];
    // The frame after it shows that its header is read from where the payload ends.
    let input = [frame(1, &payload), frame(2, b"next")].concat();
    let demuxed = demux_bytes(&input, false);
    demuxed.result.unwrap();
    assert_eq!(demuxed.stdout.writes, [DEMUX_CHUNK_SIZE, DEMUX_CHUNK_SIZE]);
    assert_eq!(demuxed.stderr.data, b"next");
}

#[test]
fn demux_reassembles_frames_from_short_reads() {
    let input = [frame(1, b"hello"), frame(2, b"world")].concat();
    let mut stdout = Some(Recorded::default());
    let mut stderr = Some(Recorded::default());
    block_on(copy_frames(
        Trickle(&input),
        &mut stdout,
        &mut stderr,
        false,
    ))
    .unwrap();
    assert_eq!(stdout.unwrap().data, b"hello");
    assert_eq!(stderr.unwrap().data, b"world");
}

#[test]
fn demux_ends_cleanly_without_output() {
    let demuxed = demux_bytes(b"", false);
    demuxed.result.unwrap();
    assert!(demuxed.stdout.writes.is_empty());
}

#[test]
fn demux_fails_on_a_truncated_header() {
    let input = [frame(1, b"out"), vec![1, 0, 0]].concat();
    let demuxed = demux_bytes(&input, false);
    let error = demuxed.result.unwrap_err();
    assert!(
        error
            .description()
            .contains("Docker exec raw stream ended with a truncated frame header")
    );
    // What came before the truncated frame was written.
    assert_eq!(demuxed.stdout.data, b"out");
}

#[test]
fn demux_fails_at_eof_in_a_payload() {
    let mut input = frame(1, b"0123456789");
    input.truncate(input.len() - 4);
    let demuxed = demux_bytes(&input, false);
    let error = demuxed.result.unwrap_err();
    assert!(
        error
            .description()
            .contains("Docker exec raw stream ended in the middle of a frame")
    );
    assert!(demuxed.stdout.writes.is_empty());
}

#[test]
fn demux_without_sinks_reads_and_drops_everything() {
    let input = [frame(1, b"out"), frame(2, b"err")].concat();
    let mut none: Option<Recorded> = None;
    let mut also_none: Option<Recorded> = None;
    block_on(copy_frames(
        input.as_slice(),
        &mut none,
        &mut also_none,
        false,
    ))
    .unwrap();
}

#[test]
fn raw_copy_copies_everything() {
    let input = vec![9u8; 40000];
    let mut stdout = Some(Recorded::default());
    block_on(copy_bytes(input.as_slice(), &mut stdout)).unwrap();
    assert_eq!(stdout.unwrap().data, input);
}
