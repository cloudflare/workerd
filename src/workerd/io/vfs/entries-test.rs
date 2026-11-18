// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn keys(map: &EntryMap<u32>) -> Vec<&str> {
    map.iter()
        .map(|(name, _)| std::str::from_utf8(name).unwrap())
        .collect()
}

#[test]
fn removing_moves_the_last_entry_into_the_hole() {
    let mut map = EntryMap::default();
    for (i, name) in ["a", "b", "c", "d", "e"].into_iter().enumerate() {
        map.push(name.as_bytes(), u32::try_from(i).unwrap());
    }
    assert!(map.remove(b"b"));
    assert_eq!(keys(&map), ["a", "e", "c", "d"]);
    // The moved entry is still found under its name.
    assert_eq!(map.get(b"e"), Some(&4));

    assert!(map.remove(b"d"));
    assert_eq!(keys(&map), ["a", "e", "c"]);

    map.push(b"f", 5);
    assert_eq!(keys(&map), ["a", "e", "c", "f"]);

    assert!(map.remove(b"a"));
    assert_eq!(keys(&map), ["f", "e", "c"]);
    assert_eq!(map.get(b"f"), Some(&5));
    assert_eq!(map.len(), 3);
}

#[test]
fn removing_a_missing_entry() {
    let mut map: EntryMap<u32> = EntryMap::default();
    assert!(!map.remove(b"a"));
    map.push(b"a", 1);
    assert!(map.remove(b"a"));
    assert!(!map.contains(b"a"));
    assert_eq!(map.len(), 0);
}
