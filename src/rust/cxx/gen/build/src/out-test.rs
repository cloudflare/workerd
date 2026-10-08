use std::path::Path;

use crate::out::abstractly_relativize_symlink;

#[cfg(not(windows))]
#[test]
fn test_relativize_symlink_unix() {
    assert_eq!(
        abstractly_relativize_symlink("/foo/bar/baz", "/foo/spam/eggs").as_deref(),
        Some(Path::new("../bar/baz")),
    );
    assert_eq!(
        abstractly_relativize_symlink("/foo/bar/../baz", "/foo/spam/eggs"),
        None,
    );
    assert_eq!(
        abstractly_relativize_symlink("/foo/bar/baz", "/foo/spam/./eggs").as_deref(),
        Some(Path::new("../bar/baz")),
    );
}

#[cfg(windows)]
#[test]
fn test_relativize_symlink_windows() {
    use std::path::PathBuf;

    let windows_target = PathBuf::from_iter(["c:\\", "windows", "foo"]);
    let windows_link = PathBuf::from_iter(["c:\\", "users", "link"]);
    let windows_different_volume_link = PathBuf::from_iter(["d:\\", "users", "link"]);

    assert_eq!(
        abstractly_relativize_symlink(&windows_target, windows_link).as_deref(),
        Some(Path::new("..\\windows\\foo")),
    );
    assert_eq!(
        abstractly_relativize_symlink(&windows_target, windows_different_volume_link),
        None,
    );
}
