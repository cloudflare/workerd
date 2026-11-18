// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

fn parts(text: &str) -> Result<Vec<String>, PathError> {
    eval(text.as_bytes()).map(|parts| {
        parts
            .into_iter()
            .map(|part| String::from_utf8(part).unwrap())
            .collect()
    })
}

#[test]
fn eval_follows_kj_path_rules() {
    assert_eq!(parts("").unwrap(), Vec::<String>::new());
    assert_eq!(parts("/").unwrap(), Vec::<String>::new());
    assert_eq!(parts("a/b").unwrap(), ["a", "b"]);
    assert_eq!(parts("/a//b/./c/../d/").unwrap(), ["a", "b", "d"]);
    assert_eq!(parts("a/..").unwrap(), Vec::<String>::new());
    assert_eq!(parts("..."), Ok(vec!["...".to_owned()]));
}

#[test]
fn eval_rejects_leaving_the_root_and_nul() {
    assert_eq!(parts(".."), Err(PathError::AboveRoot));
    assert_eq!(parts("a/../.."), Err(PathError::AboveRoot));
    assert_eq!(eval(b"a\0b"), Err(PathError::Nul));
}

#[test]
fn parse_rejects_absolute_paths() {
    assert_eq!(parse(b"/a"), Err(PathError::Absolute));
    assert_eq!(parse(b"a/b").unwrap().len(), 2);
}

#[test]
fn single_component_names() {
    for name in ["a", "a/", "./a", "a/./", "%2F"] {
        assert!(is_single_component(name.as_bytes()), "{name:?}");
    }
    for name in ["", ".", "..", "a/b", "/a", "a/..", "a\0"] {
        assert!(!is_single_component(name.as_bytes()), "{name:?}");
    }
}
