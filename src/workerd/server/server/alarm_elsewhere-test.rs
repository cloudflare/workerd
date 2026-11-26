// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::time::SystemTime;

use futures::executor::block_on;
use outcome_capnp::EventOutcome;
use worker::AlarmResult;
use worker::Interface;

use super::AlarmElsewhere;
use super::AlarmOwnedElsewhere;

fn run_alarm(then: AlarmElsewhere) -> AlarmResult {
    let mut worker = AlarmOwnedElsewhere::new(then);
    block_on(worker.run_alarm(&SystemTime::UNIX_EPOCH, 3)).unwrap()
}

#[test]
fn retry_is_canceled_and_retried_against_no_limit() {
    assert_eq!(
        run_alarm(AlarmElsewhere::Retry),
        AlarmResult {
            retry: true,
            retry_counts_against_limit: false,
            outcome: EventOutcome::Canceled,
            error_description: None,
        }
    );
}

#[test]
fn done_succeeds_without_retry() {
    assert_eq!(
        run_alarm(AlarmElsewhere::Done),
        AlarmResult {
            retry: false,
            retry_counts_against_limit: false,
            outcome: EventOutcome::Ok,
            error_description: None,
        }
    );
}

#[test]
fn abandon_alarm_clears_nothing() {
    let mut worker = AlarmOwnedElsewhere::new(AlarmElsewhere::Retry);
    assert_eq!(
        block_on(worker.abandon_alarm(&SystemTime::UNIX_EPOCH)).unwrap(),
        None
    );
}

#[test]
fn other_events_are_unimplemented() {
    let mut worker = AlarmOwnedElsewhere::new(AlarmElsewhere::Done);
    assert!(block_on(worker.run_scheduled(&SystemTime::UNIX_EPOCH, "* * * * *")).is_err());
    assert!(block_on(worker.prewarm("https://example.com/")).is_err());
    assert!(block_on(worker.test()).is_err());
}
