// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::batches;
use super::tests_of_shard;

const LIST: &str = "a::one: test\na::two: test\nb::bench: benchmark\nb::three: test\nc::four: test\n";

#[test]
fn shards_take_every_nth_test() {
    assert_eq!(tests_of_shard(LIST, 0, 2), ["a::one", "b::three"]);
    assert_eq!(tests_of_shard(LIST, 1, 2), ["a::two", "c::four"]);
}

#[test]
fn every_test_is_in_exactly_one_shard() {
    let mut all: Vec<&str> = (0..3).flat_map(|i| tests_of_shard(LIST, i, 3)).collect();
    all.sort_unstable();
    assert_eq!(all, ["a::one", "a::two", "b::three", "c::four"]);
}

#[test]
fn lines_that_are_not_tests_are_ignored() {
    let list = "some output from static initialization\nx: test\n\n";
    assert_eq!(tests_of_shard(list, 0, 1), ["x"]);
}

#[test]
fn shards_past_the_last_test_are_empty() {
    assert!(tests_of_shard(LIST, 5, 6).is_empty());
}

#[test]
fn batches_stay_within_the_limit() {
    assert_eq!(
        batches(&["aa", "bb", "cc", "d"], 4),
        [vec!["aa", "bb"], vec!["cc", "d"]]
    );
}

#[test]
fn a_name_over_the_limit_is_a_batch_of_its_own() {
    assert_eq!(
        batches(&["a", "too-long", "b"], 4),
        [vec!["a"], vec!["too-long"], vec!["b"]]
    );
}
