use std::ffi::OsStr;
use std::path::Component;
use std::path::Path;
use std::path::PathBuf;

use r#gen::fs;

use crate::error::Result;

pub fn manifest_dir() -> Result<PathBuf> {
    crate::env_os("CARGO_MANIFEST_DIR").map(PathBuf::from)
}

pub fn out_dir() -> Result<PathBuf> {
    crate::env_os("OUT_DIR").map(PathBuf::from)
}

// Given a path provided by the user, determines where generated files related
// to that path should go in our out dir. In particular we don't want to
// accidentally write generated code upward of our out dir, even if the user
// passed a path containing lots of `..` or an absolute path.
pub fn local_relative_path(path: &Path) -> PathBuf {
    let mut rel_path = PathBuf::new();
    for component in path.components() {
        match component {
            Component::Prefix(_) | Component::RootDir | Component::CurDir => {}
            Component::ParentDir => drop(rel_path.pop()), // noop if empty
            Component::Normal(name) => rel_path.push(name),
        }
    }
    rel_path
}

pub trait PathExt {
    fn with_appended_extension(&self, suffix: impl AsRef<OsStr>) -> PathBuf;
}

impl PathExt for Path {
    fn with_appended_extension(&self, suffix: impl AsRef<OsStr>) -> PathBuf {
        let Some(file_name) = self.file_name() else {
            return self.to_path_buf();
        };
        let mut appended = file_name.to_owned();
        appended.push(suffix);
        self.with_file_name(appended)
    }
}

#[cfg(unix)]
pub fn symlink_or_copy(
    path_for_symlink: impl AsRef<Path>,
    _path_for_copy: impl AsRef<Path>,
    link: impl AsRef<Path>,
) -> fs::Result<()> {
    fs::symlink_file(path_for_symlink, link)
}

#[cfg(windows)]
pub(crate) fn symlink_or_copy(
    path_for_symlink: impl AsRef<Path>,
    path_for_copy: impl AsRef<Path>,
    link: impl AsRef<Path>,
) -> fs::Result<()> {
    // Pre-Windows 10, symlinks require admin privileges. Since Windows 10, they
    // require Developer Mode. If it fails, fall back to copying the file.
    let path_for_symlink = path_for_symlink.as_ref();
    let link = link.as_ref();
    if fs::symlink_file(path_for_symlink, link).is_err() {
        let path_for_copy = path_for_copy.as_ref();
        fs::copy(path_for_copy, link)?;
    }
    Ok(())
}

#[cfg(not(any(unix, windows)))]
pub(crate) fn symlink_or_copy(
    _path_for_symlink: impl AsRef<Path>,
    path_for_copy: impl AsRef<Path>,
    copy: impl AsRef<Path>,
) -> fs::Result<()> {
    fs::copy(path_for_copy, copy)?;
    Ok(())
}
