// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The `disk` service: a directory served over HTTP.
//!
//! GET and HEAD read files (with one byte range) and list directories as JSON; PUT and DELETE,
//! on a writable directory, replace and remove entries. The service is also the directory a
//! worker's `durableObjectStorage.localDisk` names.
//!
//! Disk I/O is synchronous, as it is in `kj::Directory`: the files are local, and the loop
//! thread reads them in 64 KiB steps between writes to the response.

use std::io::Read;
use std::io::Seek;
use std::io::Write;
use std::path::Component;
use std::path::Path;
use std::path::PathBuf;
use std::pin::Pin;
use std::rc::Rc;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::time::SystemTime;

use httpdate::fmt_http_date;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeaderId;
use kj::http::HeaderTable;
use kj::http::Headers;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::Service;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_hyper::io_kj_error;
use kj_rs::KjOwn;
use percent_encoding::percent_decode_str;
use worker::AlarmResult;
use worker::Interface;
use worker::ScheduledResult;
use workerd_capnp::disk_directory;

use crate::Result;
use crate::channels::Channel;
use crate::channels::PendingToken;
use crate::channels::RequestMetadata;
use crate::channels::TokenUsage;
use crate::channels::WorkerInterface;
use crate::config::Factory;
use crate::config::text;
use crate::services::header_table;
use crate::services::not_transferable;
use crate::services::send_error;
use crate::services::unsupported;

// =======================================================================================
// Pure parts

/// Whether `segment` names one entry of a directory: a single normal path component, so nothing
/// a separator would split, no `.`, `..`, root or drive, and (on Windows) no stream of a file.
fn is_entry_name(segment: &str) -> bool {
    let mut components = Path::new(segment).components();
    matches!(components.next(), Some(Component::Normal(name)) if name == segment)
        && components.next().is_none()
        && !segment.contains('\0')
        && !(cfg!(windows) && segment.contains(':'))
}

/// The path a request URL names, as segments below the directory.
///
/// Dot segments, literal or percent-encoded, are resolved as the URL standard resolves them,
/// never above the root. `None` for a path with a segment that is not an entry's name, which
/// the service must not serve.
pub fn path_segments(url: &str) -> Option<Vec<String>> {
    let url = url::Url::parse(url).ok()?;
    let path = url.path();
    let path = path.strip_prefix('/').unwrap_or(path);
    let path = path.strip_suffix('/').unwrap_or(path);
    if path.is_empty() {
        return Some(Vec::new());
    }
    path.split('/')
        .map(|segment| {
            let segment = percent_decode_str(segment).decode_utf8().ok()?;
            is_entry_name(&segment).then(|| segment.into_owned())
        })
        .collect()
}

/// What a `Range` header asks for, against a body of a known length.
#[derive(Debug, PartialEq, Eq)]
pub enum Range {
    /// The header is malformed, or its range is beyond the body: 416.
    Unsatisfiable,
    /// The whole body, or several ranges, which get the whole body: send it as usual.
    Everything,
    /// One inclusive byte range within the body.
    Bytes(u64, u64),
}

pub fn parse_range(header: &str, length: u64) -> Range {
    let Some((unit, spec)) = header.split_once('=') else {
        return Range::Unsatisfiable;
    };
    if !unit.trim().eq_ignore_ascii_case("bytes") {
        return Range::Unsatisfiable;
    }
    if spec.contains(',') {
        return Range::Everything;
    }
    let bound = |text: &str| match text.trim() {
        "" => Ok(None),
        text => text.parse::<u64>().map(Some),
    };
    let Some((Ok(first), Ok(last))) = spec.split_once('-').map(|(a, b)| (bound(a), bound(b)))
    else {
        return Range::Unsatisfiable;
    };
    let end = length.wrapping_sub(1);
    let (start, end) = match (first, last) {
        (None, None) => return Range::Unsatisfiable,
        // A suffix range: the last `suffix` bytes, or everything when it asks for more.
        (None, Some(suffix)) => (length.saturating_sub(suffix), end),
        (Some(first), last) => (first, last.map_or(end, |last| last.min(end))),
    };
    if length == 0 || start > end {
        Range::Unsatisfiable
    } else if start == 0 && end == length - 1 {
        Range::Everything
    } else {
        Range::Bytes(start, end)
    }
}

/// A directory entry's type, as the listing names it.
fn entry_type(file_type: std::fs::FileType) -> &'static str {
    #[cfg(unix)]
    {
        use std::os::unix::fs::FileTypeExt;
        if file_type.is_block_device() {
            return "blockDevice";
        }
        if file_type.is_char_device() {
            return "characterDevice";
        }
        if file_type.is_fifo() {
            return "namedPipe";
        }
        if file_type.is_socket() {
            return "socket";
        }
    }
    if file_type.is_symlink() {
        "symlink"
    } else if file_type.is_dir() {
        "directory"
    } else if file_type.is_file() {
        "file"
    } else {
        "other"
    }
}

// =======================================================================================
// The service

struct Disk {
    factory: Rc<Factory>,
    root: PathBuf,
    writable: bool,
    allow_dotfiles: bool,
}

impl Disk {
    /// The path below the root the segments name; the segments passed validation.
    fn join(&self, segments: &[String]) -> PathBuf {
        segments
            .iter()
            .fold(self.root.clone(), |path, segment| path.join(segment))
    }
}

/// A `disk` service: a directory served over HTTP, and the directory a worker's
/// `durableObjectStorage.localDisk` names.
pub struct DiskDirectoryService(Rc<Disk>);

impl DiskDirectoryService {
    /// The directory's path when the service is writable, which Durable Object storage needs.
    #[must_use]
    pub fn writable_path(&self) -> Option<&str> {
        self.0.writable.then(|| self.0.root.to_str()).flatten()
    }
}

impl Channel for DiskDirectoryService {
    fn start_request(&self, _metadata: KjOwn<RequestMetadata>) -> Result<KjOwn<WorkerInterface>> {
        Ok(DiskRequest(Rc::clone(&self.0)).into_kj())
    }

    fn token(&self, _usage: TokenUsage) -> Result<KjOwn<PendingToken>> {
        Err(not_transferable("DiskDirectoryService"))
    }
}

/// `path_override` is the CLI's `--directory-path` for this service.
pub fn make_disk_directory_service(
    name: &str,
    conf: disk_directory::Reader<'_>,
    path_override: Option<&str>,
    factory: &Rc<Factory>,
) -> Result<Rc<DiskDirectoryService>> {
    let path = match path_override {
        Some(path) => path.to_owned(),
        None if conf.has_path() => text(conf.get_path())?,
        None => {
            return Err(kj::failed!(
                "Directory \"{name}\" has no path in the config, so must be specified on the \
                 command line with `--directory-path`."
            ));
        }
    };
    let root = std::env::current_dir()
        .map_err(|e| io_kj_error(&e))?
        .join(&path);
    if !root.is_dir() {
        return Err(kj::failed!("Directory named \"{name}\" not found: {path}"));
    }
    Ok(Rc::new(DiskDirectoryService(Rc::new(Disk {
        factory: Rc::clone(factory),
        root,
        writable: conf.get_writable(),
        allow_dotfiles: conf.get_allow_dotfiles(),
    }))))
}

// =======================================================================================
// Requests

fn kj_headers<'t>(
    table: &'t HeaderTable,
    map: &http::HeaderMap,
) -> Result<kj_hyper::ffi::Borrowing<'t, kj::http::ffi::HttpHeaders>> {
    kj_hyper::HeaderBlock::new(map, &http::Extensions::new()).to_kj(table)
}

fn header_value(value: impl AsRef<str>) -> Result<http::HeaderValue> {
    http::HeaderValue::from_str(value.as_ref())
        .map_err(|_| kj::failed!("invalid header value: {}", value.as_ref()))
}

fn len_u64(len: usize) -> u64 {
    u64::try_from(len).unwrap_or(u64::MAX)
}

/// Copies `length` bytes of the file from `start` into `out`.
async fn copy_file(
    path: &Path,
    start: u64,
    length: u64,
    out: &mut kj::io::AsyncOutputStream<'_>,
) -> Result<()> {
    let mut file = std::fs::File::open(path).map_err(|e| io_kj_error(&e))?;
    if start > 0 {
        file.seek(std::io::SeekFrom::Start(start))
            .map_err(|e| io_kj_error(&e))?;
    }
    let mut buffer = vec![0; 64 * 1024];
    let mut remaining = length;
    while remaining > 0 {
        let want = usize::try_from(remaining).map_or(buffer.len(), |r| r.min(buffer.len()));
        let read = file
            .read(&mut buffer[..want])
            .map_err(|e| io_kj_error(&e))?;
        if read == 0 {
            return Err(kj::disconnected!(
                "the file ended before its declared length"
            ));
        }
        out.write(&buffer[..read]).await?;
        remaining -= len_u64(read);
    }
    Ok(())
}

/// A file that is removed when dropped, unless it was renamed away first.
struct Temporary(PathBuf);

impl Drop for Temporary {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}

/// Writes the body to a temporary file beside `path`, then renames it into place, so that a
/// reader never sees a partial file. A write that fails or is abandoned leaves nothing behind.
async fn write_replacing(path: &Path, mut body: Pin<&mut AsyncInputStream>) -> Result<()> {
    static NONCE: AtomicU64 = AtomicU64::new(0);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| io_kj_error(&e))?;
    }
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned());
    let temp = Temporary(path.with_file_name(format!(
        ".{}.{}.{}.tmp",
        name.unwrap_or_default(),
        std::process::id(),
        NONCE.fetch_add(1, Ordering::Relaxed)
    )));
    let mut file = std::fs::File::create(&temp.0).map_err(|e| io_kj_error(&e))?;
    let mut buffer = vec![0; 64 * 1024];
    loop {
        let read = body.as_mut().try_read(&mut buffer, 1).await?;
        if read == 0 {
            break;
        }
        file.write_all(&buffer[..read])
            .map_err(|e| io_kj_error(&e))?;
    }
    drop(file);
    std::fs::rename(&temp.0, path).map_err(|e| io_kj_error(&e))
}

/// Removes a file, or a directory and everything in it; false when there was nothing there.
fn remove(path: &Path) -> Result<bool> {
    let metadata = match std::fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(false),
        Err(e) => return Err(io_kj_error(&e)),
    };
    if metadata.is_dir() {
        std::fs::remove_dir_all(path)
    } else {
        std::fs::remove_file(path)
    }
    .map_err(|e| io_kj_error(&e))?;
    Ok(true)
}

struct DiskRequest(Rc<Disk>);

impl DiskRequest {
    async fn file(
        &self,
        method: Method,
        path: &Path,
        metadata: &std::fs::Metadata,
        request_headers: HeadersRef<'_>,
        response: ServiceResponse<'_>,
    ) -> Result<()> {
        let table = header_table(&self.0.factory);
        let size = metadata.len();
        let mut headers = http::HeaderMap::new();
        headers.insert(
            http::header::CONTENT_TYPE,
            header_value("application/octet-stream")?,
        );
        if let Ok(modified) = metadata.modified() {
            headers.insert(
                http::header::LAST_MODIFIED,
                header_value(fmt_http_date(modified))?,
            );
        }
        // Partial content for a single satisfiable range; several ranges get everything.
        let mut range = None;
        if method == Method::GET
            && let Some(header) = request_headers.get(HeaderId::RANGE)
        {
            match parse_range(&String::from_utf8_lossy(header), size) {
                Range::Unsatisfiable => {
                    let mut headers = http::HeaderMap::new();
                    headers.insert(
                        http::header::CONTENT_RANGE,
                        header_value(format!("bytes */{size}"))?,
                    );
                    let headers = kj_headers(table, &headers)?;
                    return send_error(response, 416, "Range Not Satisfiable", &*headers).await;
                }
                Range::Bytes(start, end) => range = Some((start, end)),
                Range::Everything => {}
            }
        }
        // The header is set explicitly so that a worker calling this service in-process, with
        // no HTTP connection in between, sees a `Content-Length` too.
        let (status, text, start, length) = match range {
            Some((start, end)) => {
                let length = end - start + 1;
                headers.insert(
                    http::header::CONTENT_RANGE,
                    header_value(format!("bytes {start}-{end}/{size}"))?,
                );
                (206, "Partial Content", start, length)
            }
            None => (200, "OK", 0, size),
        };
        headers.insert(
            http::header::CONTENT_LENGTH,
            header_value(length.to_string())?,
        );
        let headers = kj_headers(table, &headers)?;
        let mut out = response.send(status, text, HeadersRef::from(&*headers), Some(length))?;
        if method == Method::HEAD {
            return Ok(());
        }
        copy_file(path, start, length, &mut out).await
    }

    async fn directory(
        &self,
        method: Method,
        path: &Path,
        metadata: &std::fs::Metadata,
        response: ServiceResponse<'_>,
    ) -> Result<()> {
        let table = header_table(&self.0.factory);
        let mut headers = http::HeaderMap::new();
        headers.insert(
            http::header::CONTENT_TYPE,
            header_value("application/json")?,
        );
        if let Ok(modified) = metadata.modified() {
            headers.insert(
                http::header::LAST_MODIFIED,
                header_value(fmt_http_date(modified))?,
            );
        }
        let headers = kj_headers(table, &headers)?;
        // No size: the listing may become a stream some day.
        let mut out = response.send(200, "OK", HeadersRef::from(&*headers), None)?;
        if method == Method::HEAD {
            return Ok(());
        }
        let mut entries = Vec::new();
        for entry in std::fs::read_dir(path).map_err(|e| io_kj_error(&e))? {
            let entry = entry.map_err(|e| io_kj_error(&e))?;
            let name = entry.file_name().to_string_lossy().into_owned();
            if !self.0.allow_dotfiles && name.starts_with('.') {
                continue;
            }
            let kind = entry.file_type().map_or("other", entry_type);
            entries.push((name, kind));
        }
        entries.sort();
        let listing = entries
            .iter()
            .map(|(name, kind)| serde_json::json!({"name": name, "type": kind}))
            .collect::<Vec<_>>();
        let json = serde_json::to_string(&listing).map_err(|e| kj::failed!("{e}"))?;
        out.write(json.as_bytes()).await
    }
}

#[async_trait::async_trait(?Send)]
impl Service for DiskRequest {
    async fn request<'a>(
        &'a mut self,
        method: Method,
        url: &'a [u8],
        headers: HeadersRef<'a>,
        request_body: Pin<&'a mut AsyncInputStream>,
        response: ServiceResponse<'a>,
    ) -> Result<()> {
        let disk = &self.0;
        let table = header_table(&disk.factory);
        let none = Headers::new(table);
        let url = std::str::from_utf8(url).map_err(|_| kj::failed!("request URL is not UTF-8"))?;
        let segments = path_segments(url).filter(|segments| {
            disk.allow_dotfiles || !segments.iter().any(|segment| segment.starts_with('.'))
        });
        match method {
            Method::GET | Method::HEAD => {
                let Some(segments) = segments else {
                    return send_error(response, 404, "Not Found", &none).await;
                };
                let path = disk.join(&segments);
                let Ok(metadata) = std::fs::metadata(&path) else {
                    return send_error(response, 404, "Not Found", &none).await;
                };
                if metadata.is_file() {
                    self.file(method, &path, &metadata, headers, response).await
                } else if metadata.is_dir() {
                    self.directory(method, &path, &metadata, response).await
                } else {
                    send_error(response, 406, "Not Acceptable", &none).await
                }
            }
            Method::PUT | Method::DELETE => {
                if !disk.writable {
                    return send_error(response, 405, "Method Not Allowed", &none).await;
                }
                let Some(segments) = segments.filter(|segments| !segments.is_empty()) else {
                    return send_error(response, 403, "Unauthorized", &none).await;
                };
                let path = disk.join(&segments);
                if method == Method::PUT {
                    write_replacing(&path, request_body).await?;
                } else if !remove(&path)? {
                    return send_error(response, 404, "Not Found", &none).await;
                }
                response.send(204, "No Content", &none, None)?;
                Ok(())
            }
            _ => send_error(response, 501, "Not Implemented", &none).await,
        }
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> Result<()> {
        Err(unsupported("Disk directory services"))
    }
}

#[async_trait::async_trait(?Send)]
impl Interface for DiskRequest {
    async fn run_scheduled(&mut self, _time: &SystemTime, _cron: &str) -> Result<ScheduledResult> {
        Err(unsupported("Disk directory services"))
    }

    async fn run_alarm(&mut self, _time: &SystemTime, _retry_count: u32) -> Result<AlarmResult> {
        Err(unsupported("Disk directory services"))
    }
}

#[cfg(test)]
#[path = "disk-test.rs"]
mod tests;
