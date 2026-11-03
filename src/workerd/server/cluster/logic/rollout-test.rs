// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

const fn node(node: &str, revision: Option<u64>) -> Node<'_> {
    Node {
        node,
        revision,
        refused: None,
        failed: None,
    }
}

/// When the test watches accept, and list the cluster just after.
const NOW: u64 = 100_000;

/// A watch from revision 1 that accepted revision 2 at `NOW`.
fn accepted() -> Watch {
    let mut watch = Watch::new(1);
    assert_eq!(watch.polled(Found::Revision(2)), Polled::Check(2));
    assert_eq!(watch.checked(2, Checked::Accept, NOW), Checked::Accept);
    watch
}

/// `me`'s turn on a fresh listing of `live`.
fn turn(watch: &mut Watch, me: &str, live: &[Node<'_>], roll: Option<&Roll>) -> Turn {
    watch.turn(me, NOW + 1, (Some(NOW), live), roll)
}

fn roll(revision: u64, baseline: usize, holder: Option<&str>) -> Roll {
    Roll {
        revision,
        baseline,
        holder: holder.map(str::to_owned),
        since_ms: 0,
        done: false,
    }
}

#[test]
fn delays_stay_within_their_bounds() {
    for random in [0, 1, u32::MAX / 2, u32::MAX - 1, u32::MAX] {
        assert!(first_delay_ms(random) < POLL_MS, "{random}");
        let delay = poll_delay_ms(random);
        assert!(
            (POLL_MS * 3 / 4..POLL_MS * 5 / 4).contains(&delay),
            "{random}: {delay}"
        );
    }
    assert_eq!(first_delay_ms(0), 0);
    assert_eq!(poll_delay_ms(0), POLL_MS * 3 / 4);
    assert_eq!(poll_delay_ms(u32::MAX / 2), POLL_MS - 1);
}

#[test]
fn the_revision_served_needs_nothing() {
    let mut watch = Watch::new(3);
    assert_eq!(watch.polled(Found::Revision(3)), Polled::Unchanged);
    assert_eq!(
        turn(&mut watch, "a", &[node("a", Some(3))], None),
        Turn::Idle
    );
    // A node that serves no catalog has nothing to do while there is none.
    let mut watch = Watch::new(0);
    assert_eq!(watch.polled(Found::Absent), Polled::Unchanged);
}

#[test]
fn another_revision_is_checked_once() {
    let mut watch = Watch::new(1);
    assert_eq!(watch.polled(Found::Revision(2)), Polled::Check(2));
    // The bucket failed during the check: again at the next read.
    watch.checked(2, Checked::Retry, NOW);
    assert_eq!(watch.accepted(), None);
    assert_eq!(watch.polled(Found::Revision(2)), Polled::Check(2));
    watch.checked(2, Checked::Accept, NOW);
    assert_eq!(watch.polled(Found::Revision(2)), Polled::Unchanged);
    assert_eq!(watch.accepted(), Some(2));
}

#[test]
fn a_lower_revision_is_rolled_out_too() {
    // A rollback by hand, or a catalog made anew, lowers the revision.
    let mut watch = Watch::new(5);
    assert_eq!(watch.polled(Found::Revision(1)), Polled::Check(1));
    watch.checked(1, Checked::Accept, NOW);
    let live = [node("a", Some(5)), node("b", Some(5))];
    // No node serves revision 1 yet, whatever they serve.
    let Turn::Begin { roll: begun, .. } = turn(&mut watch, "a", &live, None) else {
        panic!()
    };
    assert_eq!(begun, roll(1, 2, None));
    assert!(matches!(
        turn(&mut watch, "a", &live, Some(&begun)),
        Turn::Take(_)
    ));
    let progress = Progress::of(&live, 1);
    assert_eq!(progress.done, 0);
    assert!(!progress.done(2));
}

#[test]
fn a_corrupt_revision_is_refused_when_found_so_twice() {
    let mut watch = Watch::new(1);
    watch.polled(Found::Revision(2));
    assert_eq!(watch.checked(2, Checked::Corrupt, NOW), Checked::Retry);
    assert_eq!(watch.polled(Found::Revision(2)), Polled::Check(2));
    assert_eq!(watch.checked(2, Checked::Corrupt, NOW), Checked::Refuse);
    assert_eq!(watch.refused(), Some(2));
    // A read that was bad once only.
    let mut watch = Watch::new(1);
    watch.polled(Found::Revision(2));
    watch.checked(2, Checked::Corrupt, NOW);
    watch.polled(Found::Revision(2));
    assert_eq!(watch.checked(2, Checked::Accept, NOW), Checked::Accept);
}

#[test]
fn a_refused_revision_is_served_through_until_the_catalog_moves() {
    let mut watch = accepted();
    let live = [node("a", Some(1))];
    assert_eq!(watch.polled(Found::Revision(3)), Polled::Check(3));
    watch.checked(3, Checked::Refuse, NOW);
    assert_eq!(watch.refused(), Some(3));
    // Revision 2 was accepted, but a restart would load 3.
    assert_eq!(turn(&mut watch, "a", &live, None), Turn::Idle);
    assert_eq!(watch.polled(Found::Revision(3)), Polled::Unchanged);
    assert_eq!(watch.polled(Found::Revision(4)), Polled::Check(4));
    watch.checked(4, Checked::Accept, NOW);
    assert!(matches!(
        turn(&mut watch, "a", &live, None),
        Turn::Begin { .. }
    ));
    // Back to the refused one: nothing to adopt again.
    assert_eq!(watch.polled(Found::Revision(3)), Polled::Unchanged);
    assert_eq!(turn(&mut watch, "a", &live, None), Turn::Idle);
}

#[test]
fn an_unreadable_or_missing_catalog_is_served_through() {
    let mut watch = accepted();
    let live = [node("a", Some(1))];
    let begun = roll(2, 1, None);
    assert_eq!(
        watch.polled(Found::Unreadable),
        Polled::Unreadable { first: true }
    );
    assert_eq!(
        watch.polled(Found::Unreadable),
        Polled::Unreadable { first: false }
    );
    assert_eq!(
        turn(&mut watch, "a", &live, Some(&begun)),
        Turn::Unconfirmed
    );
    assert!(watch.failing());
    assert_eq!(watch.polled(Found::Revision(2)), Polled::Unchanged);
    assert!(!watch.failing());
    assert!(matches!(
        turn(&mut watch, "a", &live, Some(&begun)),
        Turn::Take(_)
    ));

    // A restart would serve no deployments at all.
    assert_eq!(watch.polled(Found::Absent), Polled::Absent { first: true });
    assert_eq!(watch.polled(Found::Absent), Polled::Absent { first: false });
    assert_eq!(turn(&mut watch, "a", &live, Some(&begun)), Turn::Idle);
    assert_eq!(watch.polled(Found::Revision(2)), Polled::Check(2));
}

#[test]
fn back_to_the_revision_served_cancels_a_roll() {
    let mut watch = accepted();
    assert_eq!(watch.polled(Found::Revision(1)), Polled::Unchanged);
    assert_eq!(
        turn(&mut watch, "a", &[node("a", Some(1))], None),
        Turn::Idle
    );
}

#[test]
fn a_listing_that_is_empty_old_or_without_this_node_is_stale() {
    let mut watch = accepted();
    let live = [node("a", Some(1)), node("b", Some(1))];
    let begun = roll(2, 2, None);
    let begun = Some(&begun);
    assert_eq!(watch.turn("a", NOW, (None, &live), begun), Turn::Stale);
    assert_eq!(watch.turn("a", NOW, (Some(NOW), &[]), begun), Turn::Stale);
    // Taken before this node accepted the revision.
    assert_eq!(
        watch.turn("a", NOW, (Some(NOW - 1), &live), begun),
        Turn::Stale
    );
    // Two rounds old.
    let late = NOW + FRESH_MS;
    assert_eq!(
        watch.turn("a", late + 1, (Some(NOW), &live), begun),
        Turn::Stale
    );
    assert!(matches!(
        watch.turn("a", late, (Some(NOW), &live), begun),
        Turn::Take(_)
    ));
    // Without this node, or with it not serving yet.
    let others = [node("b", Some(1))];
    assert_eq!(turn(&mut watch, "a", &others, begun), Turn::Stale);
    let starting = [node("a", None), node("b", Some(1))];
    assert_eq!(turn(&mut watch, "a", &starting, begun), Turn::Stale);
}

#[test]
fn a_roll_begins_with_the_nodes_that_serve() {
    let mut watch = accepted();
    let live = [node("a", Some(1)), node("b", Some(1)), node("c", Some(1))];
    assert_eq!(
        turn(&mut watch, "b", &live, None),
        Turn::Begin {
            roll: roll(2, 3, None),
            kept: None
        }
    );
    // A node that is starting counts once it serves.
    let starting = [node("a", Some(1)), node("b", Some(1)), node("z", None)];
    assert_eq!(
        turn(&mut watch, "b", &starting, None),
        Turn::Starting("z".into())
    );
}

#[test]
fn a_baseline_counted_after_a_peer_left_is_the_roll_s() {
    // a began the roll with three serving, took the turn and stopped.
    let begun = roll(2, 3, Some("a"));
    let mut watch = accepted();
    let without_a = [node("b", Some(1)), node("c", Some(1))];
    assert_eq!(
        turn(&mut watch, "b", &without_a, Some(&begun)),
        Turn::Short {
            serving: 2,
            baseline: 3
        }
    );
    // While a is live, it holds the turn.
    let all = [node("a", Some(1)), node("b", Some(1)), node("c", Some(1))];
    assert_eq!(
        turn(&mut watch, "b", &all, Some(&begun)),
        Turn::Held("a".into())
    );
    // Its replacement, z, serves revision 2.
    let back = [node("z", Some(2)), node("b", Some(1)), node("c", Some(1))];
    assert_eq!(
        turn(&mut watch, "b", &back, Some(&begun)),
        Turn::Take(Roll {
            holder: Some("b".into()),
            since_ms: NOW + 1,
            ..begun
        })
    );
}

#[test]
fn two_nodes_contend_for_one_turn() {
    // Both may take it; the conditional write of `roll.json` picks one.
    let begun = roll(2, 2, None);
    let live = [node("a", Some(1)), node("b", Some(1))];
    let (mut a, mut b) = (accepted(), accepted());
    let Turn::Take(taken) = turn(&mut a, "a", &live, Some(&begun)) else {
        panic!()
    };
    assert!(matches!(
        turn(&mut b, "b", &live, Some(&begun)),
        Turn::Take(_)
    ));
    // The loser rereads `roll.json` and finds a holding it.
    assert_eq!(
        turn(&mut b, "b", &live, Some(&taken)),
        Turn::Held("a".into())
    );
}

#[test]
fn a_new_revision_mid_roll_keeps_the_baseline_and_holder() {
    let mut watch = accepted();
    // a holds the turn for revision 2 and is down; revision 3 arrived.
    let mid = roll(2, 3, Some("a"));
    watch.polled(Found::Revision(3));
    watch.checked(3, Checked::Accept, NOW);
    let live = [node("b", Some(1)), node("c", Some(1))];
    assert_eq!(
        turn(&mut watch, "b", &live, Some(&mid)),
        Turn::Begin {
            roll: roll(3, 3, Some("a")),
            kept: Some((2, 3))
        }
    );
    // After a roll, the nodes that serve are counted anew, once as many as
    // its baseline serve: fewer is a holder not back yet, or a cluster that
    // shrank, which `kept` lets the executor say.
    let done = roll(2, 3, None);
    let mut watch = Watch::new(2);
    watch.polled(Found::Revision(3));
    watch.checked(3, Checked::Accept, NOW);
    let four = [
        node("a", Some(2)),
        node("b", Some(2)),
        node("c", Some(2)),
        node("d", Some(2)),
    ];
    let Turn::Begin { roll: begun, kept } = turn(&mut watch, "b", &four, Some(&done)) else {
        panic!()
    };
    assert_eq!((begun.baseline, kept), (4, None));
    let Turn::Begin { roll: begun, kept } = turn(&mut watch, "b", &four[..2], Some(&done)) else {
        panic!()
    };
    assert_eq!((begun.baseline, kept), (3, Some((2, 3))));
}

#[test]
fn a_complete_roll_is_marked_done_and_the_next_counts_anew() {
    // b serves revision 2, the last of three to restart; a's replacement
    // holds the turn no longer.
    let mut watch = Watch::new(2);
    let begun = roll(2, 3, Some("a"));
    let all = [node("z", Some(2)), node("b", Some(2)), node("y", Some(2))];
    let done = Roll {
        holder: None,
        done: true,
        ..begun.clone()
    };
    assert_eq!(
        turn(&mut watch, "b", &all, Some(&begun)),
        Turn::Finish(done.clone())
    );
    // Not while its holder is live, a node does not serve it, or fewer
    // serve than the baseline.
    let held = [node("a", Some(1)), node("b", Some(2)), node("y", Some(2))];
    assert_eq!(turn(&mut watch, "b", &held, Some(&begun)), Turn::Idle);
    let behind = [node("z", Some(2)), node("b", Some(2)), node("y", Some(1))];
    assert_eq!(turn(&mut watch, "b", &behind, Some(&begun)), Turn::Idle);
    assert_eq!(turn(&mut watch, "b", &all[1..], Some(&begun)), Turn::Idle);
    assert_eq!(turn(&mut watch, "b", &all, Some(&done)), Turn::Idle);

    // The cluster shrinks to two, deliberately; the next roll counts them.
    watch.polled(Found::Revision(3));
    watch.checked(3, Checked::Accept, NOW);
    assert_eq!(
        turn(&mut watch, "b", &all[1..], Some(&done)),
        Turn::Begin {
            roll: roll(3, 2, None),
            kept: None
        }
    );
}

#[test]
fn a_turn_given_up_is_released() {
    let mut watch = accepted();
    let live = [node("a", Some(1))];
    let Turn::Take(taken) = turn(&mut watch, "a", &live, Some(&roll(2, 1, None))) else {
        panic!()
    };
    // The catalog moved before a stopped.
    watch.moved();
    assert_eq!(
        turn(&mut watch, "a", &live, Some(&taken)),
        Turn::Release(Roll {
            holder: None,
            ..taken
        })
    );
}

#[test]
fn a_node_that_failed_to_start_holds_the_roll_for_good() {
    let mut watch = accepted();
    let begun = roll(2, 2, None);
    let failed = Node {
        failed: Some(2),
        ..node("z", None)
    };
    let live = [node("a", Some(1)), failed];
    assert_eq!(
        turn(&mut watch, "a", &live, Some(&begun)),
        Turn::Failed("z".into())
    );
    // Its record expired; the roll still holds.
    let live = [node("a", Some(1)), node("b", Some(1))];
    assert_eq!(
        turn(&mut watch, "a", &live, Some(&begun)),
        Turn::Failed("z".into())
    );
    // A failure on another revision does not count.
    let other = Node {
        failed: Some(7),
        ..node("y", Some(1))
    };
    let mut watch = accepted();
    let live = [node("a", Some(1)), other];
    assert!(matches!(
        turn(&mut watch, "a", &live, Some(&begun)),
        Turn::Take(_)
    ));
}

#[test]
fn a_roll_is_done_when_every_node_serves_the_revision() {
    let live = [node("a", Some(2)), node("b", Some(1)), node("c", None)];
    let progress = Progress::of(&live, 2);
    assert_eq!(
        progress,
        Progress {
            done: 1,
            serving: 2,
            live: 3,
            refused: vec![],
            failed: vec![],
        }
    );
    assert!(!progress.done(0));

    let live = [node("a", Some(2)), node("b", Some(2))];
    assert!(Progress::of(&live, 2).done(2));
    // One node that served when the wait began has not come back.
    assert!(!Progress::of(&live, 2).done(3));
    // A later revision is not this one.
    let live = [node("a", Some(2)), node("b", Some(3))];
    assert!(!Progress::of(&live, 2).done(2));
    // No nodes: nothing serves it.
    assert!(!Progress::of(&[], 2).done(0));
}

#[test]
fn refusals_and_failures_are_reported() {
    let refusing = Node {
        refused: Some(2),
        ..node("b", Some(1))
    };
    let failed = Node {
        failed: Some(2),
        ..node("c", None)
    };
    let progress = Progress::of(&[node("a", Some(2)), refusing, failed], 2);
    assert_eq!(progress.refused, ["b"]);
    assert_eq!(progress.failed, ["c"]);
    assert!(!progress.done(0));
    // Refused once, served later: not a refusal of this roll.
    let past = Node {
        refused: Some(2),
        ..node("b", Some(2))
    };
    assert!(Progress::of(&[past], 2).refused.is_empty());
}

const fn at(revision: u64) -> CatalogSeen {
    CatalogSeen::Revision {
        revision,
        names_version: true,
    }
}

fn marked(revision: u64, baseline: usize) -> Roll {
    Roll {
        done: true,
        ..roll(revision, baseline, None)
    }
}

fn waiting(line: &str) -> Waited {
    Waited::Waiting(line.to_owned())
}

#[test]
fn a_wait_is_done_once_the_roll_is_marked_done() {
    let mut wait = Wait::new(4, 2);
    let rolling = roll(4, 2, Some("b"));
    let live = [node("a", Some(4)), node("b", None), node("c", Some(3))];
    assert_eq!(
        wait.step(at(4), RollSeen::Present(&rolling), Some(&live)),
        waiting("1 of 3 live nodes serve catalog revision 4")
    );
    // The roll's baseline counts too: a node that left after the deploy counted waits.
    let rolling = roll(4, 3, Some("b"));
    let live = [node("a", Some(4)), node("c", Some(4))];
    assert_eq!(
        wait.step(at(4), RollSeen::Present(&rolling), Some(&live)),
        waiting(
            "2 of 2 live nodes serve catalog revision 4. Waiting for 1 more of the 3 nodes that served before"
        )
    );
    let live = [node("a", Some(4)), node("c", Some(4)), node("d", Some(4))];
    assert_eq!(
        wait.step(at(4), RollSeen::Present(&rolling), Some(&live)),
        waiting(
            "all live nodes serve catalog revision 4. Waiting for deploy/roll.json to mark the \
             rollout complete"
        )
    );
    assert_eq!(
        wait.step(at(4), RollSeen::Present(&marked(4, 3)), Some(&live)),
        Waited::Done("all 3 live nodes serve catalog revision 4".to_owned())
    );
}

#[test]
fn a_wait_with_no_roll_for_its_revision_is_done_once_every_node_serves_it() {
    // The nodes started on the revision, or the roll was deleted: none will mark it done.
    let live = [node("a", Some(4)), node("b", Some(4))];
    for roll in [RollSeen::Absent, RollSeen::Present(&marked(3, 5))] {
        let mut wait = Wait::new(4, 2);
        assert_eq!(
            wait.step(at(4), roll, Some(&live)),
            Waited::Done("all 2 live nodes serve catalog revision 4".to_owned())
        );
    }
    // As many as served before.
    let mut wait = Wait::new(4, 3);
    assert_eq!(
        wait.step(at(4), RollSeen::Absent, Some(&live)),
        waiting(
            "2 of 2 live nodes serve catalog revision 4. Waiting for 1 more of the 3 nodes that served before"
        )
    );
}

#[test]
fn a_wait_does_not_judge_what_did_not_read() {
    let mut wait = Wait::new(4, 1);
    let live = [node("a", Some(4))];
    assert_eq!(
        wait.step(CatalogSeen::Unreadable, RollSeen::Absent, Some(&live)),
        waiting("could not read deploy/catalog.json")
    );
    assert_eq!(
        wait.step(at(4), RollSeen::Absent, None),
        waiting("could not read the node records")
    );
    assert_eq!(
        wait.step(at(4), RollSeen::Unreadable, Some(&live)),
        waiting("all live nodes serve catalog revision 4, but deploy/roll.json could not be read")
    );
    // An unread roll does not hide a refusal.
    let refusing = Node {
        refused: Some(4),
        ..node("b", Some(3))
    };
    assert!(matches!(
        wait.step(at(4), RollSeen::Unreadable, Some(&[refusing])),
        Waited::Fail(_)
    ));
}

#[test]
fn a_wait_fails_on_a_refusal_a_failed_start_or_a_lost_catalog() {
    let mut wait = Wait::new(4, 1);
    let refusing = Node {
        refused: Some(4),
        ..node("b", Some(3))
    };
    assert_eq!(
        wait.step(
            at(4),
            RollSeen::Absent,
            Some(&[node("a", Some(4)), refusing])
        ),
        Waited::Fail(
            "cannot complete the rollout to catalog revision 4. These nodes refused it: b. Their \
             configs cannot serve it. Check their logs"
                .to_owned()
        )
    );
    let failed = Node {
        failed: Some(4),
        ..node("b", Some(3))
    };
    assert_eq!(
        wait.step(at(4), RollSeen::Absent, Some(&[failed])),
        Waited::Fail(
            "cannot complete the rollout to catalog revision 4. These nodes failed to start: b. \
             Publish a fix or the previous version"
                .to_owned()
        )
    );
    assert_eq!(
        wait.step(CatalogSeen::Absent, RollSeen::Absent, None),
        Waited::Fail("deploy/catalog.json is missing".to_owned())
    );
}

#[test]
fn a_wait_follows_a_later_revision_that_still_names_its_version() {
    let mut wait = Wait::new(4, 1);
    let refusing = Node {
        refused: Some(5),
        ..node("a", Some(4))
    };
    assert_eq!(
        wait.step(at(5), RollSeen::Absent, Some(&[refusing])),
        Waited::Fail(
            "cannot complete the rollout to catalog revision 5, a later deploy that also changed other deployments. These nodes \
             refused it: a. Their configs cannot serve it. Check their logs"
                .to_owned()
        )
    );
    assert_eq!(wait.revision, 5);
    let live = [node("a", Some(5))];
    assert_eq!(
        wait.step(at(5), RollSeen::Present(&marked(5, 1)), Some(&live)),
        Waited::Done("the only live node serves catalog revision 5, a later deploy that also changed other deployments".to_owned())
    );
    let superseding = CatalogSeen::Revision {
        revision: 6,
        names_version: false,
    };
    assert_eq!(
        wait.step(superseding, RollSeen::Absent, Some(&live)),
        Waited::Fail("catalog revision 6 does not select this deployment version".to_owned())
    );
}
