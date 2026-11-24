// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::OnceCell;
use std::cell::RefCell;
use std::ffi::CStr;
use std::ffi::CString;
use std::rc::Rc;

use crate::BuilderError;
use crate::File;
use crate::FileOrDirectory;
use crate::FsError;
use crate::FsType;
use crate::HostError;
use crate::MemoryCharge;
use crate::MemoryTracker;
use crate::Node;
use crate::Stat;
use crate::VfsHost;
use crate::current_tmp_store;
use crate::entries::EntryMap;
use crate::file::unique_id;
use crate::link::LinkGuard;
use crate::memory::track;
use crate::path;

/// Options for [`Directory::try_open`].
#[derive(Clone, Copy, Debug)]
pub struct OpenOptions {
    /// If the path does not exist, create it as this kind of node: a file or a directory, with
    /// any missing parent directories. Asking for a symbolic link fails with
    /// [`FsError::NotPermitted`].
    pub create_as: Option<FsType>,
    /// If the path names a symbolic link, return what the link resolves to rather than the link.
    /// Links earlier in the path are followed only if this is set.
    pub follow_links: bool,
}

impl Default for OpenOptions {
    fn default() -> Self {
        Self {
            create_as: None,
            follow_links: true,
        }
    }
}

/// Options for [`Directory::remove`].
#[derive(Clone, Copy, Debug, Default)]
pub struct RemoveOptions {
    /// Remove a directory together with its entries.
    pub recursive: bool,
}

/// One entry of a directory listing.
#[derive(Clone)]
pub struct ListEntry {
    pub name: Box<[u8]>,
    pub node: Node,
}

/// A directory. Cloning a `Directory` adds a reference to the same directory.
///
/// An in-memory directory is writable or read-only for its whole life. The `/tmp` directory
/// holds nothing itself: its entries are those of the current [`crate::TmpDirStore`]. A lazy
/// directory loads its contents on first use.
#[derive(Clone)]
pub struct Directory(Rc<DirInner>);

struct DirInner {
    kind: DirKind,
    unique_id: OnceCell<CString>,
    // The charge for the node itself, made by `count_towards_isolate_limit()`.
    self_charge: RefCell<Option<Box<dyn MemoryCharge>>>,
}

enum DirKind {
    Memory {
        writable: bool,
        entries: RefCell<EntryMap<Node>>,
    },
    Tmp,
    Lazy(RefCell<Lazy>),
}

/// What a lazy directory calls for its contents. It returns an error when the host keeps a
/// failure to report once the operation returns.
pub type LazyLoader = Box<dyn FnMut() -> Result<Directory, HostError>>;

enum Lazy {
    // The loader, taken while it runs.
    Pending(Option<LazyLoader>),
    Loaded(Directory),
}

// The directory an operation on `dir` applies to: `dir` itself, the current temporary directory
// for `/tmp`, or the contents of a lazy directory.
enum Target {
    Memory,
    Delegate(Directory),
    // `/tmp` with no store current.
    NoTmpStore,
    // A lazy directory whose loader failed.
    Failed,
}

impl Directory {
    fn from_kind(kind: DirKind) -> Self {
        Self(Rc::new(DirInner {
            kind,
            unique_id: OnceCell::new(),
            self_charge: RefCell::new(None),
        }))
    }

    fn new_memory(writable: bool, entries: EntryMap<Node>) -> Self {
        Self::from_kind(DirKind::Memory {
            writable,
            entries: RefCell::new(entries),
        })
    }

    /// A new, empty, writable directory, not charged to the isolate's memory limit. The runtime
    /// uses this for directories it creates itself.
    pub fn new_writable() -> Self {
        Self::new_memory(true, EntryMap::default())
    }

    /// A new, empty, writable directory, charged to the isolate's memory limit: the kind a
    /// Worker creates.
    pub fn new_writable_counted(host: &dyn VfsHost) -> Self {
        let dir = Self::new_writable();
        dir.count_towards_isolate_limit(host);
        dir
    }

    /// A new, empty, read-only directory.
    pub fn new_empty_readonly() -> Self {
        Builder::default().finish()
    }

    /// The `/tmp` directory, whose entries are those of the current [`crate::TmpDirStore`].
    pub fn new_tmp() -> Self {
        Self::from_kind(DirKind::Tmp)
    }

    /// A directory whose contents are those of the directory `load` returns, called on first
    /// use. If it fails, so does the operation that called it, with [`FsError::Failed`] (or
    /// nothing, for [`Self::count`] and [`Self::list`]), and the next use calls it again, as in
    /// C++.
    pub fn new_lazy(load: LazyLoader) -> Self {
        Self::from_kind(DirKind::Lazy(RefCell::new(Lazy::Pending(Some(load)))))
    }

    /// Returns true if this and `other` are the same directory.
    pub fn is_same_node(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }

    fn target(&self, host: &dyn VfsHost) -> Target {
        match &self.0.kind {
            DirKind::Memory { .. } => Target::Memory,
            DirKind::Tmp => current_tmp_store(host).map_or(Target::NoTmpStore, |store| {
                Target::Delegate(store.directory())
            }),
            DirKind::Lazy(lazy) => load(lazy).map_or(Target::Failed, Target::Delegate),
        }
    }

    /// The metadata of the node at `path`, following symbolic links, or `None` if it does not
    /// exist. An empty path is this directory.
    pub fn stat<P: AsRef<[u8]>>(
        &self,
        host: &dyn VfsHost,
        path: &[P],
    ) -> Option<Result<Stat, FsError>> {
        self.stat_in(host, &mut LinkGuard::default(), path)
    }

    pub(crate) fn stat_in<P: AsRef<[u8]>>(
        &self,
        host: &dyn VfsHost,
        guard: &mut LinkGuard,
        path: &[P],
    ) -> Option<Result<Stat, FsError>> {
        let entries = match self.target(host) {
            Target::Delegate(dir) => return dir.stat_in(host, guard, path),
            Target::NoTmpStore => return path.is_empty().then_some(Ok(Stat::directory(true))),
            Target::Failed => return Some(Err(FsError::Failed)),
            Target::Memory => self.memory_entries(),
        };
        let Some((first, rest)) = path.split_first() else {
            return Some(Ok(Stat::directory(self.memory_writable())));
        };
        let found = entries.borrow().get(first.as_ref()).cloned();
        match found? {
            Node::File(file) => rest.is_empty().then(|| Ok(file.stat())),
            Node::Directory(dir) => dir.stat_in(host, guard, rest),
            Node::SymbolicLink(link) => {
                if let Err(err) = guard.check(&link) {
                    return Some(Err(err));
                }
                // A link that resolves to a file stats the file, whatever the rest of the path.
                match link.resolve_in(host, guard)? {
                    Ok(FileOrDirectory::File(file)) => Some(Ok(file.stat())),
                    Ok(FileOrDirectory::Directory(dir)) => dir.stat_in(host, guard, rest),
                    Err(err) => Some(Err(err)),
                }
            }
        }
    }

    /// The number of entries, or of entries of `type_filter`'s kind.
    pub fn count(&self, host: &dyn VfsHost, type_filter: Option<FsType>) -> usize {
        match self.target(host) {
            Target::Delegate(dir) => dir.count(host, type_filter),
            Target::NoTmpStore | Target::Failed => 0,
            Target::Memory => {
                let entries = self.memory_entries().borrow();
                type_filter.map_or_else(
                    || entries.len(),
                    |fs_type| {
                        entries
                            .iter()
                            .filter(|(_, node)| node.fs_type() == fs_type)
                            .count()
                    },
                )
            }
        }
    }

    /// The entries, in the order `readdir()` reports them: the order in which they were added,
    /// except that removing an entry moves the last entry into its place.
    pub fn list(&self, host: &dyn VfsHost) -> Vec<ListEntry> {
        match self.target(host) {
            Target::Delegate(dir) => dir.list(host),
            Target::NoTmpStore | Target::Failed => Vec::new(),
            Target::Memory => self
                .memory_entries()
                .borrow()
                .iter()
                .map(|(name, node)| ListEntry {
                    name: name.into(),
                    node: node.clone(),
                })
                .collect(),
        }
    }

    /// Opens the node at `path`, or `None` if it does not exist and is not created. An empty path
    /// is this directory.
    ///
    /// With [`OpenOptions::create_as`], a missing node is created, together with any missing
    /// parent directories; in a read-only directory that fails with [`FsError::ReadOnly`]. A
    /// created file is empty.
    pub fn try_open<P: AsRef<[u8]>>(
        &self,
        host: &dyn VfsHost,
        path: &[P],
        options: OpenOptions,
    ) -> Option<Result<Node, FsError>> {
        self.try_open_in(host, &mut LinkGuard::default(), path, options)
    }

    pub(crate) fn try_open_in<P: AsRef<[u8]>>(
        &self,
        host: &dyn VfsHost,
        guard: &mut LinkGuard,
        path: &[P],
        options: OpenOptions,
    ) -> Option<Result<Node, FsError>> {
        let entries = match self.target(host) {
            Target::Delegate(dir) => return dir.try_open_in(host, guard, path, options),
            Target::NoTmpStore => return None,
            Target::Failed => return Some(Err(FsError::Failed)),
            Target::Memory => self.memory_entries(),
        };
        let Some((first, rest)) = path.split_first() else {
            return Some(Ok(Node::Directory(self.clone())));
        };
        // Release the borrow before going further: following a link can lead back here.
        let found = entries.borrow().get(first.as_ref()).cloned();
        if let Some(found) = found {
            return match found {
                Node::File(file) => rest.is_empty().then_some(Ok(Node::File(file))),
                Node::Directory(dir) => {
                    if rest.is_empty() {
                        Some(Ok(Node::Directory(dir)))
                    } else {
                        dir.try_open_in(host, guard, rest, options)
                    }
                }
                Node::SymbolicLink(link) => {
                    if !options.follow_links {
                        return rest.is_empty().then_some(Ok(Node::SymbolicLink(link)));
                    }
                    if let Err(err) = guard.check(&link) {
                        return Some(Err(err));
                    }
                    match link.resolve_in(host, guard)? {
                        Ok(resolved) if rest.is_empty() => Some(Ok(resolved.into())),
                        Ok(FileOrDirectory::File(_)) => None,
                        Ok(FileOrDirectory::Directory(dir)) => {
                            dir.try_open_in(host, guard, rest, options)
                        }
                        Err(err) => Some(Err(err)),
                    }
                }
            };
        }
        let fs_type = options.create_as?;
        if !self.memory_writable() {
            return Some(Err(FsError::ReadOnly));
        }
        self.create(host, guard, first.as_ref(), rest, fs_type)
    }

    // Creates `first`, which does not exist, and below it `rest`.
    fn create<P: AsRef<[u8]>>(
        &self,
        host: &dyn VfsHost,
        guard: &mut LinkGuard,
        first: &[u8],
        rest: &[P],
        fs_type: FsType,
    ) -> Option<Result<Node, FsError>> {
        let entries = self.memory_entries();
        if rest.is_empty() {
            let node = match fs_type {
                FsType::File => Node::File(File::new_writable(host, None)),
                FsType::Directory => Node::Directory(Self::new_writable_counted(host)),
                FsType::Symlink => return Some(Err(FsError::NotPermitted)),
            };
            entries.borrow_mut().push(first, node.clone());
            return Some(Ok(node));
        }
        let dir = Self::new_writable_counted(host);
        let options = OpenOptions {
            create_as: Some(fs_type),
            follow_links: true,
        };
        let result = dir.try_open_in(host, guard, rest, options)?;
        // The new directory is kept whenever the nested open produced a result, even an error.
        entries.borrow_mut().push(first, Node::Directory(dir));
        Some(result)
    }

    /// Adds `node` to this directory as `name`.
    ///
    /// # Errors
    ///
    /// [`FsError::InvalidPath`] if `name` is not a single path component,
    /// [`FsError::AlreadyExists`] if the name is taken, and [`FsError::NotPermitted`] if this
    /// directory is read-only.
    pub fn add(&self, host: &dyn VfsHost, name: &[u8], node: Node) -> Result<(), FsError> {
        match self.target(host) {
            Target::Delegate(dir) => dir.add(host, name, node),
            Target::NoTmpStore => Err(FsError::NotPermitted),
            Target::Failed => Err(FsError::Failed),
            Target::Memory => {
                if !self.memory_writable() {
                    return Err(FsError::NotPermitted);
                }
                if !path::is_single_component(name) {
                    return Err(FsError::InvalidPath);
                }
                let mut entries = self.memory_entries().borrow_mut();
                if entries.contains(name) {
                    return Err(FsError::AlreadyExists);
                }
                entries.push(name, node);
                Ok(())
            }
        }
    }

    /// Removes the node at `path` and returns whether there was one. A symbolic link is removed
    /// itself, never its target. A directory with entries is removed only with
    /// [`RemoveOptions::recursive`]. An empty path removes nothing.
    ///
    /// # Errors
    ///
    /// [`FsError::ReadOnly`] if the directory holding the node is read-only,
    /// [`FsError::NotEmpty`] for a directory with entries without `recursive`, and
    /// [`FsError::NotDirectory`] if a path component before the last is a file or a link.
    pub fn remove<P: AsRef<[u8]>>(
        &self,
        host: &dyn VfsHost,
        path: &[P],
        options: RemoveOptions,
    ) -> Result<bool, FsError> {
        let entries = match self.target(host) {
            Target::Delegate(dir) => return dir.remove(host, path, options),
            Target::NoTmpStore => return Ok(false),
            Target::Failed => return Err(FsError::Failed),
            Target::Memory => self.memory_entries(),
        };
        if !self.memory_writable() {
            return Err(FsError::ReadOnly);
        }
        let Some((first, rest)) = path.split_first() else {
            return Ok(false);
        };
        let first = first.as_ref();
        let found = entries.borrow().get(first).cloned();
        match found {
            None => Ok(false),
            Some(Node::File(_) | Node::SymbolicLink(_)) => {
                if !rest.is_empty() {
                    return Err(FsError::NotDirectory);
                }
                Ok(entries.borrow_mut().remove(first))
            }
            Some(Node::Directory(dir)) => {
                if !rest.is_empty() {
                    return dir.remove(host, rest, options);
                }
                // Counting first, as C++ does, loads a lazy directory even when `recursive`.
                if dir.count(host, None) > 0 && !options.recursive {
                    return Err(FsError::NotEmpty);
                }
                Ok(entries.borrow_mut().remove(first))
            }
        }
    }

    /// A UUID that identifies this directory, the same on every call, or `None` if one cannot be
    /// made because no request is current. A lazy directory has the identifier of its contents.
    /// NUL-terminated for C++, which keeps a pointer to it.
    pub fn unique_id(&self, host: &dyn VfsHost) -> Option<&CStr> {
        if let DirKind::Lazy(lazy) = &self.0.kind
            && self.0.unique_id.get().is_none()
        {
            // The contents' identifier never changes once made, so a copy of it stays equal.
            let id = load(lazy).ok()?.unique_id(host)?.to_owned();
            self.0.unique_id.set(id).ok()?;
        }
        unique_id(&self.0.unique_id, host)
    }

    /// Charges the directory itself to the isolate's memory limit, once. Only writable in-memory
    /// directories are charged; the runtime's read-only directories are not.
    pub fn count_towards_isolate_limit(&self, host: &dyn VfsHost) {
        if !matches!(self.0.kind, DirKind::Memory { writable: true, .. }) {
            return;
        }
        let mut self_charge = self.0.self_charge.borrow_mut();
        if self_charge.is_none() {
            *self_charge = Some(host.charge_memory(size_of::<DirInner>()));
        }
    }

    /// The name heap snapshots give this kind of directory.
    pub fn memory_name(&self) -> &'static CStr {
        match &self.0.kind {
            DirKind::Memory { .. } => c"Directory",
            DirKind::Tmp => c"TmpDirectory",
            DirKind::Lazy(_) => c"LazyDirectory",
        }
    }

    /// The size of the directory itself, without its entries.
    pub const fn memory_self_size(&self) -> usize {
        size_of::<DirInner>()
    }

    /// Reports the directory to a heap snapshot as a node of its own, with an edge named `edge`
    /// from the current node, and everything in it; see [`MemoryTracker`].
    pub fn track_memory(&self, tracker: &mut dyn MemoryTracker, edge: &'static CStr) {
        track(tracker, edge, FileOrDirectory::Directory(self.clone()));
    }

    /// What identifies the directory in a heap snapshot: the address of the state its handles
    /// share.
    pub(crate) fn memory_id(&self) -> usize {
        Rc::as_ptr(&self.0).addr()
    }

    /// The files and directories a heap snapshot reports inside this directory, in order.
    /// Symbolic links are left out, as are the contents of `/tmp`, which belong to a request. A
    /// lazy directory has those of its contents once they are loaded, and none before.
    pub(crate) fn memory_children(&self) -> Vec<FileOrDirectory> {
        match &self.0.kind {
            DirKind::Memory { entries, .. } => entries
                .borrow()
                .iter()
                .filter_map(|(_, node)| match node {
                    Node::File(file) => Some(FileOrDirectory::File(file.clone())),
                    Node::Directory(dir) => Some(FileOrDirectory::Directory(dir.clone())),
                    Node::SymbolicLink(_) => None,
                })
                .collect(),
            DirKind::Tmp => Vec::new(),
            DirKind::Lazy(lazy) => match &*lazy.borrow() {
                Lazy::Loaded(dir) => dir.memory_children(),
                Lazy::Pending(_) => Vec::new(),
            },
        }
    }

    fn memory_entries(&self) -> &RefCell<EntryMap<Node>> {
        match &self.0.kind {
            DirKind::Memory { entries, .. } => entries,
            DirKind::Tmp | DirKind::Lazy(_) => unreachable!("not an in-memory directory"),
        }
    }

    fn memory_writable(&self) -> bool {
        matches!(self.0.kind, DirKind::Memory { writable: true, .. })
    }
}

// The contents of a lazy directory, loading them if they are not loaded yet. A loader that
// fails is kept for the next use.
fn load(lazy: &RefCell<Lazy>) -> Result<Directory, HostError> {
    let loader = match &mut *lazy.borrow_mut() {
        Lazy::Loaded(dir) => return Ok(dir.clone()),
        Lazy::Pending(loader) => loader.take(),
    };
    // A loader that uses its own directory, directly or through a link, would recurse forever;
    // that use fails instead, with no failure kept by the host, so with `FsError::Failed` alone.
    let Some(mut loader) = loader else {
        return Err(HostError);
    };
    let result = loader();
    *lazy.borrow_mut() = match &result {
        Ok(dir) => Lazy::Loaded(dir.clone()),
        Err(HostError) => Lazy::Pending(Some(loader)),
    };
    result
}

/// Builds a read-only directory, whose entries may themselves be writable.
#[derive(Default)]
pub struct Builder {
    entries: EntryMap<BuilderEntry>,
}

enum BuilderEntry {
    Node(FileOrDirectory),
    Builder(Builder),
}

impl Builder {
    /// Adds `node` as `name`.
    ///
    /// # Errors
    ///
    /// [`BuilderError::InvalidName`] if `name` is not a single path component, and
    /// [`BuilderError::AlreadyExists`] if it is taken.
    pub fn add(
        &mut self,
        name: &[u8],
        node: impl Into<FileOrDirectory>,
    ) -> Result<(), BuilderError> {
        self.add_entry(name, BuilderEntry::Node(node.into()))
    }

    /// Adds the directory `builder` will build as `name`.
    ///
    /// # Errors
    ///
    /// As for [`add`](Self::add).
    pub fn add_builder(&mut self, name: &[u8], builder: Self) -> Result<(), BuilderError> {
        self.add_entry(name, BuilderEntry::Builder(builder))
    }

    fn add_entry(&mut self, name: &[u8], entry: BuilderEntry) -> Result<(), BuilderError> {
        if !path::is_single_component(name) {
            return Err(BuilderError::InvalidName(name.to_vec()));
        }
        if self.entries.contains(name) {
            return Err(BuilderError::AlreadyExists(name.to_vec()));
        }
        self.entries.push(name, entry);
        Ok(())
    }

    /// Adds `node` at `path`, adding read-only directories for its missing parents.
    ///
    /// # Errors
    ///
    /// [`BuilderError::EmptyPath`] for an empty path, [`BuilderError::PathIsFile`] or
    /// [`BuilderError::PathIsDirectory`] if a parent was added as a node rather than built here,
    /// and the errors of [`add`](Self::add) for the last component.
    pub fn add_path<P: AsRef<[u8]>>(
        &mut self,
        path: &[P],
        node: impl Into<FileOrDirectory>,
    ) -> Result<(), BuilderError> {
        match path {
            [] => Err(BuilderError::EmptyPath),
            [name] => self.add(name.as_ref(), node),
            [first, rest @ ..] => {
                let first = first.as_ref();
                if !self.entries.contains(first) {
                    self.entries
                        .push(first, BuilderEntry::Builder(Self::default()));
                }
                match self.entries.get_mut(first) {
                    Some(BuilderEntry::Builder(builder)) => builder.add_path(rest, node),
                    Some(BuilderEntry::Node(FileOrDirectory::File(_))) => {
                        Err(BuilderError::PathIsFile(first.to_vec()))
                    }
                    Some(BuilderEntry::Node(FileOrDirectory::Directory(_))) => {
                        Err(BuilderError::PathIsDirectory(first.to_vec()))
                    }
                    None => unreachable!("the entry was just added"),
                }
            }
        }
    }

    /// The read-only directory, with its entries in the order they were added.
    pub fn finish(self) -> Directory {
        let mut entries = EntryMap::default();
        for (name, entry) in self.entries.into_iter() {
            let node = match entry {
                BuilderEntry::Node(node) => node.into(),
                BuilderEntry::Builder(builder) => Node::Directory(builder.finish()),
            };
            entries.push(&name, node);
        }
        Directory::new_memory(false, entries)
    }
}

#[cfg(test)]
#[path = "dir-test.rs"]
mod tests;
