// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;

use futures::future::join_all;

use super::*;

fn runtime() -> tokio::runtime::Runtime {
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
}

/// A dial that counts itself and, while `fail` is set, fails.
fn dial(dials: &Rc<Cell<u32>>, fail: bool) -> Dial<u32> {
    let dials = Rc::clone(dials);
    async move {
        dials.set(dials.get() + 1);
        tokio::task::yield_now().await;
        if fail {
            Err("refused".to_owned())
        } else {
            Ok(dials.get())
        }
    }
    .boxed_local()
}

#[test]
fn concurrent_callers_share_one_dial() {
    let (sessions, dials) = (Sessions::default(), Rc::new(Cell::new(0)));
    let gets = (0..3).map(|_| sessions.get("peer", || dial(&dials, false)));
    let got = runtime().block_on(join_all(gets));
    assert_eq!(got, vec![Ok(1), Ok(1), Ok(1)]);
    assert_eq!(dials.get(), 1);
}

#[test]
fn a_failed_dial_is_forgotten() {
    let (sessions, dials) = (Sessions::default(), Rc::new(Cell::new(0)));
    let rt = runtime();
    let failed = rt.block_on(sessions.get("peer", || dial(&dials, true)));
    assert_eq!(failed, Err("refused".to_owned()));
    let retried = rt.block_on(sessions.get("peer", || dial(&dials, false)));
    assert_eq!(retried, Ok(2));
}

#[test]
fn a_disconnected_session_is_forgotten() {
    let (sessions, dials) = (Sessions::default(), Rc::new(Cell::new(0)));
    let rt = runtime();
    assert_eq!(
        rt.block_on(sessions.get("peer", || dial(&dials, false))),
        Ok(1)
    );
    // Another session's disconnect leaves this one.
    sessions.forget("peer", |&s| s == 7);
    assert_eq!(
        rt.block_on(sessions.get("peer", || dial(&dials, false))),
        Ok(1)
    );
    sessions.forget("peer", |&s| s == 1);
    assert_eq!(
        rt.block_on(sessions.get("peer", || dial(&dials, false))),
        Ok(2)
    );
}

#[test]
fn an_advertised_port_of_zero_is_the_bound_port() {
    assert_eq!(with_bound_port("127.0.0.1:0", 7000), "127.0.0.1:7000");
    assert_eq!(with_bound_port("[::1]:0", 7000), "[::1]:7000");
    assert_eq!(
        with_bound_port("node1.internal:0", 7000),
        "node1.internal:7000"
    );
    // A port given stays, whatever was bound.
    assert_eq!(with_bound_port("10.0.0.4:7001", 7000), "10.0.0.4:7001");
    assert_eq!(with_bound_port("10.0.0.4:10", 7000), "10.0.0.4:10");
}
