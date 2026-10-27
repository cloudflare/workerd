// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// A `Dict` built in Rust can hold a key twice, which an object read from JavaScript can't.
#[test]
fn get_returns_the_first_entry_with_a_duplicate_key() {
    let dict = Dict::from(vec![("key".to_owned(), 1), ("key".to_owned(), 2)]);
    assert_eq!(dict.get("key"), Some(&1));
    assert_eq!(dict.get("other"), None);
}
