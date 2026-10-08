// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;
use crate::Event;
use crate::RecordingSink;

#[test]
fn trackers_share_the_isolate() {
    let isolate = new_isolate();
    let mut a = new_tracker(&isolate, "a", "");
    let mut b = new_tracker(&isolate, "b", "");
    let x = a.create_ffi(3, "x", 0, 0);
    let y = b.create_ffi(3, "y", 0, 0);
    assert!(y > x);
}

#[test]
fn empty_actor_means_none() {
    let isolate = new_isolate();
    for (actor, expected) in [("", None), ("id", Some("id".to_owned()))] {
        let mut tracker = new_tracker(&isolate, "w", actor);
        let sink = RecordingSink::new();
        tracker.add_sink(Box::new(sink.clone()));
        assert!(matches!(
            &sink.events()[0],
            Event::ContextBegin { actor: a, .. } if *a == expected
        ));
    }
}

#[test]
fn create_and_settle_translate_their_arguments() {
    let isolate = new_isolate();
    let mut tracker = new_tracker(&isolate, "w", "");
    let sink = RecordingSink::new();
    tracker.add_sink(Box::new(sink.clone()));
    let _ = sink.take();

    tracker.begin_stack();
    tracker.push_frame("f", "s.js", 1, 2, 3);
    let stack = tracker.end_stack_ffi();
    assert_ne!(stack, 0);
    let id = tracker.create_ffi(5, "kv_get", 0, stack);
    let unstacked = tracker.create_ffi(200, "other", id, 0);
    tracker.settle_ffi(id, 2);

    let events = sink.take();
    assert!(matches!(&events[0], Event::Stack { id: s, .. } if *s == stack));
    assert!(matches!(
        &events[1],
        Event::Init { kind: Kind::Operation, stack: Some(s), .. } if *s == stack
    ));
    assert!(matches!(
        &events[2],
        Event::Init { id: i, kind: Kind::Other, trigger, stack: None, .. }
            if *i == unstacked && *trigger == id
    ));
    assert!(matches!(
        &events[3],
        Event::Settle {
            outcome: Outcome::Canceled,
            ..
        }
    ));
}

#[test]
fn empty_stack_is_zero() {
    let isolate = new_isolate();
    let mut tracker = new_tracker(&isolate, "w", "");
    tracker.begin_stack();
    assert_eq!(tracker.end_stack_ffi(), 0);
}

#[test]
fn ndjson_writer_round_trip() {
    let dir = std::env::var("TEST_TMPDIR").map_or_else(|_| std::env::temp_dir(), Into::into);
    let path = dir.join("async-trace-ffi-test.ndjson");
    let path = path.to_str().unwrap();
    let writer = open_ndjson_writer(path, "0.0.0-test").unwrap();
    let isolate = new_isolate();
    let mut tracker = new_tracker(&isolate, "w", "");
    tracker.add_ndjson_sink(&writer);
    tracker.create_ffi(0, "fetch", 0, 0);
    tracker.close();
    assert!(!writer.failed());

    let text = std::fs::read_to_string(path).unwrap();
    let lines: Vec<serde_json::Value> = text
        .lines()
        .map(|line| serde_json::from_str(line).unwrap())
        .collect();
    assert_eq!(lines[0]["producer"], "workerd");
    assert_eq!(lines[0]["version"], "0.0.0-test");
    assert_eq!(lines[2]["kind"], "request");
    assert_eq!(lines.last().unwrap()["e"], "ctx_end");
}

#[test]
fn open_ndjson_writer_reports_errors() {
    assert!(open_ndjson_writer("/nonexistent-dir/x/y.ndjson", "1").is_err());
}
