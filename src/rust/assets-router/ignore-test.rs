use super::*;

fn ignored(patterns: &str, path: &str) -> bool {
    Matcher::new(patterns, &mut |w| panic!("{w}")).ignored(path)
}

#[test]
fn skips_patterns_that_cannot_compile() {
    let mut warnings = Vec::new();
    let matcher = Matcher::new("[z-a]\n*.log", &mut |w| warnings.push(w));
    assert_eq!(
        warnings,
        ["Skipping the .assetsignore pattern [z-a]: it cannot be compiled."]
    );
    assert!(matcher.ignored("/a.log"));
    assert!(!matcher.ignored("/z"));
}

#[test]
fn ignores_the_rule_files_at_the_root() {
    for path in ["/_headers", "/_redirects", "/.assetsignore"] {
        assert!(ignored("", path), "{path}");
    }
    assert!(!ignored("", "/sub/_headers"));
    assert!(!ignored("", "/index.html"));
}

#[test]
fn a_pattern_without_a_slash_matches_at_any_depth() {
    assert!(ignored("*.log", "/a.log"));
    assert!(ignored("*.log", "/d/b.log"));
    assert!(!ignored("*.log", "/a.log.txt"));
    assert!(ignored("secret/", "/secret/key.txt"));
    assert!(ignored("secret/", "/a/secret/key.txt"));
    // A directory pattern does not match a file.
    assert!(!ignored("secret/", "/secret"));
}

#[test]
fn a_pattern_with_a_slash_is_anchored() {
    assert!(ignored("/build", "/build/x.js"));
    assert!(!ignored("/build", "/d/build/x.js"));
    assert!(ignored("d/build", "/d/build"));
    assert!(!ignored("d/build", "/e/d/build"));
}

#[test]
fn globs() {
    assert!(ignored("**/foo", "/foo"));
    assert!(ignored("**/foo", "/a/b/foo"));
    assert!(ignored("a/**/b", "/a/b"));
    assert!(ignored("a/**/b", "/a/x/y/b"));
    assert!(ignored("a/**", "/a/x/y"));
    assert!(ignored("?.txt", "/a.txt"));
    assert!(!ignored("?.txt", "/ab.txt"));
    assert!(ignored("[ab].txt", "/b.txt"));
    assert!(!ignored("[ab].txt", "/c.txt"));
    assert!(ignored("[!ab].txt", "/c.txt"));
    assert!(!ignored("*.txt", "/d.x/y"));
    assert!(ignored(r"\*.txt", "/*.txt"));
    assert!(!ignored(r"\*.txt", "/a.txt"));
}

#[test]
fn negation_does_not_reach_under_an_ignored_directory() {
    let patterns = "*.txt\n!keep.txt\ndir/\n!dir/keep";
    assert!(ignored(patterns, "/a.txt"));
    assert!(!ignored(patterns, "/keep.txt"));
    assert!(ignored(patterns, "/dir/keep"));
    // The rule files can be served.
    assert!(!ignored("!_headers", "/_headers"));
}

#[test]
fn comments_whitespace_and_byte_order_mark() {
    let patterns = "\u{feff}a.txt\r\n# b.txt\n\nc.txt  \nd\\ ";
    assert!(ignored(patterns, "/a.txt"));
    assert!(!ignored(patterns, "/b.txt"));
    assert!(!ignored(patterns, "/# b.txt"));
    assert!(ignored(patterns, "/c.txt"));
    assert!(ignored(patterns, "/d "));
    assert!(!ignored(patterns, "/d"));
}
