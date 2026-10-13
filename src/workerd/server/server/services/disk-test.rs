// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

/// `path_segments(url)`, comparable with the expected segments written as string literals.
#[derive(Debug)]
struct Segments(Option<Vec<String>>);

impl PartialEq<Option<Vec<&str>>> for Segments {
    fn eq(&self, expected: &Option<Vec<&str>>) -> bool {
        let actual = self
            .0
            .as_ref()
            .map(|segments| segments.iter().map(String::as_str).collect::<Vec<_>>());
        actual == *expected
    }
}

fn segments(url: &str) -> Segments {
    Segments(path_segments(url))
}

#[test]
fn paths_are_split_decoded_and_checked() {
    assert_eq!(segments("http://h/"), Some(vec![]));
    assert_eq!(segments("http://h"), Some(vec![]));
    assert_eq!(segments("http://h/a/b.txt?x=1"), Some(vec!["a", "b.txt"]));
    assert_eq!(segments("http://h/dir/"), Some(vec!["dir"]));
    assert_eq!(segments("http://h/a%20b/c%2Fd"), None);
    assert_eq!(segments("http://h/sp%20ace"), Some(vec!["sp ace"]));
    assert_eq!(segments("http://h/../etc"), Some(vec!["etc"]));
    assert_eq!(segments("http://h/a/../b"), Some(vec!["b"]));
    assert_eq!(segments("http://h/a/%2e%2e/b"), Some(vec!["b"]));
    assert_eq!(segments("http://h/a//b"), None);
    assert_eq!(segments("http://h/./a"), Some(vec!["a"]));
    assert_eq!(segments("http://h/a%00"), None);
    assert_eq!(segments("http://h/.hidden"), Some(vec![".hidden"]));
}

#[test]
fn a_segment_is_one_entry_name() {
    assert!(is_entry_name("b.txt"));
    for segment in ["", ".", "..", "/", "a/b", "a\0"] {
        assert!(!is_entry_name(segment), "{segment:?}");
    }
    // What Windows reads as separators, drives and streams are ordinary characters elsewhere.
    for segment in [
        "x\\..\\..\\secret",
        "..\\secret",
        "C:\\Windows\\win.ini",
        "C:secret",
        "file:stream",
    ] {
        assert_eq!(is_entry_name(segment), cfg!(not(windows)), "{segment:?}");
    }
}

#[test]
fn one_range_is_served_and_several_get_everything() {
    assert_eq!(parse_range("bytes=0-99", 1000), Range::Bytes(0, 99));
    assert_eq!(parse_range("bytes=500-", 1000), Range::Bytes(500, 999));
    assert_eq!(parse_range("bytes=-100", 1000), Range::Bytes(900, 999));
    assert_eq!(parse_range("BYTES = 5-6", 1000), Range::Bytes(5, 6));
    assert_eq!(parse_range("bytes=-5000", 1000), Range::Everything);
    assert_eq!(parse_range("bytes=0-", 1000), Range::Everything);
    assert_eq!(parse_range("bytes=0-2000", 1000), Range::Everything);
    assert_eq!(parse_range("bytes=0-1, 5-6", 1000), Range::Everything);
    assert_eq!(parse_range("bytes=1000-", 1000), Range::Unsatisfiable);
    assert_eq!(parse_range("bytes=5-2", 1000), Range::Unsatisfiable);
    assert_eq!(parse_range("bytes=-", 1000), Range::Unsatisfiable);
    assert_eq!(parse_range("bytes=a-b", 1000), Range::Unsatisfiable);
    assert_eq!(parse_range("items=0-1", 1000), Range::Unsatisfiable);
    assert_eq!(parse_range("bytes=0-1", 0), Range::Unsatisfiable);
}
