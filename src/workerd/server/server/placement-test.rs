// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

const ID: &str = "aa299662980ce671dbcb09a5d7ab26ab30e45465bb11c7f3b5d1dc5b4e4a3d1f";

#[test]
fn one_thread_is_every_actors_home() {
    for key in ["", "a", ID] {
        assert_eq!(home(&Scope::Durable("ns"), key, 1), 0);
        assert_eq!(home(&Scope::Durable("ns"), key, 0), 0);
    }
}

#[test]
fn a_placement_that_does_not_spread_keeps_every_actor_on_the_first_thread() {
    let placement = Placement {
        count: 8,
        spread: false,
    };
    for n in 0..200 {
        assert_eq!(placement.home(&Scope::Durable("ns"), &format!("{n}")), 0);
    }
    let spread = Placement {
        spread: true,
        ..placement
    };
    let homes: std::collections::HashSet<usize> = (0..200)
        .map(|n| spread.home(&Scope::Durable("ns"), &format!("{n}")))
        .collect();
    assert_eq!(homes.len(), 8);
}

#[test]
fn a_home_is_within_the_threads_and_the_same_every_time() {
    for count in 2..=16 {
        for n in 0..500 {
            let key = format!("{ID}{n}");
            let first = home(&Scope::Durable("ns"), &key, count);
            assert!(first < count);
            assert_eq!(first, home(&Scope::Durable("ns"), &key, count));
        }
    }
}

/// The hash is the same in every build: its key is fixed.
#[test]
fn the_hash_is_pinned() {
    let durable = Scope::Durable("ns");
    let ephemeral = Scope::Ephemeral("service", "Class");
    let found = [
        home(&durable, ID, 4),
        home(&durable, ID, 7),
        home(&durable, "", 4),
        home(&ephemeral, "name", 4),
        home(&ephemeral, "name", 1000),
    ];
    assert_eq!(found, PINNED);
}

const PINNED: [usize; 5] = [3, 5, 1, 2, 741];

#[test]
fn actors_spread_evenly() {
    for count in [2, 3, 4, 8, 12] {
        let mut homes = vec![0u32; count];
        let actors = 4000 * count;
        for n in 0..actors {
            homes[home(&Scope::Durable("ns"), &format!("{n:064x}"), count)] += 1;
        }
        for (thread, actors) in homes.iter().enumerate() {
            assert!(
                (3600..=4400).contains(actors),
                "thread {thread} of {count} has {actors} of 4000 expected"
            );
        }
    }
}

#[test]
fn the_namespace_is_part_of_what_is_hashed() {
    let differs = |a: Scope<'_>, b: Scope<'_>| {
        (0..64).any(|n| {
            let key = format!("{n}");
            home(&a, &key, 16) != home(&b, &key, 16)
        })
    };
    assert!(differs(Scope::Durable("a"), Scope::Durable("b")));
    assert!(differs(
        Scope::Ephemeral("service", "A"),
        Scope::Ephemeral("service", "B")
    ));
    assert!(differs(
        Scope::Ephemeral("one", "A"),
        Scope::Ephemeral("two", "A")
    ));
    // The parts' boundaries count: these have the same bytes in a row.
    assert!(differs(
        Scope::Ephemeral("ab", "c"),
        Scope::Ephemeral("a", "bc")
    ));
    // An ephemeral namespace is no durable one of the same name.
    assert!(differs(Scope::Durable("ab"), Scope::Ephemeral("a", "b")));
}
