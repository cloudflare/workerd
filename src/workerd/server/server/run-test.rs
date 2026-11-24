// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn shards_split_the_cases_between_them() {
    let cases: Vec<u32> = (0..7).collect();
    let shards: Vec<Vec<u32>> = (0..3)
        .map(|index| shard_cases(cases.clone(), (index, 3)))
        .collect();
    assert_eq!(shards, [vec![0, 3, 6], vec![1, 4], vec![2, 5]]);
}

#[test]
fn shards_beyond_the_cases_are_empty() {
    assert_eq!(shard_cases(vec!["a", "b"], (1, 2)), ["b"]);
    assert!(shard_cases(vec!["a", "b"], (2, 3)).is_empty());
}

#[test]
fn one_shard_runs_every_case() {
    assert_eq!(shard_cases(vec!["a", "b", "c"], (0, 1)), ["a", "b", "c"]);
}
