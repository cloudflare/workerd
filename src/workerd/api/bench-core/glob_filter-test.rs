// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn matches(pattern: &str, name: &str) -> bool {
    GlobFilter::new(pattern.as_bytes()).matches(name.as_bytes())
}

#[test]
fn matches_literals_exactly() {
    assert!(matches("encode", "encode"));
    assert!(!matches("encode", "encoder"));
    assert!(!matches("encode", "encod"));
    assert!(matches("", ""));
    assert!(!matches("", "a"));
}

#[test]
fn star_matches_any_run_but_not_a_separator() {
    assert!(matches("*", ""));
    assert!(matches("*", "anything"));
    assert!(matches("enc*", "encode"));
    assert!(matches("*code", "encode"));
    assert!(matches("e*o*e", "encode"));
    assert!(matches("**", "encode"));
    assert!(!matches("a*c", "a/c"));
    assert!(!matches("a*c", "a\\c"));
}

#[test]
fn question_mark_matches_one_byte_but_not_a_separator() {
    assert!(matches("enc?de", "encode"));
    assert!(!matches("enc?de", "encde"));
    assert!(!matches("a?c", "a/c"));
}

#[test]
fn matching_starts_again_after_a_separator() {
    assert!(matches("b", "a/b"));
    assert!(matches("b", "a\\b"));
    assert!(matches("c*", "a/b/cd"));
    assert!(matches("b/c", "a/b/c"));
    assert!(!matches("a", "a/b"));
}

#[test]
fn startup_phases_arent_selected_by_patterns_for_cases() {
    // A ':' isn't a separator, so a pattern has to name the "startup:" prefix.
    assert!(matches("startup:*", "startup:total"));
    assert!(matches("startup:*r*", "startup:script"));
    assert!(!matches("startup:*r*", "startup:total"));
    assert!(!matches("total", "startup:total"));
    assert!(!matches("w*", "startup:worker"));
}
