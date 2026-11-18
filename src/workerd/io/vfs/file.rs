// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::OnceCell;
use std::cell::RefCell;
use std::ffi::CStr;
use std::ffi::CString;
use std::rc::Rc;
use std::rc::Weak;

use crate::FileOrDirectory;
use crate::FsError;
use crate::FsType;
use crate::HostError;
use crate::MemoryCharge;
use crate::MemoryTracker;
use crate::Stat;
use crate::Stdio;
use crate::VfsHost;
use crate::memory::track;

/// The bytes of a read-only file, owned by the file or kept alive on its behalf.
///
/// A read-only file's contents are controlled by the runtime, not the Worker, so they are not
/// charged to the isolate's memory limit. The C++ facade implements this for views of bundle
/// data that the Worker's script keeps alive.
pub trait ExternalBytes {
    fn bytes(&self) -> &[u8];
}

impl ExternalBytes for Vec<u8> {
    fn bytes(&self) -> &[u8] {
        self
    }
}

impl ExternalBytes for Box<[u8]> {
    fn bytes(&self) -> &[u8] {
        self
    }
}

impl ExternalBytes for &'static [u8] {
    fn bytes(&self) -> &[u8] {
        self
    }
}

/// A file. Cloning a `File` adds a reference to the same file; see [`File::clone_file`] for a
/// copy.
///
/// Only a file that owns its contents, which every file the Worker creates does, is writable;
/// its contents are charged to the isolate's memory limit. Read-only files hold
/// [`ExternalBytes`]. The device files under `/dev` and the stdio files have behaviors of their
/// own.
#[derive(Clone)]
pub struct File(Rc<FileInner>);

struct FileInner {
    kind: FileKind,
    unique_id: OnceCell<CString>,
    // The charge for the node itself, made by `count_towards_isolate_limit()`.
    self_charge: RefCell<Option<Box<dyn MemoryCharge>>>,
}

enum FileKind {
    Data(RefCell<Data>),
    Device(Device),
    Stdio(StdioFile),
}

struct Data {
    contents: Contents,
    last_modified: i64,
}

enum Contents {
    Writable {
        bytes: Vec<u8>,
        charge: Box<dyn MemoryCharge>,
    },
    ReadOnly(Box<dyn ExternalBytes>),
}

impl Contents {
    fn bytes(&self) -> &[u8] {
        match self {
            Self::Writable { bytes, .. } => bytes,
            Self::ReadOnly(bytes) => bytes.bytes(),
        }
    }

    const fn is_writable(&self) -> bool {
        matches!(self, Self::Writable { .. })
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Device {
    // Discards writes; reads are at end of file.
    Null,
    // Discards writes; reads return zeros.
    Zero,
    // Refuses writes; reads return zeros.
    Full,
    // Refuses writes; reads return random bytes within a request.
    Random,
}

// A standard stream. Writes to stdout and stderr are logged one line at a time; reads are at end
// of file.
struct StdioFile {
    stdio: Stdio,
    state: RefCell<StdioState>,
}

#[derive(Default)]
struct StdioState {
    // A line not yet ended by a newline. It is logged when the line ends or, if it does not, by a
    // microtask, so synchronous writes join one line and asynchronous writes start new ones.
    line_buffer: Vec<u8>,
    flush_scheduled: bool,
}

// The longest line kept in a stdio file's line buffer; longer lines keep their end.
const MAX_LINE_BUFFER_SIZE: usize = 4096;
// The most bytes one write to a stdio file accepts; the rest is dropped.
const MAX_STDIO_WRITE_SIZE: usize = 16 * 1024;

// Lengths are reported as `u32`, as files are limited to `u32::MAX` bytes.
fn len_u32(len: usize) -> u32 {
    u32::try_from(len).unwrap_or(u32::MAX)
}

impl File {
    fn from_kind(kind: FileKind) -> Self {
        Self(Rc::new(FileInner {
            kind,
            unique_id: OnceCell::new(),
            self_charge: RefCell::new(None),
        }))
    }

    fn writable_with(host: &dyn VfsHost, bytes: Vec<u8>) -> Self {
        let charge = host.charge_memory(bytes.len());
        Self::from_kind(FileKind::Data(RefCell::new(Data {
            contents: Contents::Writable { bytes, charge },
            last_modified: 0,
        })))
    }

    /// A new writable file of `size` zero bytes, or empty if `size` is `None`, capped at the blob
    /// size limit. Its contents and the file itself are charged to the isolate's memory limit.
    ///
    /// The file is not in any directory until it is added to one.
    pub fn new_writable(host: &dyn VfsHost, size: Option<u32>) -> Self {
        let size = (size.unwrap_or(0) as usize).min(host.blob_size_limit());
        let file = Self::writable_with(host, vec![0; size]);
        file.count_towards_isolate_limit(host);
        file
    }

    /// A new read-only file holding `bytes`, which are not charged to the isolate's memory limit.
    pub fn new_readable(bytes: Box<dyn ExternalBytes>) -> Self {
        Self::from_kind(FileKind::Data(RefCell::new(Data {
            contents: Contents::ReadOnly(bytes),
            last_modified: 0,
        })))
    }

    /// A new `/dev/null`.
    pub fn new_dev_null() -> Self {
        Self::from_kind(FileKind::Device(Device::Null))
    }

    /// A new `/dev/zero`.
    pub fn new_dev_zero() -> Self {
        Self::from_kind(FileKind::Device(Device::Zero))
    }

    /// A new `/dev/full`.
    pub fn new_dev_full() -> Self {
        Self::from_kind(FileKind::Device(Device::Full))
    }

    /// A new `/dev/random`.
    pub fn new_dev_random() -> Self {
        Self::from_kind(FileKind::Device(Device::Random))
    }

    pub(crate) fn new_stdio(stdio: Stdio) -> Self {
        Self::from_kind(FileKind::Stdio(StdioFile {
            stdio,
            state: RefCell::new(StdioState::default()),
        }))
    }

    /// Returns true if this and `other` are the same file.
    pub fn is_same_node(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }

    /// The file's metadata.
    pub fn stat(&self) -> Stat {
        let mut stat = Stat {
            fs_type: FsType::File,
            size: 0,
            last_modified: 0,
            created: 0,
            writable: true,
            device: false,
        };
        match &self.0.kind {
            FileKind::Data(data) => {
                let data = data.borrow();
                stat.size = len_u32(data.contents.bytes().len());
                stat.last_modified = data.last_modified;
                stat.writable = data.contents.is_writable();
            }
            FileKind::Device(_) => stat.device = true,
            FileKind::Stdio(_) => {}
        }
        stat
    }

    /// Reads into `buffer` from `offset` and returns the number of bytes read, 0 at end of file.
    pub fn read(&self, host: &dyn VfsHost, offset: u32, buffer: &mut [u8]) -> u32 {
        match &self.0.kind {
            FileKind::Data(data) => {
                let data = data.borrow();
                let bytes = data.contents.bytes();
                let Some(src) = bytes.get(offset as usize..) else {
                    return 0;
                };
                let n = src.len().min(buffer.len());
                buffer[..n].copy_from_slice(&src[..n]);
                len_u32(n)
            }
            FileKind::Device(Device::Null) | FileKind::Stdio(_) => 0,
            FileKind::Device(Device::Zero | Device::Full) => {
                buffer.fill(0);
                len_u32(buffer.len())
            }
            FileKind::Device(Device::Random) => {
                if host.fill_random(buffer) {
                    len_u32(buffer.len())
                } else {
                    0
                }
            }
        }
    }

    /// Writes `data` at `offset`, growing the file if needed, and returns the number of bytes
    /// written.
    ///
    /// `data` may view memory that JavaScript can change. A write that runs JavaScript, as a
    /// write to stdout or stderr does, copies `data` first.
    ///
    /// # Errors
    ///
    /// [`FsError::FileSizeLimitExceeded`] if the file would exceed the blob size limit,
    /// [`FsError::ReadOnly`] for a read-only file, and [`FsError::NotPermitted`] for `/dev/full`,
    /// `/dev/random` and stdin.
    pub fn write(&self, host: &dyn VfsHost, offset: u32, data: &[u8]) -> Result<u32, FsError> {
        match &self.0.kind {
            FileKind::Data(cell) => {
                let max = host.blob_size_limit();
                if data.len() > max {
                    return Err(FsError::FileSizeLimitExceeded);
                }
                let end = (offset as usize)
                    .checked_add(data.len())
                    .ok_or(FsError::FileSizeLimitExceeded)?;
                if end > max {
                    return Err(FsError::FileSizeLimitExceeded);
                }
                if !cell.borrow().contents.is_writable() {
                    return Err(FsError::ReadOnly);
                }
                if end > cell.borrow().contents.bytes().len() {
                    let end = u32::try_from(end).map_err(|_| FsError::FileSizeLimitExceeded)?;
                    self.resize(host, end)?;
                }
                let mut cell = cell.borrow_mut();
                let Contents::Writable { bytes, .. } = &mut cell.contents else {
                    return Err(FsError::ReadOnly);
                };
                bytes[offset as usize..end].copy_from_slice(data);
                Ok(len_u32(data.len()))
            }
            FileKind::Device(Device::Null | Device::Zero) => Ok(len_u32(data.len())),
            FileKind::Device(Device::Full | Device::Random) => Err(FsError::NotPermitted),
            FileKind::Stdio(stdio) => self.write_stdio(host, stdio, data),
        }
    }

    /// Replaces the contents of the file with `data`.
    ///
    /// # Errors
    ///
    /// The errors of [`resize`](Self::resize) and [`write`](Self::write).
    pub fn write_all(&self, host: &dyn VfsHost, data: &[u8]) -> Result<u32, FsError> {
        let size = u32::try_from(data.len()).map_err(|_| FsError::FileSizeLimitExceeded)?;
        self.resize(host, size)?;
        self.write(host, 0, data)
    }

    /// Sets every byte from `offset`, or from the start, to `value`.
    ///
    /// # Errors
    ///
    /// [`FsError::ReadOnly`] for a read-only file, and [`FsError::NotPermitted`] for
    /// `/dev/full`, `/dev/random` and the stdio files.
    pub fn fill(&self, value: u8, offset: Option<u32>) -> Result<(), FsError> {
        match &self.0.kind {
            FileKind::Data(cell) => {
                let mut cell = cell.borrow_mut();
                let Contents::Writable { bytes, .. } = &mut cell.contents else {
                    return Err(FsError::ReadOnly);
                };
                if let Some(tail) = bytes.get_mut(offset.unwrap_or(0) as usize..) {
                    tail.fill(value);
                }
                Ok(())
            }
            FileKind::Device(Device::Null | Device::Zero) => Ok(()),
            FileKind::Device(Device::Full | Device::Random) | FileKind::Stdio(_) => {
                Err(FsError::NotPermitted)
            }
        }
    }

    /// Truncates the file to `size` bytes, or extends it with zeros.
    ///
    /// # Errors
    ///
    /// [`FsError::ReadOnly`] for a read-only file, [`FsError::FileSizeLimitExceeded`] if `size`
    /// exceeds the blob size limit, and [`FsError::NotPermitted`] for `/dev/full`, `/dev/random`
    /// and the stdio files.
    pub fn resize(&self, host: &dyn VfsHost, size: u32) -> Result<(), FsError> {
        match &self.0.kind {
            FileKind::Data(cell) => {
                let mut cell = cell.borrow_mut();
                let Contents::Writable { bytes, charge } = &mut cell.contents else {
                    return Err(FsError::ReadOnly);
                };
                let size = size as usize;
                if size == bytes.len() {
                    return Ok(());
                }
                if size > host.blob_size_limit() {
                    return Err(FsError::FileSizeLimitExceeded);
                }
                bytes.resize(size, 0);
                bytes.shrink_to_fit();
                charge.set(size);
                Ok(())
            }
            FileKind::Device(Device::Null | Device::Zero) => Ok(()),
            FileKind::Device(Device::Full | Device::Random) | FileKind::Stdio(_) => {
                Err(FsError::NotPermitted)
            }
        }
    }

    /// Sets the last modification time, in nanoseconds since the Unix epoch. Does nothing for a
    /// read-only file, `/dev/null`, `/dev/zero` and `/dev/full`.
    ///
    /// # Errors
    ///
    /// [`FsError::NotPermitted`] for `/dev/random` and the stdio files.
    pub fn set_last_modified(&self, date: i64) -> Result<(), FsError> {
        match &self.0.kind {
            FileKind::Data(cell) => {
                let mut cell = cell.borrow_mut();
                if cell.contents.is_writable() {
                    cell.last_modified = date;
                }
                Ok(())
            }
            FileKind::Device(Device::Null | Device::Zero | Device::Full) => Ok(()),
            FileKind::Device(Device::Random) | FileKind::Stdio(_) => Err(FsError::NotPermitted),
        }
    }

    /// A writable copy of this file, whose contents are charged to the isolate's memory limit. A
    /// device or stdio file is not copied: the result is the same file.
    ///
    /// # Errors
    ///
    /// [`FsError::FileSizeLimitExceeded`] if the file is larger than the blob size limit.
    pub fn clone_file(&self, host: &dyn VfsHost) -> Result<Self, FsError> {
        match &self.0.kind {
            FileKind::Data(cell) => {
                let bytes = cell.borrow().contents.bytes().to_vec();
                if bytes.len() > host.blob_size_limit() {
                    return Err(FsError::FileSizeLimitExceeded);
                }
                Ok(Self::writable_with(host, bytes))
            }
            FileKind::Device(_) | FileKind::Stdio(_) => Ok(self.clone()),
        }
    }

    /// Replaces the contents and last modification time of this file with those of `other`.
    /// Does nothing for `/dev/null` and `/dev/zero`.
    ///
    /// # Errors
    ///
    /// [`FsError::ReadOnly`] for a read-only file, and [`FsError::NotPermitted`] for
    /// `/dev/full`, `/dev/random` and the stdio files.
    pub fn replace(&self, host: &dyn VfsHost, other: &Self) -> Result<(), FsError> {
        match &self.0.kind {
            FileKind::Data(cell) => {
                if !cell.borrow().contents.is_writable() {
                    return Err(FsError::ReadOnly);
                }
                // Read `other` before borrowing this file, which `other` may be.
                let stat = other.stat();
                let mut buffer = vec![0; stat.size as usize];
                other.read(host, 0, &mut buffer);
                let mut cell = cell.borrow_mut();
                let Contents::Writable { bytes, charge } = &mut cell.contents else {
                    return Err(FsError::ReadOnly);
                };
                charge.set(buffer.len());
                *bytes = buffer;
                cell.last_modified = stat.last_modified;
                Ok(())
            }
            FileKind::Device(Device::Null | Device::Zero) => Ok(()),
            FileKind::Device(Device::Full | Device::Random) | FileKind::Stdio(_) => {
                Err(FsError::NotPermitted)
            }
        }
    }

    /// A UUID that identifies this file, the same on every call, or `None` if one cannot be made
    /// because no request is current. NUL-terminated for C++, which keeps a pointer to it.
    pub fn unique_id(&self, host: &dyn VfsHost) -> Option<&CStr> {
        unique_id(&self.0.unique_id, host)
    }

    /// Charges the file itself to the isolate's memory limit, once. Only writable files are
    /// charged; the runtime's read-only files and devices are not.
    pub fn count_towards_isolate_limit(&self, host: &dyn VfsHost) {
        let FileKind::Data(cell) = &self.0.kind else {
            return;
        };
        if !cell.borrow().contents.is_writable() {
            return;
        }
        let mut self_charge = self.0.self_charge.borrow_mut();
        if self_charge.is_none() {
            *self_charge = Some(host.charge_memory(size_of::<FileInner>()));
        }
    }

    /// The name heap snapshots give this kind of file.
    pub fn memory_name(&self) -> &'static CStr {
        match &self.0.kind {
            FileKind::Data(_) => c"File",
            FileKind::Device(Device::Null) => c"/dev/null",
            FileKind::Device(Device::Zero) => c"/dev/zero",
            FileKind::Device(Device::Full) => c"/dev/full",
            FileKind::Device(Device::Random) => c"/dev/random",
            FileKind::Stdio(_) => c"stdio",
        }
    }

    /// The size of the file itself, without its contents.
    pub const fn memory_self_size(&self) -> usize {
        size_of::<FileInner>()
    }

    /// Reports the file to a heap snapshot as a node of its own, with an edge named `edge` from
    /// the current node.
    pub fn track_memory(&self, tracker: &mut dyn MemoryTracker, edge: &'static CStr) {
        track(tracker, edge, FileOrDirectory::File(self.clone()));
    }

    /// What identifies the file in a heap snapshot: the address of the state its handles share.
    pub(crate) fn memory_id(&self) -> usize {
        Rc::as_ptr(&self.0).addr()
    }

    /// The bytes of contents that heap snapshots attribute to this file: those of a writable
    /// file, and nothing for others.
    pub fn memory_tracked_size(&self) -> usize {
        match &self.0.kind {
            FileKind::Data(cell) => match &cell.borrow().contents {
                Contents::Writable { bytes, .. } => bytes.len(),
                Contents::ReadOnly(_) => 0,
            },
            FileKind::Device(_) | FileKind::Stdio(_) => 0,
        }
    }

    // Logs the complete lines of `data` and buffers the rest. `log_stdio()` can run JavaScript,
    // which can write to this file again or change the memory `data` views, so this copies
    // `data` first and holds no borrow of the state while logging.
    // A failure of `log_stdio()` ends the write with `FsError::Failed`.
    fn write_stdio(
        &self,
        host: &dyn VfsHost,
        file: &StdioFile,
        data: &[u8],
    ) -> Result<u32, FsError> {
        let data = &data[..data.len().min(MAX_STDIO_WRITE_SIZE)];
        if data.is_empty() || file.stdio == Stdio::In {
            return Ok(len_u32(data.len()));
        }
        let owned = data.to_vec();
        let mut rest = &owned[..];
        while !rest.is_empty() {
            if let Some(newline) = rest.iter().position(|&b| b == b'\n') {
                let (line, tail) = rest.split_at(newline + 1);
                let mut pending = std::mem::take(&mut file.state.borrow_mut().line_buffer);
                if pending.is_empty() {
                    log_line(host, file.stdio, line)?;
                } else {
                    pending.extend_from_slice(line);
                    log_line(host, file.stdio, &pending)?;
                }
                rest = tail;
            } else {
                append_to_line_buffer(&mut file.state.borrow_mut().line_buffer, rest);
                self.schedule_stdio_flush(host, file);
                break;
            }
        }
        Ok(len_u32(data.len()))
    }

    fn schedule_stdio_flush(&self, host: &dyn VfsHost, file: &StdioFile) {
        {
            let mut state = file.state.borrow_mut();
            if state.flush_scheduled {
                return;
            }
            state.flush_scheduled = true;
        }
        let weak: Weak<FileInner> = Rc::downgrade(&self.0);
        host.enqueue_microtask(Box::new(move |host| {
            let Some(inner) = weak.upgrade() else {
                return;
            };
            let FileKind::Stdio(file) = &inner.kind else {
                return;
            };
            let pending = {
                let mut state = file.state.borrow_mut();
                state.flush_scheduled = false;
                std::mem::take(&mut state.line_buffer)
            };
            if !pending.is_empty() && host.has_io_context() {
                // The host reports a failure itself when the microtask returns.
                let _ = log_line(host, file.stdio, &pending);
            }
        }));
    }
}

// Logs `line` without one trailing newline.
fn log_line(host: &dyn VfsHost, stdio: Stdio, line: &[u8]) -> Result<(), FsError> {
    host.log_stdio(stdio, line.strip_suffix(b"\n").unwrap_or(line))
        .map_err(|HostError| FsError::Failed)
}

// Appends `data` to `buffer`, keeping only the last `MAX_LINE_BUFFER_SIZE` bytes.
fn append_to_line_buffer(buffer: &mut Vec<u8>, data: &[u8]) {
    if buffer.len() + data.len() <= MAX_LINE_BUFFER_SIZE {
        buffer.extend_from_slice(data);
    } else if data.len() >= MAX_LINE_BUFFER_SIZE {
        buffer.clear();
        buffer.extend_from_slice(&data[data.len() - MAX_LINE_BUFFER_SIZE..]);
    } else {
        let excess = buffer.len() + data.len() - MAX_LINE_BUFFER_SIZE;
        buffer.drain(..excess);
        buffer.extend_from_slice(data);
    }
}

pub fn unique_id<'a>(cell: &'a OnceCell<CString>, host: &dyn VfsHost) -> Option<&'a CStr> {
    if cell.get().is_none() {
        let id = CString::new(host.random_uuid()?).ok()?;
        cell.set(id).ok()?;
    }
    cell.get().map(CString::as_c_str)
}

#[cfg(test)]
#[path = "file-test.rs"]
mod tests;
