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

/// Two instances' ids, and an address they may share.
const A: &str = "a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1";
const B: &str = "b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2";
const AT: &str = "127.0.0.1:7000";

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
    let gets = (0..3).map(|_| sessions.get(A, AT, |_, _| dial(&dials, false)));
    let got = runtime().block_on(join_all(gets));
    assert_eq!(got, vec![Ok(1), Ok(1), Ok(1)]);
    assert_eq!(dials.get(), 1);
}

#[test]
fn a_failed_dial_is_forgotten() {
    let (sessions, dials) = (Sessions::default(), Rc::new(Cell::new(0)));
    let rt = runtime();
    let failed = rt.block_on(sessions.get(A, AT, |_, _| dial(&dials, true)));
    assert_eq!(failed, Err("refused".to_owned()));
    let retried = rt.block_on(sessions.get(A, AT, |_, _| dial(&dials, false)));
    assert_eq!(retried, Ok(2));
}

#[test]
fn a_disconnected_session_is_forgotten() {
    let (sessions, dials) = (Sessions::default(), Rc::new(Cell::new(0)));
    let rt = runtime();
    assert_eq!(
        rt.block_on(sessions.get(A, AT, |_, _| dial(&dials, false))),
        Ok(1)
    );
    // Another session's disconnect leaves this one.
    sessions.forget(A, AT, |&s| s == 7);
    assert_eq!(
        rt.block_on(sessions.get(A, AT, |_, _| dial(&dials, false))),
        Ok(1)
    );
    sessions.forget(A, AT, |&s| s == 1);
    assert_eq!(
        rt.block_on(sessions.get(A, AT, |_, _| dial(&dials, false))),
        Ok(2)
    );
}

/// Another instance at the same address, one that took a dead peer's port or shares an
/// `advertise` address, gets a session of its own: the open session passed the first one's
/// CONNECT, not its own. Its refusal leaves the open session, and its next request dials again.
#[test]
fn each_instance_at_an_address_has_its_own_session() {
    let (sessions, dials) = (Sessions::default(), Rc::new(Cell::new(0)));
    let rt = runtime();
    let dialed = Rc::new(RefCell::new(Vec::new()));
    let get = |node, fail| {
        let (dials, dialed) = (Rc::clone(&dials), Rc::clone(&dialed));
        sessions.get(node, AT, move |node, address| {
            dialed.borrow_mut().push(format!("{node}@{address}"));
            dial(&dials, fail)
        })
    };
    assert_eq!(rt.block_on(get(A, false)), Ok(1));
    // The instance there now is A, which refuses a CONNECT for B.
    assert_eq!(rt.block_on(get(B, true)), Err("refused".to_owned()));
    assert_eq!(rt.block_on(get(A, false)), Ok(1));
    assert_eq!(rt.block_on(get(B, false)), Ok(3));
    assert_eq!(rt.block_on(get(B, false)), Ok(3));
    assert_eq!(
        *dialed.borrow(),
        [
            format!("{A}@{AT}"),
            format!("{B}@{AT}"),
            format!("{B}@{AT}")
        ]
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

/// A keyed MAC for tests: the key, then the bytes.
fn keyed(key: &'static [u8]) -> impl Fn(&[u8]) -> Vec<u8> {
    move |data| [key, data].concat()
}

/// Whether instance `me`, dialed at `advertise` with the key `b"key"`, accepts `auth` at `now`.
fn accepts(
    me: &str,
    advertise: &str,
    auth: Option<&str>,
    now: u64,
    nonces: &mut HashMap<String, u64>,
) -> std::result::Result<(), String> {
    let header = auth.map(str::as_bytes);
    check_connect_auth(&keyed(b"key"), me, advertise, header, now, nonces)
        .map_err(|e| e.description().to_owned())
}

#[test]
fn a_connect_is_for_one_instance_at_one_address() {
    let now = 1_000_000;
    let mac = keyed(b"key");
    let auth = connect_auth(&mac, A, AT, now, "n1").unwrap();
    let mut nonces = HashMap::new();
    // What took a dead peer's port since: another instance of the cluster, or one of another
    // cluster with the same key.
    let other = accepts(B, AT, Some(&auth), now, &mut nonces).unwrap_err();
    assert!(other.contains(&format!("for instance {A}")), "{other}");
    assert!(accepts(A, "127.0.0.1:7001", Some(&auth), now, &mut nonces).is_err());
    let unkeyed = connect_auth(&keyed(b"other"), A, AT, now, "n1").unwrap();
    assert!(accepts(A, AT, Some(&unkeyed), now, &mut nonces).is_err());
    // Nothing above used the nonce.
    assert_eq!(accepts(A, AT, Some(&auth), now, &mut nonces), Ok(()));
    let replayed = accepts(A, AT, Some(&auth), now, &mut nonces).unwrap_err();
    assert!(replayed.contains("replayed"), "{replayed}");
    let stale = connect_auth(&mac, A, AT, now - AUTH_WINDOW_MS - 1, "n2").unwrap();
    assert!(accepts(A, AT, Some(&stale), now, &mut nonces).is_err());
    assert!(accepts(A, AT, None, now, &mut nonces).is_err());
}

/// The instance a header names is signed: naming another one breaks the MAC.
#[test]
fn a_connect_renamed_for_another_instance_is_unsigned() {
    let now = 1_000_000;
    let auth = connect_auth(&keyed(b"key"), A, AT, now, "n1").unwrap();
    let renamed = auth.replacen(A, B, 1);
    assert_ne!(renamed, auth);
    let refused = accepts(B, AT, Some(&renamed), now, &mut HashMap::new()).unwrap_err();
    assert!(refused.contains("without a valid"), "{refused}");
}

/// A header names an instance by its id, so that the signed text has one reading: neither side
/// makes or takes one that names anything else.
#[test]
fn a_connect_names_an_instance_id() {
    let now = 1_000_000;
    let mac = keyed(b"key");
    assert!(is_node_id(&new_node_id()));
    for node in [
        "a1",
        &A.to_uppercase(),
        &format!("{A}0"),
        &format!("{}\n", &A[1..]),
    ] {
        assert!(!is_node_id(node), "{node:?}");
        let made = connect_auth(&mac, node, AT, now, "n1")
            .unwrap_err()
            .to_string();
        assert!(made.contains("not an instance id"), "{made}");
        let tag = cluster::hex(&connect_mac(&mac, node, AT, now, "n1"));
        let auth = format!("{node}.{now}.n1.{tag}");
        let refused = accepts(node, AT, Some(&auth), now, &mut HashMap::new()).unwrap_err();
        assert!(refused.contains("without a valid"), "{refused}");
    }
}

#[test]
fn a_fenced_instance_exits_while_its_lease_holds() {
    let ms = Duration::from_millis;
    assert_eq!(fence_log_wait(ms(5_000)), Some(FENCE_LOG_WAIT));
    assert_eq!(fence_log_wait(ms(100)), Some(ms(80)));
    assert_eq!(fence_log_wait(FENCE_SLACK + ms(1)), Some(ms(1)));
    assert_eq!(fence_log_wait(FENCE_SLACK), None);
    assert_eq!(fence_log_wait(ms(5)), None);
    assert_eq!(fence_log_wait(Duration::ZERO), None);
}

/// Counts that a test advances, as loops would, and the one count the lease thread reads of
/// them.
fn loops(n: usize) -> (Vec<Arc<std::sync::atomic::AtomicU64>>, cluster::Progress) {
    let counts: Vec<_> = (0..n)
        .map(|_| Arc::<std::sync::atomic::AtomicU64>::default())
        .collect();
    let read: Vec<cluster::Progress> = counts
        .iter()
        .map(|count| {
            let count = Arc::clone(count);
            Box::new(move || count.load(std::sync::atomic::Ordering::SeqCst)) as cluster::Progress
        })
        .collect();
    (counts, every_loop(read))
}

#[test]
fn the_watched_count_advances_only_when_every_loop_did() {
    use std::sync::atomic::Ordering::SeqCst;
    let (counts, watched) = loops(3);
    // The first reading is of loops that were never read before.
    let start = watched();
    assert_eq!(watched(), start, "no loop moved");

    // Two of three move, round after round: the count stands still, as for one stuck loop.
    for _ in 0..10 {
        counts[0].fetch_add(1, SeqCst);
        counts[2].fetch_add(5, SeqCst);
        assert_eq!(watched(), start);
    }
    // The third moves at last: one step, however far the others went.
    counts[1].fetch_add(1, SeqCst);
    assert_eq!(watched(), start + 1);
    assert_eq!(watched(), start + 1);

    // Every loop moves between two readings: a step each time.
    for step in 2..6 {
        for count in &counts {
            count.fetch_add(1, SeqCst);
        }
        assert_eq!(watched(), start + step);
    }
}

#[test]
fn one_loop_is_watched_as_it_counts() {
    use std::sync::atomic::Ordering::SeqCst;
    let (counts, watched) = loops(1);
    let start = watched();
    assert_eq!(watched(), start);
    counts[0].fetch_add(1, SeqCst);
    assert_eq!(watched(), start + 1);
    counts[0].fetch_add(7, SeqCst);
    assert_eq!(watched(), start + 2);
}
