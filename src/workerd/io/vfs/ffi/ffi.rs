// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The bridge between the C++ VFS facade (`//src/workerd/io:worker-fs-rust.c++`) and the `vfs`
//! crate.
//!
//! Every function here is called by C++ with the isolate lock held. Each that runs an operation
//! takes the C++ `VfsHost` for it, which [`CppHost`] adapts to [`vfs::VfsHost`]. Nodes cross as
//! boxes of handles, so C++ can hold them; results that may be a node, an error or nothing cross
//! as an [`RsNode`]. An error crosses as its C++ `workerd::FsError` value, and a status of
//! [`OK`] or [`NONE`] means there is none.
//!
//! A path crosses as its components joined by `/`, which a component never contains.

#![allow(
    clippy::unnecessary_box_returns,
    clippy::boxed_local,
    clippy::needless_lifetimes,
    reason = "cxx passes opaque Rust types only in boxes, and requires explicit lifetimes on the \
              references it returns"
)]

use std::any::Any;
use std::ffi::CStr;
use std::pin::Pin;

use kj_rs::KjOwn;
use vfs::Directory;
use vfs::ExternalBytes;
use vfs::File;
use vfs::FileOrDirectory;
use vfs::FsError;
use vfs::FsType;
use vfs::HostError;
use vfs::ListEntry;
use vfs::MemoryCharge;
use vfs::Microtask;
use vfs::Node;
use vfs::Stat;
use vfs::Stdio;
use vfs::SymbolicLink;
use vfs::TmpDirStore;

/// The status of a result that holds a value.
pub const OK: i32 = ffi::Status::Ok.repr as i32;
/// The status of a result that holds nothing, as `kj::none`.
pub const NONE: i32 = ffi::Status::None.repr as i32;

#[expect(unsafe_code, reason = "the cxx bridge expands to unsafe FFI glue")]
#[cxx::bridge(namespace = "workerd::vfs")]
pub mod ffi {
    /// The statuses that are not an error. Any other status is an [`FsErrorCode`].
    #[repr(i8)]
    enum Status {
        Ok = -1,
        None = -2,
    }

    /// C++ `workerd::FsError`, in its order, which worker-fs-rust.c++ checks against it.
    enum FsErrorCode {
        NotDirectory,
        NotEmpty,
        ReadOnly,
        NotPermitted,
        NotPermittedOnDirectory,
        AlreadyExists,
        TooManyOpenFiles,
        Failed,
        NotSupported,
        InvalidPath,
        FileSizeLimitExceeded,
        SymlinkDepthExceeded,
        NotFound,
    }

    /// C++ `workerd::FsType`, checked as [`FsErrorCode`] is.
    enum FsTypeCode {
        File,
        Directory,
        Symlink,
    }

    /// C++ `VirtualFileSystem::Stdio`, checked as [`FsErrorCode`] is.
    enum StdioCode {
        In,
        Out,
        Err,
    }

    /// `workerd::Stat`, with times in nanoseconds since the Unix epoch.
    struct StatOut {
        fs_type: u8,
        size: u32,
        last_modified: i64,
        created: i64,
        writable: bool,
        device: bool,
    }

    /// A `Stat`, an error or nothing.
    struct StatResult {
        status: i32,
        stat: StatOut,
    }

    /// A link's target as written, if it keeps one.
    struct RawTarget {
        kept: bool,
        target: Vec<u8>,
    }

    /// A count, or an error.
    struct U32Result {
        status: i32,
        value: u32,
    }

    /// `VirtualFileSystem::OpenOptions`.
    struct FdOpenOptions {
        read: bool,
        write: bool,
        append: bool,
        create: bool,
        exclusive: bool,
        truncate: bool,
        follow_links: bool,
    }

    /// What an [`RsNode`] holds.
    enum NodeKind {
        None,
        Error,
        File,
        Directory,
        SymbolicLink,
    }

    unsafe extern "C++" {
        include!("workerd/io/vfs/ffi/host.h");

        type VfsHost;
        type FdObserver;
        type LazyLoader;
        type OwnedBytes;
        type SnapshotTracker;

        fn isolate(self: &VfsHost) -> usize;
        #[cxx_name = "blobSizeLimit"]
        fn blob_size_limit(self: &VfsHost) -> usize;
        #[cxx_name = "fillRandom"]
        fn fill_random(self: &VfsHost, buffer: &mut [u8]) -> bool;
        #[cxx_name = "randomUuid"]
        fn random_uuid(self: &VfsHost) -> String;
        #[cxx_name = "hasIoContext"]
        fn has_io_context(self: &VfsHost) -> bool;
        #[cxx_name = "logStdio"]
        fn log_stdio(self: &VfsHost, stdio: u8, line: &[u8]) -> bool;
        #[cxx_name = "enqueueMicrotask"]
        fn enqueue_microtask(self: &VfsHost, task: Box<RsMicrotask>);
        #[cxx_name = "hasIoContextTmpStore"]
        fn has_io_context_tmp_store(self: &VfsHost) -> bool;
        #[cxx_name = "ioContextTmpStore"]
        fn io_context_tmp_store(self: &VfsHost) -> Box<RsTmpStore>;

        #[cxx_name = "onOpen"]
        fn on_open(self: &FdObserver, open_fds: usize, total_fds: usize);
        #[cxx_name = "onClose"]
        fn on_close(self: &FdObserver, open_fds: usize, total_fds: usize);
        #[cxx_name = "onMaxFds"]
        fn on_max_fds(self: &FdObserver, open_fds: usize);

        fn load(self: Pin<&mut LazyLoader>, loaded: &mut RsLoaded) -> bool;

        fn bytes<'a>(self: &'a OwnedBytes) -> &'a [u8];

        #[cxx_name = "pushNode"]
        fn push_node(
            self: Pin<&mut SnapshotTracker>,
            edge: &'static [u8],
            id: usize,
            name: &'static [u8],
            self_size: usize,
        ) -> bool;
        #[cxx_name = "popNode"]
        fn pop_node(self: Pin<&mut SnapshotTracker>);
        #[cxx_name = "trackBytes"]
        fn track_bytes(
            self: Pin<&mut SnapshotTracker>,
            edge: &'static [u8],
            name: &'static [u8],
            size: usize,
        );
    }

    impl KjOwn<FdObserver> {}
    impl KjOwn<LazyLoader> {}
    impl KjOwn<OwnedBytes> {}

    extern "Rust" {
        type RsFile;
        type RsDirectory;
        type RsSymbolicLink;
        type RsNode;
        type RsListing;
        type RsBuilder;
        type RsTmpStore;
        type RsTmpScope;
        type RsLinkGuard;
        type RsVfs;
        type RsOpenedFile;
        type RsHandle;
        type RsMicrotask;
        type RsLoaded;

        // Files
        fn file_new_writable(host: &VfsHost, size: u32, has_size: bool) -> Box<RsFile>;
        /// # Safety
        /// The `len` bytes at `data` must stay valid and unchanged for as long as the file lives.
        unsafe fn file_new_readable_view(data: *const u8, len: usize) -> Box<RsFile>;
        fn file_new_readable_owned(bytes: KjOwn<OwnedBytes>) -> Box<RsFile>;
        fn file_new_dev_null() -> Box<RsFile>;
        fn file_new_dev_zero() -> Box<RsFile>;
        fn file_new_dev_full() -> Box<RsFile>;
        fn file_new_dev_random() -> Box<RsFile>;
        fn file_add_ref(file: &RsFile) -> Box<RsFile>;
        fn file_is_same(file: &RsFile, other: &RsFile) -> bool;
        fn file_stat(file: &RsFile) -> StatOut;
        fn file_read(file: &RsFile, host: &VfsHost, offset: u32, buffer: &mut [u8]) -> u32;
        fn file_write(file: &RsFile, host: &VfsHost, offset: u32, data: &[u8]) -> U32Result;
        fn file_fill(file: &RsFile, value: u8, offset: u32, has_offset: bool) -> i32;
        fn file_resize(file: &RsFile, host: &VfsHost, size: u32) -> i32;
        fn file_set_last_modified(file: &RsFile, date: i64) -> i32;
        fn file_clone(file: &RsFile, host: &VfsHost) -> Box<RsNode>;
        fn file_replace(file: &RsFile, host: &VfsHost, other: &RsFile) -> i32;
        /// The NUL-terminated identifier, or an empty slice outside a request.
        ///
        /// # Safety
        /// The identifier is valid only while the node lives.
        unsafe fn file_unique_id<'a>(file: &'a RsFile, host: &VfsHost) -> &'a [u8];
        fn file_count_towards_isolate_limit(file: &RsFile, host: &VfsHost);
        /// Reports the file to a heap snapshot as a node of its own, with an edge named "file".
        fn file_track_memory(file: &RsFile, tracker: Pin<&mut SnapshotTracker>);

        // Directories
        fn dir_new_writable() -> Box<RsDirectory>;
        fn dir_new_writable_counted(host: &VfsHost) -> Box<RsDirectory>;
        fn dir_new_tmp() -> Box<RsDirectory>;
        fn dir_new_lazy(loader: KjOwn<LazyLoader>) -> Box<RsDirectory>;
        fn dir_add_ref(dir: &RsDirectory) -> Box<RsDirectory>;
        fn dir_is_same(dir: &RsDirectory, other: &RsDirectory) -> bool;
        fn dir_stat(dir: &RsDirectory, host: &VfsHost, path: &[u8]) -> StatResult;
        fn dir_count(dir: &RsDirectory, host: &VfsHost, type_filter: i32) -> usize;
        fn dir_list(dir: &RsDirectory, host: &VfsHost) -> Box<RsListing>;
        fn dir_try_open(
            dir: &RsDirectory,
            host: &VfsHost,
            path: &[u8],
            create_as: i32,
            follow_links: bool,
        ) -> Box<RsNode>;
        fn dir_add(dir: &RsDirectory, host: &VfsHost, name: &[u8], node: Box<RsNode>) -> i32;
        fn dir_remove(dir: &RsDirectory, host: &VfsHost, path: &[u8], recursive: bool)
        -> U32Result;
        /// The NUL-terminated identifier, or an empty slice outside a request.
        ///
        /// # Safety
        /// The identifier is valid only while the node lives.
        unsafe fn dir_unique_id<'a>(dir: &'a RsDirectory, host: &VfsHost) -> &'a [u8];
        fn dir_count_towards_isolate_limit(dir: &RsDirectory, host: &VfsHost);
        /// Reports the directory to a heap snapshot as a node of its own, with an edge named
        /// "directory".
        fn dir_track_memory(dir: &RsDirectory, tracker: Pin<&mut SnapshotTracker>);

        // Listings
        fn listing_len(listing: &RsListing) -> usize;
        /// # Safety
        /// The name is valid only while `listing` lives.
        unsafe fn listing_name<'a>(listing: &'a RsListing, index: usize) -> &'a [u8];
        fn listing_node(listing: &RsListing, index: usize) -> Box<RsNode>;

        // Nodes
        fn node_kind(node: &RsNode) -> NodeKind;
        fn node_error(node: &RsNode) -> i32;
        fn node_into_file(node: Box<RsNode>) -> Box<RsFile>;
        fn node_into_directory(node: Box<RsNode>) -> Box<RsDirectory>;
        fn node_into_symbolic_link(node: Box<RsNode>) -> Box<RsSymbolicLink>;
        fn node_from_file(file: &RsFile) -> Box<RsNode>;
        fn node_from_directory(dir: &RsDirectory) -> Box<RsNode>;
        fn node_from_symbolic_link(link: &RsSymbolicLink) -> Box<RsNode>;

        // Symbolic links
        fn link_new(
            root: &RsDirectory,
            target: &[u8],
            raw_target: &RawTarget,
        ) -> Box<RsSymbolicLink>;
        fn link_add_ref(link: &RsSymbolicLink) -> Box<RsSymbolicLink>;
        fn link_is_same(link: &RsSymbolicLink, other: &RsSymbolicLink) -> bool;
        fn link_target(link: &RsSymbolicLink) -> Vec<u8>;
        fn link_raw_target(link: &RsSymbolicLink) -> RawTarget;
        fn link_resolve(link: &RsSymbolicLink, host: &VfsHost) -> Box<RsNode>;
        /// As `link_resolve`, counting the links followed in `guard`.
        fn link_resolve_in(
            link: &RsSymbolicLink,
            host: &VfsHost,
            guard: &mut RsLinkGuard,
        ) -> Box<RsNode>;
        /// The NUL-terminated identifier, or an empty slice outside a request.
        ///
        /// # Safety
        /// The identifier is valid only while the node lives.
        unsafe fn link_unique_id<'a>(link: &'a RsSymbolicLink, host: &VfsHost) -> &'a [u8];
        fn link_count_towards_isolate_limit(link: &RsSymbolicLink, host: &VfsHost);
        fn link_guard_new() -> Box<RsLinkGuard>;
        fn link_guard_check(guard: &mut RsLinkGuard, link: &RsSymbolicLink) -> i32;

        // Read-only directories
        fn builder_new() -> Box<RsBuilder>;
        fn builder_add(builder: &mut RsBuilder, name: &[u8], node: Box<RsNode>) -> Result<()>;
        fn builder_finish(builder: Box<RsBuilder>) -> Box<RsDirectory>;

        // Temporary directory stores
        fn tmp_store_new() -> Box<RsTmpStore>;
        fn tmp_store_add_ref(store: &RsTmpStore) -> Box<RsTmpStore>;
        fn tmp_store_directory(store: &RsTmpStore) -> Box<RsDirectory>;
        fn tmp_scope_enter() -> Box<RsTmpScope>;
        fn tmp_scope_store(scope: &RsTmpScope) -> Box<RsTmpStore>;

        // File systems
        fn vfs_new(root: &RsDirectory, observer: KjOwn<FdObserver>) -> Box<RsVfs>;
        fn vfs_root(vfs: &RsVfs) -> Box<RsDirectory>;
        fn vfs_open_fd(
            vfs: &RsVfs,
            host: &VfsHost,
            path: &[u8],
            options: &FdOpenOptions,
        ) -> Box<RsOpenedFile>;
        fn vfs_close_fd(vfs: &RsVfs, fd: i32);
        fn vfs_wrap_fd(vfs: &RsVfs, fd: i32) -> Box<RsHandle>;
        fn vfs_try_get_fd(vfs: &RsVfs, fd: i32) -> Box<RsOpenedFile>;
        fn vfs_get_stdio(vfs: &RsVfs, stdio: u8) -> Box<RsOpenedFile>;
        fn vfs_lock(vfs: &RsVfs, locators: Vec<String>) -> Box<RsHandle>;
        fn vfs_is_locked(vfs: &RsVfs, locator: &str) -> bool;

        // Open file descriptors
        fn opened_status(opened: &RsOpenedFile) -> i32;
        fn opened_fd(opened: &RsOpenedFile) -> i32;
        fn opened_read(opened: &RsOpenedFile) -> bool;
        fn opened_write(opened: &RsOpenedFile) -> bool;
        fn opened_append(opened: &RsOpenedFile) -> bool;
        fn opened_node(opened: &RsOpenedFile) -> Box<RsNode>;
        fn opened_position(opened: &RsOpenedFile) -> u32;
        fn opened_set_position(opened: &RsOpenedFile, position: u32);

        fn run_microtask(task: Box<RsMicrotask>, host: &VfsHost);

        fn loaded_set(loaded: &mut RsLoaded, dir: Box<RsDirectory>);
    }
}

pub struct RsFile(File);
pub struct RsDirectory(Directory);
pub struct RsSymbolicLink(SymbolicLink);
pub struct RsNode(Option<Result<Node, FsError>>);
pub struct RsListing(Vec<ListEntry>);
pub struct RsBuilder(vfs::Builder);
pub struct RsTmpStore(TmpDirStore);
/// A store made current on this thread while the scope lives, or, if one already was, a store
/// that isn't current.
pub struct RsTmpScope {
    _scope: Option<vfs::TmpDirStoreScope>,
    store: TmpDirStore,
}
pub struct RsLinkGuard(vfs::LinkGuard);
pub struct RsVfs(vfs::VirtualFileSystem);
/// An open file descriptor, or the status of why there is none: an error, or [`NONE`] for a
/// descriptor that isn't open.
pub struct RsOpenedFile(Result<vfs::OpenedFile, i32>);
/// A handle that releases something when dropped: a wrapped file descriptor or a lock.
pub struct RsHandle {
    _handle: Box<dyn Any>,
}
pub struct RsMicrotask(Microtask);
/// Where a lazy directory's loader puts what it loaded.
pub struct RsLoaded(Option<Directory>);

/// [`vfs::VfsHost`] over the C++ host of one operation.
struct CppHost<'a>(&'a ffi::VfsHost);

/// Memory charged through C++ JSG's external memory accounting.
struct Charge(jsg::ExternalMemoryAdjustment);

impl MemoryCharge for Charge {
    fn set(&mut self, bytes: usize) {
        self.0.set(bytes);
    }
}

impl vfs::VfsHost for CppHost<'_> {
    fn blob_size_limit(&self) -> usize {
        self.0.blob_size_limit()
    }

    #[expect(unsafe_code, reason = "reaches the locked isolate through its address")]
    fn charge_memory(&self, bytes: usize) -> Box<dyn MemoryCharge> {
        let isolate = self.0.isolate() as *mut jsg::v8::ffi::Isolate;
        // SAFETY: C++ runs every operation with this isolate locked. The reference lives only for
        // this call, which uses no other `Lock` or `Isolate` reference for the isolate: a Rust
        // JSG frame further up the stack, if any, is suspended until it returns.
        let isolate = unsafe { jsg::Isolate::from_raw(isolate) };
        Box::new(Charge(isolate.external_memory_adjustment(bytes)))
    }

    fn fill_random(&self, buffer: &mut [u8]) -> bool {
        self.0.fill_random(buffer)
    }

    fn random_uuid(&self) -> Option<String> {
        Some(self.0.random_uuid()).filter(|uuid| !uuid.is_empty())
    }

    fn has_io_context(&self) -> bool {
        self.0.has_io_context()
    }

    fn log_stdio(&self, stdio: Stdio, line: &[u8]) -> Result<(), HostError> {
        if self.0.log_stdio(stdio_code(stdio).repr, line) {
            Ok(())
        } else {
            Err(HostError)
        }
    }

    fn enqueue_microtask(&self, task: Microtask) {
        self.0.enqueue_microtask(Box::new(RsMicrotask(task)));
    }

    fn io_context_tmp_store(&self) -> Option<TmpDirStore> {
        self.0
            .has_io_context_tmp_store()
            .then(|| self.0.io_context_tmp_store().0)
    }
}

/// [`vfs::FdObserver`] over a C++ `VirtualFileSystem::Observer`.
struct CppObserver(KjOwn<ffi::FdObserver>);

impl vfs::FdObserver for CppObserver {
    fn on_open(&self, open_fds: usize, total_fds: usize) {
        self.0.on_open(open_fds, total_fds);
    }

    fn on_close(&self, open_fds: usize, total_fds: usize) {
        self.0.on_close(open_fds, total_fds);
    }

    fn on_max_fds(&self, open_fds: usize) {
        self.0.on_max_fds(open_fds);
    }
}

/// Bytes that C++ keeps alive for as long as the file that views them.
struct BytesView {
    data: *const u8,
    len: usize,
}

impl ExternalBytes for BytesView {
    #[expect(unsafe_code, reason = "views bytes that C++ keeps alive")]
    fn bytes(&self) -> &[u8] {
        // An empty `kj::ArrayPtr` may be null, which a slice may not be, even an empty one.
        if self.len == 0 {
            return &[];
        }
        // SAFETY: `file_new_readable_view()`'s caller guarantees that the bytes stay valid and
        // unchanged for as long as the file lives, and the file owns this view. They are not null,
        // since there are some.
        unsafe { std::slice::from_raw_parts(self.data, self.len) }
    }
}

/// Bytes that C++ owns and the file keeps, freed through C++ when the file is destroyed.
struct CppBytes(KjOwn<ffi::OwnedBytes>);

impl ExternalBytes for CppBytes {
    fn bytes(&self) -> &[u8] {
        self.0.bytes()
    }
}

/// [`vfs::MemoryTracker`] over the C++ `jsg::MemoryTracker` of a heap snapshot. Names cross
/// NUL-terminated, since C++ keeps them as `kj::StringPtr`s.
struct CppTracker<'a>(Pin<&'a mut ffi::SnapshotTracker>);

impl vfs::MemoryTracker for CppTracker<'_> {
    fn push_node(
        &mut self,
        edge: &'static CStr,
        id: usize,
        name: &'static CStr,
        self_size: usize,
    ) -> bool {
        self.0.as_mut().push_node(
            edge.to_bytes_with_nul(),
            id,
            name.to_bytes_with_nul(),
            self_size,
        )
    }

    fn pop_node(&mut self) {
        self.0.as_mut().pop_node();
    }

    fn track_bytes(&mut self, edge: &'static CStr, name: &'static CStr, size: usize) {
        self.0
            .as_mut()
            .track_bytes(edge.to_bytes_with_nul(), name.to_bytes_with_nul(), size);
    }
}

/// The status C++ reads an error from: its `workerd::FsError` value.
fn error_status(error: FsError) -> i32 {
    let code = match error {
        FsError::NotDirectory => ffi::FsErrorCode::NotDirectory,
        FsError::NotEmpty => ffi::FsErrorCode::NotEmpty,
        FsError::ReadOnly => ffi::FsErrorCode::ReadOnly,
        FsError::NotPermitted => ffi::FsErrorCode::NotPermitted,
        FsError::NotPermittedOnDirectory => ffi::FsErrorCode::NotPermittedOnDirectory,
        FsError::AlreadyExists => ffi::FsErrorCode::AlreadyExists,
        FsError::TooManyOpenFiles => ffi::FsErrorCode::TooManyOpenFiles,
        FsError::Failed => ffi::FsErrorCode::Failed,
        FsError::NotSupported => ffi::FsErrorCode::NotSupported,
        FsError::InvalidPath => ffi::FsErrorCode::InvalidPath,
        FsError::FileSizeLimitExceeded => ffi::FsErrorCode::FileSizeLimitExceeded,
        FsError::SymlinkDepthExceeded => ffi::FsErrorCode::SymlinkDepthExceeded,
        FsError::NotFound => ffi::FsErrorCode::NotFound,
    };
    i32::from(code.repr)
}

const fn fs_type_code(fs_type: FsType) -> ffi::FsTypeCode {
    match fs_type {
        FsType::File => ffi::FsTypeCode::File,
        FsType::Directory => ffi::FsTypeCode::Directory,
        FsType::Symlink => ffi::FsTypeCode::Symlink,
    }
}

const fn stdio_code(stdio: Stdio) -> ffi::StdioCode {
    match stdio {
        Stdio::In => ffi::StdioCode::In,
        Stdio::Out => ffi::StdioCode::Out,
        Stdio::Err => ffi::StdioCode::Err,
    }
}

fn status(result: Result<(), FsError>) -> i32 {
    match result {
        Ok(()) => OK,
        Err(error) => error_status(error),
    }
}

fn stat_out(stat: Stat) -> ffi::StatOut {
    ffi::StatOut {
        fs_type: fs_type_code(stat.fs_type).repr,
        size: stat.size,
        last_modified: stat.last_modified,
        created: stat.created,
        writable: stat.writable,
        device: stat.device,
    }
}

fn fs_type(value: i32) -> Option<FsType> {
    let code = ffi::FsTypeCode {
        repr: u8::try_from(value).ok()?,
    };
    match code {
        ffi::FsTypeCode::File => Some(FsType::File),
        ffi::FsTypeCode::Directory => Some(FsType::Directory),
        ffi::FsTypeCode::Symlink => Some(FsType::Symlink),
        _ => None,
    }
}

// The components of a path joined by `/`.
fn components(path: &[u8]) -> Vec<&[u8]> {
    if path.is_empty() {
        Vec::new()
    } else {
        path.split(|&b| b == b'/').collect()
    }
}

fn node(node: impl Into<Node>) -> Box<RsNode> {
    Box::new(RsNode(Some(Ok(node.into()))))
}

fn node_result(result: Option<Result<impl Into<Node>, FsError>>) -> Box<RsNode> {
    Box::new(RsNode(result.map(|result| result.map(Into::into))))
}

// Files

fn file_new_writable(host: &ffi::VfsHost, size: u32, has_size: bool) -> Box<RsFile> {
    let size = has_size.then_some(size);
    Box::new(RsFile(File::new_writable(&CppHost(host), size)))
}

#[expect(
    unsafe_code,
    reason = "C++ keeps the bytes alive for as long as the file"
)]
unsafe fn file_new_readable_view(data: *const u8, len: usize) -> Box<RsFile> {
    Box::new(RsFile(File::new_readable(Box::new(BytesView {
        data,
        len,
    }))))
}

fn file_new_readable_owned(bytes: KjOwn<ffi::OwnedBytes>) -> Box<RsFile> {
    Box::new(RsFile(File::new_readable(Box::new(CppBytes(bytes)))))
}

fn file_new_dev_null() -> Box<RsFile> {
    Box::new(RsFile(File::new_dev_null()))
}

fn file_new_dev_zero() -> Box<RsFile> {
    Box::new(RsFile(File::new_dev_zero()))
}

fn file_new_dev_full() -> Box<RsFile> {
    Box::new(RsFile(File::new_dev_full()))
}

fn file_new_dev_random() -> Box<RsFile> {
    Box::new(RsFile(File::new_dev_random()))
}

fn file_add_ref(file: &RsFile) -> Box<RsFile> {
    Box::new(RsFile(file.0.clone()))
}

fn file_is_same(file: &RsFile, other: &RsFile) -> bool {
    file.0.is_same_node(&other.0)
}

fn file_stat(file: &RsFile) -> ffi::StatOut {
    stat_out(file.0.stat())
}

fn file_read(file: &RsFile, host: &ffi::VfsHost, offset: u32, buffer: &mut [u8]) -> u32 {
    file.0.read(&CppHost(host), offset, buffer)
}

fn file_write(file: &RsFile, host: &ffi::VfsHost, offset: u32, data: &[u8]) -> ffi::U32Result {
    match file.0.write(&CppHost(host), offset, data) {
        Ok(value) => ffi::U32Result { status: OK, value },
        Err(error) => ffi::U32Result {
            status: error_status(error),
            value: 0,
        },
    }
}

fn file_fill(file: &RsFile, value: u8, offset: u32, has_offset: bool) -> i32 {
    status(file.0.fill(value, has_offset.then_some(offset)))
}

fn file_resize(file: &RsFile, host: &ffi::VfsHost, size: u32) -> i32 {
    status(file.0.resize(&CppHost(host), size))
}

fn file_set_last_modified(file: &RsFile, date: i64) -> i32 {
    status(file.0.set_last_modified(date))
}

fn file_clone(file: &RsFile, host: &ffi::VfsHost) -> Box<RsNode> {
    node_result(Some(file.0.clone_file(&CppHost(host))))
}

fn file_replace(file: &RsFile, host: &ffi::VfsHost, other: &RsFile) -> i32 {
    status(file.0.replace(&CppHost(host), &other.0))
}

#[expect(
    unsafe_code,
    reason = "cxx requires unsafe to return a borrow of an argument"
)]
unsafe fn file_unique_id<'a>(file: &'a RsFile, host: &ffi::VfsHost) -> &'a [u8] {
    file.0
        .unique_id(&CppHost(host))
        .map_or(&[], CStr::to_bytes_with_nul)
}

fn file_count_towards_isolate_limit(file: &RsFile, host: &ffi::VfsHost) {
    file.0.count_towards_isolate_limit(&CppHost(host));
}

fn file_track_memory(file: &RsFile, tracker: Pin<&mut ffi::SnapshotTracker>) {
    file.0.track_memory(&mut CppTracker(tracker), c"file");
}

// Directories

fn dir_new_writable() -> Box<RsDirectory> {
    Box::new(RsDirectory(Directory::new_writable()))
}

fn dir_new_writable_counted(host: &ffi::VfsHost) -> Box<RsDirectory> {
    Box::new(RsDirectory(Directory::new_writable_counted(&CppHost(host))))
}

fn dir_new_tmp() -> Box<RsDirectory> {
    Box::new(RsDirectory(Directory::new_tmp()))
}

fn dir_new_lazy(mut load_fn: KjOwn<ffi::LazyLoader>) -> Box<RsDirectory> {
    let load = Box::new(move || {
        let mut slot = RsLoaded(None);
        // On failure, the loader has given the host what went wrong.
        if load_fn.pin_mut().load(&mut slot) {
            slot.0.ok_or(HostError)
        } else {
            Err(HostError)
        }
    });
    Box::new(RsDirectory(Directory::new_lazy(load)))
}

fn dir_add_ref(dir: &RsDirectory) -> Box<RsDirectory> {
    Box::new(RsDirectory(dir.0.clone()))
}

fn dir_is_same(dir: &RsDirectory, other: &RsDirectory) -> bool {
    dir.0.is_same_node(&other.0)
}

fn dir_stat(dir: &RsDirectory, host: &ffi::VfsHost, path: &[u8]) -> ffi::StatResult {
    let empty = ffi::StatOut {
        fs_type: 0,
        size: 0,
        last_modified: 0,
        created: 0,
        writable: false,
        device: false,
    };
    match dir.0.stat(&CppHost(host), &components(path)) {
        None => ffi::StatResult {
            status: NONE,
            stat: empty,
        },
        Some(Ok(stat)) => ffi::StatResult {
            status: OK,
            stat: stat_out(stat),
        },
        Some(Err(error)) => ffi::StatResult {
            status: error_status(error),
            stat: empty,
        },
    }
}

fn dir_count(dir: &RsDirectory, host: &ffi::VfsHost, type_filter: i32) -> usize {
    dir.0.count(&CppHost(host), fs_type(type_filter))
}

fn dir_list(dir: &RsDirectory, host: &ffi::VfsHost) -> Box<RsListing> {
    Box::new(RsListing(dir.0.list(&CppHost(host))))
}

fn dir_try_open(
    dir: &RsDirectory,
    host: &ffi::VfsHost,
    path: &[u8],
    create_as: i32,
    follow_links: bool,
) -> Box<RsNode> {
    let options = vfs::OpenOptions {
        create_as: fs_type(create_as),
        follow_links,
    };
    let result = dir.0.try_open(&CppHost(host), &components(path), options);
    Box::new(RsNode(result))
}

fn dir_add(dir: &RsDirectory, host: &ffi::VfsHost, name: &[u8], node: Box<RsNode>) -> i32 {
    match node.0 {
        Some(Ok(node)) => status(dir.0.add(&CppHost(host), name, node)),
        _ => unreachable!("C++ adds only nodes"),
    }
}

fn dir_remove(
    dir: &RsDirectory,
    host: &ffi::VfsHost,
    path: &[u8],
    recursive: bool,
) -> ffi::U32Result {
    let options = vfs::RemoveOptions { recursive };
    match dir.0.remove(&CppHost(host), &components(path), options) {
        Ok(removed) => ffi::U32Result {
            status: OK,
            value: u32::from(removed),
        },
        Err(error) => ffi::U32Result {
            status: error_status(error),
            value: 0,
        },
    }
}

#[expect(
    unsafe_code,
    reason = "cxx requires unsafe to return a borrow of an argument"
)]
unsafe fn dir_unique_id<'a>(dir: &'a RsDirectory, host: &ffi::VfsHost) -> &'a [u8] {
    dir.0
        .unique_id(&CppHost(host))
        .map_or(&[], CStr::to_bytes_with_nul)
}

fn dir_count_towards_isolate_limit(dir: &RsDirectory, host: &ffi::VfsHost) {
    dir.0.count_towards_isolate_limit(&CppHost(host));
}

fn dir_track_memory(dir: &RsDirectory, tracker: Pin<&mut ffi::SnapshotTracker>) {
    dir.0.track_memory(&mut CppTracker(tracker), c"directory");
}

// Listings

fn listing_len(listing: &RsListing) -> usize {
    listing.0.len()
}

#[expect(
    unsafe_code,
    reason = "cxx requires unsafe to return a borrow of an argument"
)]
unsafe fn listing_name(listing: &RsListing, index: usize) -> &[u8] {
    &listing.0[index].name
}

fn listing_node(listing: &RsListing, index: usize) -> Box<RsNode> {
    node(listing.0[index].node.clone())
}

// Nodes

fn node_kind(node: &RsNode) -> ffi::NodeKind {
    match &node.0 {
        None => ffi::NodeKind::None,
        Some(Err(_)) => ffi::NodeKind::Error,
        Some(Ok(Node::File(_))) => ffi::NodeKind::File,
        Some(Ok(Node::Directory(_))) => ffi::NodeKind::Directory,
        Some(Ok(Node::SymbolicLink(_))) => ffi::NodeKind::SymbolicLink,
    }
}

fn node_error(node: &RsNode) -> i32 {
    match &node.0 {
        Some(Err(error)) => error_status(*error),
        None => NONE,
        Some(Ok(_)) => OK,
    }
}

fn node_into_file(node: Box<RsNode>) -> Box<RsFile> {
    match node.0 {
        Some(Ok(Node::File(file))) => Box::new(RsFile(file)),
        _ => unreachable!("C++ checks node_kind() first"),
    }
}

fn node_into_directory(node: Box<RsNode>) -> Box<RsDirectory> {
    match node.0 {
        Some(Ok(Node::Directory(dir))) => Box::new(RsDirectory(dir)),
        _ => unreachable!("C++ checks node_kind() first"),
    }
}

fn node_into_symbolic_link(node: Box<RsNode>) -> Box<RsSymbolicLink> {
    match node.0 {
        Some(Ok(Node::SymbolicLink(link))) => Box::new(RsSymbolicLink(link)),
        _ => unreachable!("C++ checks node_kind() first"),
    }
}

fn node_from_file(file: &RsFile) -> Box<RsNode> {
    node(file.0.clone())
}

fn node_from_directory(dir: &RsDirectory) -> Box<RsNode> {
    node(dir.0.clone())
}

fn node_from_symbolic_link(link: &RsSymbolicLink) -> Box<RsNode> {
    node(link.0.clone())
}

// Symbolic links

fn link_new(root: &RsDirectory, target: &[u8], raw_target: &ffi::RawTarget) -> Box<RsSymbolicLink> {
    let target = components(target).into_iter().map(<[u8]>::to_vec).collect();
    let raw_target = raw_target.kept.then(|| raw_target.target.clone());
    Box::new(RsSymbolicLink(SymbolicLink::new_with_raw_target(
        root.0.clone(),
        target,
        raw_target,
    )))
}

fn link_add_ref(link: &RsSymbolicLink) -> Box<RsSymbolicLink> {
    Box::new(RsSymbolicLink(link.0.clone()))
}

fn link_is_same(link: &RsSymbolicLink, other: &RsSymbolicLink) -> bool {
    link.0.is_same_node(&other.0)
}

fn link_target(link: &RsSymbolicLink) -> Vec<u8> {
    link.0.target_path().join(&b'/')
}

fn link_raw_target(link: &RsSymbolicLink) -> ffi::RawTarget {
    link.0.raw_target().map_or_else(
        || ffi::RawTarget {
            kept: false,
            target: Vec::new(),
        },
        |target| ffi::RawTarget {
            kept: true,
            target: target.to_vec(),
        },
    )
}

fn link_resolve(link: &RsSymbolicLink, host: &ffi::VfsHost) -> Box<RsNode> {
    node_result(link.0.resolve(&CppHost(host)))
}

fn link_resolve_in(
    link: &RsSymbolicLink,
    host: &ffi::VfsHost,
    guard: &mut RsLinkGuard,
) -> Box<RsNode> {
    node_result(link.0.resolve_in(&CppHost(host), &mut guard.0))
}

#[expect(
    unsafe_code,
    reason = "cxx requires unsafe to return a borrow of an argument"
)]
unsafe fn link_unique_id<'a>(link: &'a RsSymbolicLink, host: &ffi::VfsHost) -> &'a [u8] {
    link.0
        .unique_id(&CppHost(host))
        .map_or(&[], CStr::to_bytes_with_nul)
}

fn link_count_towards_isolate_limit(link: &RsSymbolicLink, host: &ffi::VfsHost) {
    link.0.count_towards_isolate_limit(&CppHost(host));
}

fn link_guard_new() -> Box<RsLinkGuard> {
    Box::new(RsLinkGuard(vfs::LinkGuard::default()))
}

fn link_guard_check(guard: &mut RsLinkGuard, link: &RsSymbolicLink) -> i32 {
    status(guard.0.check(&link.0))
}

// Read-only directories

fn builder_new() -> Box<RsBuilder> {
    Box::new(RsBuilder(vfs::Builder::default()))
}

fn builder_add(
    builder: &mut RsBuilder,
    name: &[u8],
    node: Box<RsNode>,
) -> Result<(), vfs::BuilderError> {
    let entry = match node.0 {
        Some(Ok(Node::File(file))) => FileOrDirectory::File(file),
        Some(Ok(Node::Directory(dir))) => FileOrDirectory::Directory(dir),
        _ => unreachable!("C++ adds only files and directories"),
    };
    builder.0.add(name, entry)
}

fn builder_finish(builder: Box<RsBuilder>) -> Box<RsDirectory> {
    Box::new(RsDirectory(builder.0.finish()))
}

// Temporary directory stores

fn tmp_store_new() -> Box<RsTmpStore> {
    Box::new(RsTmpStore(TmpDirStore::new()))
}

fn tmp_store_add_ref(store: &RsTmpStore) -> Box<RsTmpStore> {
    Box::new(RsTmpStore(store.0.clone()))
}

fn tmp_store_directory(store: &RsTmpStore) -> Box<RsDirectory> {
    Box::new(RsDirectory(store.0.directory()))
}

fn tmp_scope_enter() -> Box<RsTmpScope> {
    let scope = vfs::TmpDirStoreScope::enter();
    let store = scope
        .as_ref()
        .map_or_else(TmpDirStore::new, |scope| scope.store().clone());
    Box::new(RsTmpScope {
        _scope: scope,
        store,
    })
}

fn tmp_scope_store(scope: &RsTmpScope) -> Box<RsTmpStore> {
    Box::new(RsTmpStore(scope.store.clone()))
}

// File systems

fn vfs_new(root: &RsDirectory, observer: KjOwn<ffi::FdObserver>) -> Box<RsVfs> {
    let vfs = vfs::VirtualFileSystem::new(root.0.clone(), Box::new(CppObserver(observer)));
    Box::new(RsVfs(vfs))
}

fn vfs_root(vfs: &RsVfs) -> Box<RsDirectory> {
    Box::new(RsDirectory(vfs.0.root()))
}

fn vfs_open_fd(
    vfs: &RsVfs,
    host: &ffi::VfsHost,
    path: &[u8],
    options: &ffi::FdOpenOptions,
) -> Box<RsOpenedFile> {
    let options = vfs::FdOpenOptions {
        read: options.read,
        write: options.write,
        append: options.append,
        create: options.create,
        exclusive: options.exclusive,
        truncate: options.truncate,
        follow_links: options.follow_links,
    };
    let opened = vfs.0.open_fd(&CppHost(host), &components(path), options);
    Box::new(RsOpenedFile(opened.map_err(error_status)))
}

fn vfs_close_fd(vfs: &RsVfs, fd: i32) {
    vfs.0.close_fd(fd);
}

fn vfs_wrap_fd(vfs: &RsVfs, fd: i32) -> Box<RsHandle> {
    Box::new(RsHandle {
        _handle: Box::new(vfs.0.wrap_fd(fd)),
    })
}

fn vfs_try_get_fd(vfs: &RsVfs, fd: i32) -> Box<RsOpenedFile> {
    Box::new(RsOpenedFile(vfs.0.try_get_fd(fd).ok_or(NONE)))
}

fn vfs_get_stdio(vfs: &RsVfs, stdio: u8) -> Box<RsOpenedFile> {
    let stdio = match (ffi::StdioCode { repr: stdio }) {
        ffi::StdioCode::In => Stdio::In,
        ffi::StdioCode::Out => Stdio::Out,
        ffi::StdioCode::Err => Stdio::Err,
        _ => unreachable!("C++ passes a VirtualFileSystem::Stdio"),
    };
    Box::new(RsOpenedFile(Ok(vfs.0.get_stdio(stdio))))
}

fn vfs_lock(vfs: &RsVfs, locators: Vec<String>) -> Box<RsHandle> {
    Box::new(RsHandle {
        _handle: Box::new(vfs.0.lock(locators)),
    })
}

fn vfs_is_locked(vfs: &RsVfs, locator: &str) -> bool {
    vfs.0.is_locked(locator)
}

// Open file descriptors

fn opened(opened: &RsOpenedFile) -> &vfs::OpenedFile {
    match &opened.0 {
        Ok(opened) => opened,
        Err(_) => unreachable!("C++ checks opened_status() first"),
    }
}

fn opened_status(opened: &RsOpenedFile) -> i32 {
    match opened.0 {
        Ok(_) => OK,
        Err(status) => status,
    }
}

fn opened_fd(file: &RsOpenedFile) -> i32 {
    opened(file).fd()
}

fn opened_read(file: &RsOpenedFile) -> bool {
    opened(file).read()
}

fn opened_write(file: &RsOpenedFile) -> bool {
    opened(file).write()
}

fn opened_append(file: &RsOpenedFile) -> bool {
    opened(file).append()
}

fn opened_node(file: &RsOpenedFile) -> Box<RsNode> {
    node(opened(file).node().clone())
}

fn opened_position(file: &RsOpenedFile) -> u32 {
    opened(file).position()
}

fn opened_set_position(file: &RsOpenedFile, position: u32) {
    opened(file).set_position(position);
}

fn loaded_set(loaded: &mut RsLoaded, dir: Box<RsDirectory>) {
    loaded.0 = Some(dir.0);
}

fn run_microtask(task: Box<RsMicrotask>, host: &ffi::VfsHost) {
    (task.0)(&CppHost(host));
}
