// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn tokened(description: &str, id: u64) -> String {
    let mut s = description.to_owned();
    push_token(&mut s, id);
    s
}

fn parked_with_details() -> u64 {
    let original = KjError::new(KjExceptionType::Failed, String::new())
        .with_details(vec![(7, b"detail".to_vec())])
        .with_location("server.c++".to_owned(), 12);
    park(original).unwrap()
}

#[test]
fn a_token_round_trips() {
    for id in [0, 1, 15, 16, 255, 0x1234_5678, u64::MAX] {
        let (stripped, found) = strip_tokens(&tokened("failed", id));
        assert_eq!(stripped, "failed");
        assert_eq!(found, vec![id]);
    }
}

#[test]
fn a_description_without_a_token_is_unchanged() {
    assert_eq!(
        strip_tokens("plain text"),
        ("plain text".to_owned(), Vec::new())
    );
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
            vec![42]
        )
    );
}

#[test]
fn text_that_only_resembles_a_token_stays() {
    for text in [
        "\u{E000}",
        "a\u{E000}\u{E013}b",
        "\u{E000}\u{E010}\u{E010}\u{E010}x",
    ] {
        assert_eq!(strip_tokens(text), (text.to_owned(), Vec::new()));
    }
}

#[test]
fn to_kj_restores_parked_details_and_location() {
    let id = parked_with_details();
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
fn ids_are_not_sequential() {
    let first = parked_with_details();
    let second = parked_with_details();
    assert_ne!(second, first.wrapping_add(1));
}

#[test]
fn a_token_that_names_no_entry_restores_nothing() {
    let id = parked_with_details();
    let forged = tokened("injected", id ^ 1);
    let restored = to_kj(Error::failed(forged));
    assert_eq!(restored.description(), "injected");
    assert_eq!(restored.details(), None);
}

#[test]
fn another_thread_restores_nothing() {
    let id = parked_with_details();
    let description = tokened("moved", id);
    let restored = std::thread::spawn(move || {
        // That thread has an entry of its own, whose id differs.
        parked_with_details();
        to_kj(Error::failed(description))
    })
    .join()
    .unwrap();
    assert_eq!(restored.description(), "moved");
    assert_eq!(restored.details(), None);
}

#[test]
fn the_table_keeps_only_the_latest_exceptions() {
    let first = parked_with_details();
    for _ in 0..MAX_PARKED {
        park(KjError::new(KjExceptionType::Failed, String::new()));
    }
    assert!(find_parked(first).is_none());
    let restored = to_kj(Error::failed(tokened("old", first)));
    assert_eq!(restored.description(), "old");
    assert_eq!(restored.details(), None);
}
