// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Path text evaluated as `kj::Path` evaluates it.
//!
//! The file system takes paths as slices of names that have already been evaluated. These
//! functions turn text into such a path with the rules of `kj::Path::eval()`: `/` separates
//! components, empty and `.` components are dropped, `..` removes the previous component, and a
//! component may not contain NUL.

use thiserror::Error;

/// Why path text could not be evaluated.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Error)]
pub enum PathError {
    #[error("expected a relative path, got an absolute one")]
    Absolute,
    #[error("can't use \"..\" to break out of the starting directory")]
    AboveRoot,
    #[error("NUL character in path component")]
    Nul,
}

/// Evaluates `text` relative to the root, as `kj::Path{}.eval(text)` does. A leading `/` is
/// allowed and changes nothing, since the starting point is already the root.
///
/// # Errors
///
/// [`PathError::AboveRoot`] if a `..` would leave the root, and [`PathError::Nul`] if a component
/// contains NUL.
pub fn eval(text: &[u8]) -> Result<Vec<Vec<u8>>, PathError> {
    let mut parts: Vec<Vec<u8>> = Vec::new();
    for part in text.split(|&b| b == b'/') {
        match part {
            b"" | b"." => {}
            b".." => {
                if parts.pop().is_none() {
                    return Err(PathError::AboveRoot);
                }
            }
            _ => {
                if part.contains(&0) {
                    return Err(PathError::Nul);
                }
                parts.push(part.to_vec());
            }
        }
    }
    Ok(parts)
}

/// Evaluates relative path text, as `kj::Path::parse(text)` does.
///
/// # Errors
///
/// [`PathError::Absolute`] if `text` starts with `/`, and otherwise the errors of [`eval`].
pub fn parse(text: &[u8]) -> Result<Vec<Vec<u8>>, PathError> {
    if text.first() == Some(&b'/') {
        return Err(PathError::Absolute);
    }
    eval(text)
}

/// Whether `name` parses as a path of exactly one component.
///
/// This is the test C++ applies to the name of a new directory entry. Such a name is stored as
/// given, so `"a/"` and `"./a"` are accepted and kept with their separators, as in C++.
pub fn is_single_component(name: &[u8]) -> bool {
    parse(name).is_ok_and(|parts| parts.len() == 1)
}

#[cfg(test)]
#[path = "path-test.rs"]
mod tests;
