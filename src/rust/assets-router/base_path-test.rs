use super::*;

fn normalize(value: &str) -> Result<String, &'static str> {
    BasePath::normalize(value).map(|base| base.as_str().to_owned())
}

fn base(value: &str) -> BasePath {
    BasePath::normalize(value).unwrap()
}

// Upstream: `utils/testdata/base-path-normalization.json` (contract version 1), the vectors
// the control plane's normalization is held to as well.
#[test]
fn normalize_matches_the_shared_contract() {
    for (name, input, canonical) in [
        ("root", "/", Some("/")),
        ("absolute", "/docs", Some("/docs/")),
        ("relative", "./docs", Some("/docs/")),
        ("duplicate segments", "/a//b", Some("/a/b/")),
        ("dot segments", "/a/./b/../c", Some("/a/c/")),
        ("above root", "../../docs", Some("/docs/")),
        ("unicode", "/café/naïve", Some("/café/naïve/")),
        ("empty", "", None),
        ("url", "https://example.com/docs", None),
        ("protocol relative url", "//example.com/docs", None),
        ("backslash", "/a\\b", None),
        ("query", "/docs?lang=en", None),
        ("fragment", "/docs#intro", None),
        ("percent encoding", "/docs%2Fapi", None),
        ("control character", "/docs\u{1}api", None),
    ] {
        assert_eq!(normalize(input).ok().as_deref(), canonical, "{name}");
    }
}

// Upstream: utils/tests/base-path.test.ts `normalizeBasePath > valid values`.
#[test]
fn normalize_accepts_pathnames() {
    assert_eq!(normalize("/subpath").unwrap(), "/subpath/");
    assert_eq!(normalize("/a/b/c").unwrap(), "/a/b/c/");
    assert_eq!(normalize("subpath").unwrap(), "/subpath/");
    assert_eq!(normalize("../app").unwrap(), "/app/");
    assert_eq!(normalize("/subpath/").unwrap(), "/subpath/");
    assert_eq!(normalize("/.").unwrap(), "/");

    let long = format!("/{}", "😀".repeat(2000));
    assert_eq!(normalize(&long).unwrap(), format!("{long}/"));
}

// Upstream: utils/tests/base-path.test.ts `normalizeBasePath > invalid values`.
#[test]
fn normalize_rejects_values_that_are_not_pathnames() {
    assert_eq!(normalize(""), Err("The value must not be empty."));
    assert_eq!(
        normalize("/sub\\path"),
        Err("The value must not contain backslashes.")
    );
    assert_eq!(
        normalize("/sub\u{0}path"),
        Err("The value must not contain control characters.")
    );
    assert_eq!(
        normalize("/sub\u{7f}path"),
        Err("The value must not contain control characters.")
    );
    assert_eq!(
        normalize("/subpath?a=1"),
        Err("The value must not contain query strings or fragments.")
    );
    assert_eq!(
        normalize("/subpath#frag"),
        Err("The value must not contain query strings or fragments.")
    );
    assert_eq!(
        normalize("/sub%2Fpath"),
        Err("The value must not contain percent-encoded sequences.")
    );
    assert_eq!(
        normalize("/sub%20path"),
        Err("The value must not contain percent-encoded sequences.")
    );
    assert_eq!(
        normalize("//a"),
        Err("The value must be a pathname, not a URL.")
    );
    assert_eq!(
        normalize("https://example.com/subpath"),
        Err("The value must be a pathname, not a URL.")
    );
    assert_eq!(
        normalize("data:text/plain,hello"),
        Err("The value must be a pathname, not a URL.")
    );
}

#[test]
fn normalize_only_treats_a_leading_scheme_as_a_url() {
    assert_eq!(normalize("/a:b").unwrap(), "/a:b/");
    assert_eq!(normalize("1a:b").unwrap(), "/1a:b/");
}

// Upstream: asset-worker/tests/base-path.test.ts `stripBasePath`.
#[test]
fn strip_maps_public_paths_to_asset_paths() {
    assert_eq!(base("/").strip("/foo/bar"), Some("/foo/bar"));
    assert_eq!(base("/subpath/").strip("/subpath/foo"), Some("/foo"));
    assert_eq!(base("/subpath/").strip("/subpath"), Some("/"));
    assert_eq!(base("/subpath/").strip("/subpath/"), Some("/"));
    assert_eq!(base("/subpath/").strip("/subpath-other/foo"), None);
    assert_eq!(base("/subpath/").strip("/other"), None);
    assert_eq!(base("/路径/").strip("/路径/foo"), Some("/foo"));
}

// Upstream: asset-worker/tests/base-path.test.ts `addBasePath`.
#[test]
fn add_maps_asset_paths_to_public_paths() {
    assert_eq!(base("/").add("/foo/"), "/foo/");
    assert_eq!(base("/subpath/").add("/"), "/subpath/");
    assert_eq!(base("/subpath/").add("/foo/"), "/subpath/foo/");
    assert_eq!(base("/subpath/").add(""), "/subpath");
}
