// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::OnceCell;
use std::cell::RefCell;
use std::rc::Rc;

use crate::Directory;
use crate::FileOrDirectory;
use crate::FsError;
use crate::FsType;
use crate::MemoryCharge;
use crate::Node;
use crate::OpenOptions;
use crate::Stat;
use crate::VfsHost;
use crate::file::unique_id;

// The most distinct symbolic links one resolution may follow.
const MAX_SYMLINK_DEPTH: usize = 256;

/// A symbolic link: a path, resolved from the root of the file system it was made for. The
/// target need not exist, and may be removed and created again; the link stays valid.
///
/// Cloning a `SymbolicLink` adds a reference to the same link.
#[derive(Clone)]
pub struct SymbolicLink(Rc<LinkInner>);

struct LinkInner {
    root: Directory,
    target: Vec<Vec<u8>>,
    unique_id: OnceCell<String>,
    self_charge: RefCell<Option<Box<dyn MemoryCharge>>>,
}

impl SymbolicLink {
    /// A new link to `target`, an evaluated path from `root`.
    pub fn new(root: Directory, target: Vec<Vec<u8>>) -> Self {
        Self(Rc::new(LinkInner {
            root,
            target,
            unique_id: OnceCell::new(),
            self_charge: RefCell::new(None),
        }))
    }

    /// Returns true if this and `other` are the same link.
    pub fn is_same_node(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }

    /// The metadata of the link itself.
    pub const fn stat(&self) -> Stat {
        Stat {
            fs_type: FsType::Symlink,
            size: 0,
            last_modified: 0,
            created: 0,
            writable: false,
            device: false,
        }
    }

    /// The path the link points to, from the root.
    pub fn target_path(&self) -> &[Vec<u8>] {
        &self.0.target
    }

    /// The file or directory the link points to, following any further links, or `None` if the
    /// target does not exist.
    pub fn resolve(&self, host: &dyn VfsHost) -> Option<Result<FileOrDirectory, FsError>> {
        self.resolve_in(host, &mut LinkGuard::default())
    }

    /// As [`resolve`](Self::resolve), counting the links followed in `guard`, so that a caller
    /// walking a tree can detect a cycle across many resolutions.
    pub fn resolve_in(
        &self,
        host: &dyn VfsHost,
        guard: &mut LinkGuard,
    ) -> Option<Result<FileOrDirectory, FsError>> {
        let resolved =
            self.0
                .root
                .try_open_in(host, guard, &self.0.target, OpenOptions::default())?;
        Some(resolved.map(|node| match node {
            Node::File(file) => FileOrDirectory::File(file),
            Node::Directory(dir) => FileOrDirectory::Directory(dir),
            Node::SymbolicLink(_) => unreachable!("opening with follow_links never yields a link"),
        }))
    }

    /// A UUID that identifies this link, the same on every call, or `None` if one cannot be made
    /// because no request is current.
    pub fn unique_id(&self, host: &dyn VfsHost) -> Option<String> {
        unique_id(&self.0.unique_id, host)
    }

    /// Charges the link itself to the isolate's memory limit, once.
    pub fn count_towards_isolate_limit(&self, host: &dyn VfsHost) {
        let mut self_charge = self.0.self_charge.borrow_mut();
        if self_charge.is_none() {
            *self_charge = Some(host.charge_memory(size_of::<LinkInner>()));
        }
    }
}

/// The symbolic links one resolution has followed, to stop it at a cycle or after
/// 256 distinct links.
///
/// Every operation that resolves a path starts with a new guard and counts every link it follows
/// in it, including links in the targets of earlier links. A caller that walks a tree, such as a
/// recursive `readdir()`, keeps one guard for the whole walk and passes it to
/// [`SymbolicLink::resolve_in`].
#[derive(Default)]
pub struct LinkGuard {
    // The links themselves rather than their addresses, so none can be freed and its address
    // reused while it is counted.
    seen: Vec<SymbolicLink>,
}

impl LinkGuard {
    /// Counts `link` as followed.
    ///
    /// # Errors
    ///
    /// [`FsError::SymlinkDepthExceeded`] if `link` was already followed, or if it is the 257th
    /// distinct link.
    pub fn check(&mut self, link: &SymbolicLink) -> Result<(), FsError> {
        if self.seen.iter().any(|seen| seen.is_same_node(link)) {
            return Err(FsError::SymlinkDepthExceeded);
        }
        self.seen.push(link.clone());
        if self.seen.len() > MAX_SYMLINK_DEPTH {
            return Err(FsError::SymlinkDepthExceeded);
        }
        Ok(())
    }
}
