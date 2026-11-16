// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// A `Dict` built in Rust can hold a key twice, which an object read from JavaScript can't.
#[test]
fn find_returns_the_first_entry_with_a_duplicate_key() {
    let dict = Dict::from(vec![("key".to_owned(), 1), ("key".to_owned(), 2)]);
    assert_eq!(dict.find("key"), Some(&1));
    assert_eq!(dict.find("other"), None);
}

#[test]
fn iter_and_len_see_every_entry_in_order() {
    let dict = Dict::from(vec![
        ("b".to_owned(), 1),
        ("a".to_owned(), 2),
        ("b".to_owned(), 3),
    ]);
    assert_eq!(dict.len(), 3);
    assert!(!dict.is_empty());
    assert_eq!(
        dict.iter().collect::<Vec<_>>(),
        [("b", &1), ("a", &2), ("b", &3)]
    );
    assert!(Dict::<u8>::default().is_empty());
}
