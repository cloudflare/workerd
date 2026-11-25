// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::task::Poll;

use super::*;

/// Records being revoked, or dropped.
#[derive(Default)]
struct Flag(Rc<Cell<bool>>);

impl Revocable for Flag {
    fn revoke(&self) {
        self.0.set(true);
    }
}

impl Drop for Flag {
    fn drop(&mut self) {
        self.0.set(true);
    }
}

fn description(result: Result<()>) -> String {
    result.unwrap_err().description().to_owned()
}

#[test]
fn revoking_lets_go_of_what_is_held() {
    let revoker = Revoker::default();
    let held = Rc::new(Flag::default());
    let flag = Rc::clone(&held.0);
    let weak: Weak<Flag> = Rc::downgrade(&held);
    revoker.0.hold(weak);
    assert!(revoker.check().is_ok());

    revoker.revoke("The server was closed.");
    assert!(flag.get());
    assert_eq!(
        description(revoker.check()),
        "jsg.Error: The server was closed."
    );
}

#[test]
fn the_first_reason_stands() {
    let revoker = Revoker::default();
    revoker.revoke("first");
    revoker.revoke("second");
    assert_eq!(description(revoker.check()), "jsg.Error: first");
}

#[test]
fn holding_prunes_what_is_gone() {
    let revoker = Revoker::default();
    let gone = Rc::new(Flag::default());
    let weak: Weak<Flag> = Rc::downgrade(&gone);
    revoker.0.hold(weak);
    drop(gone);
    let kept = Rc::new(Flag::default());
    let weak: Weak<Flag> = Rc::downgrade(&kept);
    revoker.0.hold(weak);
    assert_eq!(revoker.0.held.borrow().len(), 1);
}

#[test]
fn revoking_cancels_a_call_in_progress() {
    let revoker = Revoker::default();
    let dropped = Rc::new(Cell::new(false));
    let call = {
        let flag = Flag(Rc::clone(&dropped));
        async move {
            let _flag = flag;
            std::future::pending::<Result<()>>().await
        }
    };
    futures::executor::block_on(async {
        let mut guarded = std::pin::pin!(revoker.0.guard(call));
        assert!(futures::poll!(guarded.as_mut()).is_pending());
        assert!(!dropped.get());

        revoker.revoke("closed");
        let Poll::Ready(result) = futures::poll!(guarded.as_mut()) else {
            panic!("a revoked call is still pending");
        };
        assert_eq!(description(result), "jsg.Error: closed");
        // Dropped, so canceled, before the guard answered.
        assert!(dropped.get());
    });
}

#[test]
fn a_call_after_revocation_does_not_run() {
    let revoker = Revoker::default();
    revoker.revoke("closed");
    let ran = Cell::new(false);
    let result = futures::executor::block_on(revoker.0.guard(async {
        ran.set(true);
        Ok(())
    }));
    assert_eq!(description(result), "jsg.Error: closed");
    assert!(!ran.get());
}
