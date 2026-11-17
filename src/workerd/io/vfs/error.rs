// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/// The kind of a file system node.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum FsType {
    File,
    Directory,
    Symlink,
}

/// An error from a file system operation. The variants and their order match C++
/// `workerd::FsError`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, thiserror::Error)]
#[repr(u8)]
pub enum FsError {
    #[error("path segment is not a directory")]
    NotDirectory,
    #[error("directory is not empty")]
    NotEmpty,
    #[error("node is read-only")]
    ReadOnly,
    #[error("not permitted")]
    NotPermitted,
    #[error("not permitted on a directory")]
    NotPermittedOnDirectory,
    #[error("already exists")]
    AlreadyExists,
    #[error("too many open files")]
    TooManyOpenFiles,
    #[error("operation failed")]
    Failed,
    #[error("not supported")]
    NotSupported,
    #[error("invalid path")]
    InvalidPath,
    #[error("file size limit exceeded")]
    FileSizeLimitExceeded,
    #[error("symbolic link depth exceeded")]
    SymlinkDepthExceeded,
    #[error("no such file or directory")]
    NotFound,
}

/// An error from building a read-only directory with [`crate::Builder`]. C++ throws these from
/// `Directory::Builder`; they are mistakes in the runtime's setup, never in user code.
#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum BuilderError {
    #[error("invalid name {0:?}: a name must be a single path component")]
    InvalidName(Vec<u8>),
    #[error("file or directory already exists: {0:?}")]
    AlreadyExists(Vec<u8>),
    #[error("path already exists and is a file: {0:?}")]
    PathIsFile(Vec<u8>),
    #[error("path already exists and is a directory: {0:?}")]
    PathIsDirectory(Vec<u8>),
    #[error("a path must have at least one component")]
    EmptyPath,
}

/// Metadata about a file system node.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Stat {
    pub fs_type: FsType,
    /// The size in bytes; always 0 for directories, symbolic links and devices. Files are limited
    /// to `u32::MAX` bytes, well above any isolate's heap limit.
    pub size: u32,
    /// The last modification time, in nanoseconds since the Unix epoch.
    pub last_modified: i64,
    /// The creation time, in nanoseconds since the Unix epoch.
    pub created: i64,
    pub writable: bool,
    /// True for the device files under `/dev`. Some operations treat devices differently.
    pub device: bool,
}

impl Stat {
    pub(crate) const fn directory(writable: bool) -> Self {
        Self {
            fs_type: FsType::Directory,
            size: 0,
            last_modified: 0,
            created: 0,
            writable,
            device: false,
        }
    }
}
