// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use serde_json::Value;
use serde_json::json;

use super::*;
use crate::IsolateState;
use crate::Tracker;

/// An in-memory output that stays readable after the writer takes it.
#[derive(Clone, Default)]
struct SharedBuf(Arc<Mutex<Vec<u8>>>);

impl SharedBuf {
    fn lines(&self) -> Vec<Value> {
        let bytes = self.0.lock().unwrap().clone();
        let text = String::from_utf8(bytes).unwrap();
        assert!(text.is_empty() || text.ends_with('\n'));
        text.lines()
            .map(|line| serde_json::from_str(line).unwrap())
            .collect()
    }
}

impl Write for SharedBuf {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        self.0.lock().unwrap().extend_from_slice(buf);
        Ok(buf.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// Fails every write.
struct Broken;

impl Write for Broken {
    fn write(&mut self, _buf: &[u8]) -> io::Result<usize> {
        Err(io::Error::other("broken"))
    }

    fn flush(&mut self) -> io::Result<()> {
        Err(io::Error::other("broken"))
    }
}

/// Fails after the first `ok` writes.
struct FailsAfter {
    ok: usize,
    out: SharedBuf,
}

impl Write for FailsAfter {
    fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        if self.ok == 0 {
            return Err(io::Error::other("full"));
        }
        self.ok -= 1;
        self.out.write(buf)
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

#[derive(Clone, Copy)]
struct FixedClock(Nanos);

impl crate::Clock for FixedClock {
    fn now(&self) -> Nanos {
        self.0
    }
}

fn writer(buf: &SharedBuf) -> Arc<NdjsonWriter> {
    Arc::new(NdjsonWriter::new(Box::new(buf.clone()), "test", "1.2.3").unwrap())
}

#[test]
fn header() {
    let buf = SharedBuf::default();
    let _writer = writer(&buf);
    let lines = buf.lines();
    assert_eq!(lines.len(), 1);
    let header = &lines[0];
    assert_eq!(header["e"], "header");
    assert_eq!(header["v"], FORMAT_VERSION);
    assert_eq!(header["producer"], "test");
    assert_eq!(header["version"], "1.2.3");
    assert_eq!(header["pid"], std::process::id());
    assert_eq!(header["epochUnixMs"], crate::epoch_unix_ms());
}

#[test]
fn header_write_error_is_returned() {
    assert!(NdjsonWriter::new(Box::new(Broken), "test", "1").is_err());
}

#[test]
fn every_event_type() {
    let buf = SharedBuf::default();
    let isolate = Arc::new(IsolateState::new());
    let iso = isolate.id();
    let mut tracker = Tracker::new(isolate, "svc", Some("abc"), Box::new(FixedClock(42)));
    let ctx = tracker.context_id();
    tracker.add_sink(Box::new(NdjsonSink::new(writer(&buf))));

    let request = tracker.create(Kind::Request, "fetch", 0, None);
    tracker.turn_begin(0);
    tracker.turn_locked();
    tracker.set_turn_cause(request);
    tracker.begin_stack();
    tracker.push_frame("handle", "worker.js", 9, 4, 2);
    let stack = tracker.end_stack();
    let op = tracker.create(Kind::Operation, "kv_get", 0, stack);
    tracker.annotate(op, "key", "\"quoted\"\n");
    tracker.turn_end();
    tracker.settle(op, Outcome::Canceled);
    let timer = tracker.create(Kind::Timer, "setTimeout", 0, None);
    tracker.destroy(timer);
    let stack = stack.unwrap();
    drop(tracker);

    let lines = buf.lines();
    assert_eq!(
        lines[1..],
        [
            json!({"e": "ctx", "ctx": ctx, "iso": iso, "worker": "svc", "actor": "abc", "at": 42}),
            json!({"e": "init", "ctx": ctx, "id": request, "kind": "request", "name": "fetch",
                   "trigger": 0, "exec": 0, "at": 42}),
            json!({"e": "before", "ctx": ctx, "id": request, "at": 42}),
            json!({"e": "stack", "iso": iso, "id": stack, "frames": [
                {"fn": "handle", "script": "worker.js", "scriptId": 9, "line": 4, "col": 2}]}),
            json!({"e": "init", "ctx": ctx, "id": op, "kind": "operation", "name": "kv_get",
                   "trigger": request, "exec": request, "at": 42, "stack": stack}),
            json!({"e": "annotate", "ctx": ctx, "id": op, "k": "key", "v": "\"quoted\"\n"}),
            json!({"e": "after", "ctx": ctx, "id": request, "at": 42}),
            json!({"e": "turn", "ctx": ctx, "cause": request, "start": 42, "locked": 42,
                   "end": 42}),
            json!({"e": "settle", "ctx": ctx, "id": op, "outcome": "canceled", "at": 42}),
            json!({"e": "init", "ctx": ctx, "id": timer, "kind": "timer", "name": "setTimeout",
                   "trigger": 0, "exec": 0, "at": 42}),
            json!({"e": "destroy", "ctx": ctx, "id": timer, "at": 42}),
            json!({"e": "ctx_end", "ctx": ctx, "at": 42, "created": 3, "dropped": 0,
                   "unknown": 0, "unbalanced": 0, "ambiguousBindings": 0, "unusedOperationNames": 0,
                   "foreignThread": 0}),
        ]
    );
}

#[test]
fn optional_fields_are_omitted_or_null() {
    let buf = SharedBuf::default();
    let mut tracker = Tracker::new(
        Arc::new(IsolateState::new()),
        "svc",
        None,
        Box::new(FixedClock(1)),
    );
    tracker.add_sink(Box::new(NdjsonSink::new(writer(&buf))));
    tracker.turn_begin(0);
    tracker.turn_end();
    let lines = buf.lines();
    assert_eq!(lines[1]["actor"], Value::Null);
    let turn = lines[2].as_object().unwrap();
    assert_eq!(turn["e"], "turn");
    assert!(!turn.contains_key("locked"));
}

#[test]
fn sink_buffers_until_turn_end() {
    let buf = SharedBuf::default();
    let mut tracker = Tracker::new(
        Arc::new(IsolateState::new()),
        "svc",
        None,
        Box::new(FixedClock(1)),
    );
    tracker.add_sink(Box::new(NdjsonSink::new(writer(&buf))));
    tracker.turn_begin(0);
    tracker.create(Kind::Timer, "setTimeout", 0, None);
    assert_eq!(buf.lines().len(), 1); // Header only.
    tracker.turn_end();
    let kinds: Vec<Value> = buf.lines().iter().map(|line| line["e"].clone()).collect();
    assert_eq!(kinds, ["header", "ctx", "init", "turn"]);
}

#[test]
fn sink_writes_out_early_when_its_buffer_is_large() {
    let buf = SharedBuf::default();
    let mut tracker = Tracker::new(
        Arc::new(IsolateState::new()),
        "svc",
        None,
        Box::new(FixedClock(1)),
    );
    tracker.add_sink(Box::new(NdjsonSink::new(writer(&buf))));
    tracker.turn_begin(0);
    let name = "x".repeat(1024);
    for _ in 0..80 {
        tracker.create(Kind::Timer, &name, 0, None);
    }
    assert!(buf.lines().len() > 1);
    tracker.turn_end();
}

#[test]
fn contexts_sharing_a_writer_produce_whole_lines() {
    let buf = SharedBuf::default();
    let writer = writer(&buf);
    let isolate = Arc::new(IsolateState::new());
    let mut a = Tracker::new(Arc::clone(&isolate), "a", None, Box::new(FixedClock(1)));
    let mut b = Tracker::new(isolate, "b", None, Box::new(FixedClock(1)));
    a.add_sink(Box::new(NdjsonSink::new(Arc::clone(&writer))));
    b.add_sink(Box::new(NdjsonSink::new(Arc::clone(&writer))));
    a.turn_begin(0);
    b.turn_begin(0);
    a.create(Kind::Timer, "a", 0, None);
    b.create(Kind::Timer, "b", 0, None);
    b.turn_end();
    a.turn_end();
    drop(a);
    drop(b);
    let ctxs: Vec<(Value, Value)> = buf.lines()[1..]
        .iter()
        .map(|line| (line["e"].clone(), line["ctx"].clone()))
        .collect();
    // Each sink's lines arrive together, in its own order.
    assert_eq!(ctxs.len(), 8);
    assert_eq!(ctxs[0].1, ctxs[1].1);
    assert_eq!(ctxs[0].1, ctxs[2].1);
}

#[test]
fn io_error_disables_the_writer() {
    let out = SharedBuf::default();
    let writer = Arc::new(
        NdjsonWriter::new(
            Box::new(FailsAfter {
                ok: 1,
                out: out.clone(),
            }),
            "test",
            "1",
        )
        .unwrap(),
    );
    assert!(!writer.failed());
    writer.append(b"{\"e\":\"x\"}\n");
    assert!(writer.failed());
    writer.append(b"{\"e\":\"y\"}\n");
    writer.flush();
    assert!(writer.failed());
    // Only the header made it.
    assert_eq!(out.lines().len(), 1);
}

#[test]
fn create_writes_a_file() {
    let dir = std::env::var("TEST_TMPDIR").map_or_else(|_| std::env::temp_dir(), Into::into);
    let path = dir.join("async-trace-ndjson-test.ndjson");
    let path = path.to_str().unwrap();
    {
        let writer = Arc::new(NdjsonWriter::create(path, "test", "9").unwrap());
        let mut tracker = Tracker::new(
            Arc::new(IsolateState::new()),
            "svc",
            None,
            Box::new(FixedClock(1)),
        );
        tracker.add_sink(Box::new(NdjsonSink::new(writer)));
    }
    let text = std::fs::read_to_string(path).unwrap();
    let kinds: Vec<String> = text
        .lines()
        .map(|line| {
            serde_json::from_str::<Value>(line).unwrap()["e"]
                .as_str()
                .unwrap()
                .to_owned()
        })
        .collect();
    assert_eq!(kinds, ["header", "ctx", "ctx_end"]);
}

#[test]
fn create_fails_for_a_bad_path() {
    assert!(NdjsonWriter::create("/nonexistent-dir/x/y.ndjson", "test", "1").is_err());
}

#[test]
fn finish_lists_the_contexts_that_did_not_end() {
    let buf = SharedBuf::default();
    let writer = writer(&buf);
    let info = ContextInfo {
        isolate: 1,
        worker: "w",
        actor: None,
        at: 0,
    };
    let mut ended = NdjsonSink::new(Arc::clone(&writer));
    let mut open = NdjsonSink::new(Arc::clone(&writer));
    ended.context_begin(1, &info);
    open.context_begin(2, &info);
    ended.context_end(1, 5, &ContextStats::default());
    open.flush();
    writer.finish(9);
    // Dropped: the output has ended.
    open.context_end(2, 10, &ContextStats::default());
    drop(open);

    let lines = buf.lines();
    let last = lines.last().unwrap();
    assert_eq!(last["e"], "exit");
    assert_eq!(last["at"], 9);
    assert_eq!(last["open"], serde_json::json!([2]));
    assert_eq!(lines.iter().filter(|l| l["e"] == "ctx_end").count(), 1);
    assert!(!writer.failed());
}
