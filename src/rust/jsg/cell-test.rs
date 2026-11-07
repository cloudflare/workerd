// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;

use super::*;

/// A write from the code that reports the value would drop it under the report.
#[test]
fn set_panics_while_the_value_is_reported() {
    let cell = Cell::new(1u32);
    let written = catch_unwind(AssertUnwindSafe(|| {
        cell.read_for_report(|_| cell.set(2));
    }));
    assert!(written.is_err());
    // The panic ended the report, which let the cell go.
    cell.set(3);
    assert_eq!(cell.read_for_report(|value| *value), 3);
}

/// One flag for the thread: reporting one cell refuses writes into any other.
#[test]
fn set_of_another_cell_panics_while_a_value_is_reported() {
    let cell = Cell::new(1u32);
    let other = Cell::new(1u32);
    let written = catch_unwind(AssertUnwindSafe(|| {
        cell.read_for_report(|_| other.set(2));
    }));
    assert!(written.is_err());
    other.set(3);
}

/// Another thread's report does not refuse writes on this one.
#[test]
fn a_report_on_another_thread_allows_writes() {
    let cell = Cell::new(1u32);
    let (reading, read) = std::sync::mpsc::channel();
    let (done, finish) = std::sync::mpsc::channel::<()>();
    let reporter = std::thread::spawn(move || {
        let other = Cell::new(1u32);
        other.read_for_report(|_| {
            reading.send(()).unwrap();
            finish.recv().unwrap();
        });
    });
    read.recv().unwrap();
    cell.set(2);
    done.send(()).unwrap();
    reporter.join().unwrap();
}

#[test]
fn a_nested_report_leaves_the_outer_one_in_force() {
    let cell = Cell::new(1u32);
    let written = catch_unwind(AssertUnwindSafe(|| {
        cell.read_for_report(|_| {
            cell.read_for_report(|_| {});
            cell.set(2);
        });
    }));
    assert!(written.is_err());
    cell.set(3);
    assert_eq!(cell.read_for_report(|value| *value), 3);
}
