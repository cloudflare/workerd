// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn the_phases_of_a_stop_come_in_order_within_it() {
    let stops = (0..=1_000).chain([DEFAULT_STOP_MS, 600_000, u64::from(u32::MAX)]);
    for stop_ms in stops {
        let budget = Budget { stop_ms };
        assert!(budget.in_flight_ms() <= budget.handoff_ms(), "{budget:?}");
        assert!(budget.grace_ms() <= budget.handoff_ms(), "{budget:?}");
        assert!(budget.handoff_ms() <= budget.release_ms(), "{budget:?}");
        assert!(budget.release_ms() <= budget.connections_ms(), "{budget:?}");
        assert!(budget.connections_ms() <= stop_ms, "{budget:?}");
    }
    let budget = Budget::default();
    assert_eq!(
        (
            budget.in_flight_ms(),
            budget.handoff_ms(),
            budget.release_ms(),
            budget.connections_ms()
        ),
        (4_000, 6_000, 6_600, 7_200)
    );
    assert_eq!(budget.grace_ms(), GRACE_MS);
    assert_eq!(Budget { stop_ms: 4_000 }.grace_ms(), 3_000);
}

#[test]
fn heat_halves_every_half_life() {
    let mut heat = Heat::default();
    assert!(heat.at(0).abs() < 1e-9);
    heat.hit(1_000);
    heat.hit(1_000);
    assert!((heat.at(1_000) - 2.0).abs() < 1e-9);
    let later = 1_000 + u64::from(HALF_LIFE_MS);
    assert!((heat.at(later) - 1.0).abs() < 1e-9);
    heat.hit(later);
    assert!((heat.at(later) - 2.0).abs() < 1e-9);
    // A clock that steps back counts the hit, and ages nothing.
    heat.hit(0);
    assert!((heat.at(later) - 3.0).abs() < 1e-9);
}

#[test]
fn the_busiest_object_is_handed_off_first() {
    let mut cold = Heat::default();
    cold.hit(0);
    cold.hit(0);
    cold.hit(0);
    let mut hot = Heat::default();
    hot.hit(60_000);
    hot.hit(60_000);
    let idle = Heat::default();
    // Three requests a minute ago count for less than two just now.
    assert_eq!(busiest_first(&[idle, cold, hot], 60_000), [2, 1, 0]);
    assert!(busiest_first(&[], 0).is_empty());
}

#[test]
fn a_successor_is_stable_and_spread_over_the_peers() {
    let peers = ["a", "b", "c"];
    assert_eq!(successor("x", []), None);
    assert_eq!(successor("x", ["a"]), Some("a"));
    let mut counts = [0usize; 3];
    for i in 0..3000 {
        let object = format!("object-{i}");
        let chosen = successor(&object, peers).unwrap_or_default();
        // The order the peers are listed in changes nothing.
        assert_eq!(successor(&object, ["c", "a", "b"]), Some(chosen));
        counts[peers.iter().position(|p| *p == chosen).unwrap_or_default()] += 1;
        // A peer that leaves moves only its own objects.
        let without_b = successor(&object, ["a", "c"]).unwrap_or_default();
        if chosen != "b" {
            assert_eq!(without_b, chosen);
        }
    }
    for count in counts {
        assert!((800..1200).contains(&count), "{counts:?}");
    }
}
