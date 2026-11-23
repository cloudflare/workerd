// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn tokened(description: &str, id: u64) -> String {
    let mut s = description.to_owned();
    push_token(&mut s, id);
    s
}

#[test]
fn a_token_round_trips() {
    for id in [0, 1, 15, 16, 255, 0x1234_5678, u64::MAX] {
        let (stripped, found) = strip_tokens(&tokened("failed", id));
        assert_eq!(stripped, "failed");
        assert_eq!(found, Some(id));
    }
}

#[test]
fn a_description_without_a_token_is_unchanged() {
    assert_eq!(strip_tokens("plain text"), ("plain text".to_owned(), None));
}

#[test]
fn a_token_is_found_inside_added_context() {
    let wrapped = format!(
        "while forwarding: {} (retrying)",
        tokened("disconnected", 42)
    );
    assert_eq!(
        strip_tokens(&wrapped),
        (
            "while forwarding: disconnected (retrying)".to_owned(),
            Some(42)
        )
    );
}

#[test]
fn to_kj_restores_parked_details_and_location() {
    let original = KjError::new(KjExceptionType::Failed, String::new())
        .with_details(vec![(7, b"detail".to_vec())])
        .with_location("server.c++".to_owned(), 12);
    let id = park(original);
    let error = Error {
        kind: ErrorKind::Disconnected,
        extra: format!("context: {}", tokened("lost", id)),
    };
    let restored = to_kj(error);
    assert_eq!(restored.description(), "context: lost");
    assert_eq!(restored.exception_type(), KjExceptionType::Disconnected);
    assert_eq!(restored.details(), Some(&vec![(7, b"detail".to_vec())]));
    assert_eq!(restored.file(), Some("server.c++"));
    assert_eq!(restored.line(), Some(12));
}

#[test]
fn the_table_keeps_only_the_latest_exceptions() {
    let first = park(KjError::new(KjExceptionType::Failed, String::new()));
    for _ in 0..MAX_PARKED {
        park(KjError::new(KjExceptionType::Failed, String::new()));
    }
    assert!(find_parked(first).is_none());
    let restored = to_kj(Error::failed(tokened("old", first)));
    assert_eq!(restored.description(), "old");
    assert_eq!(restored.details(), None);
}
