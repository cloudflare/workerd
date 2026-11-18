// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::rc::Rc;

use super::*;
use crate::tests::TestHost;

fn contents(host: &TestHost, file: &File) -> Vec<u8> {
    let mut buffer = vec![0; file.stat().size as usize];
    assert_eq!(file.read(host, 0, &mut buffer) as usize, buffer.len());
    buffer
}

fn readable(bytes: &'static [u8]) -> File {
    File::new_readable(Box::new(bytes))
}

#[test]
fn new_writable_is_zeroed_capped_and_charged() {
    let host = TestHost::new();
    host.blob_size_limit.set(16);
    let file = File::new_writable(&host, Some(100));
    let stat = file.stat();
    assert_eq!(stat.size, 16);
    assert!(stat.writable);
    assert!(!stat.device);
    assert_eq!(contents(&host, &file), [0; 16]);
    assert_eq!(host.charged.get(), 16 + file.memory_self_size());
    drop(file);
    assert_eq!(host.charged.get(), 0);
}

#[test]
fn write_grows_the_file_and_its_charge() {
    let host = TestHost::new();
    let file = File::new_writable(&host, None);
    let self_size = file.memory_self_size();
    assert_eq!(file.write(&host, 2, b"hi"), Ok(2));
    assert_eq!(contents(&host, &file), b"\0\0hi");
    assert_eq!(host.charged.get(), 4 + self_size);
    assert_eq!(file.write(&host, 0, b"x"), Ok(1));
    assert_eq!(contents(&host, &file), b"x\0hi");
    assert_eq!(file.memory_tracked_size(), 4);
}

#[test]
fn write_checks_the_limit_before_writability() {
    let host = TestHost::new();
    host.blob_size_limit.set(4);
    let file = File::new_writable(&host, None);
    assert_eq!(
        file.write(&host, 0, b"12345"),
        Err(FsError::FileSizeLimitExceeded)
    );
    assert_eq!(
        file.write(&host, 3, b"12"),
        Err(FsError::FileSizeLimitExceeded)
    );
    assert_eq!(
        file.write(&host, u32::MAX, b"1"),
        Err(FsError::FileSizeLimitExceeded)
    );

    let read_only = readable(b"abc");
    assert_eq!(
        read_only.write(&host, 0, b"12345"),
        Err(FsError::FileSizeLimitExceeded)
    );
    assert_eq!(read_only.write(&host, 0, b"1"), Err(FsError::ReadOnly));
}

#[test]
fn read_from_an_offset() {
    let host = TestHost::new();
    let file = readable(b"hello");
    let mut buffer = [0; 3];
    assert_eq!(file.read(&host, 1, &mut buffer), 3);
    assert_eq!(&buffer, b"ell");
    assert_eq!(file.read(&host, 3, &mut buffer), 2);
    assert_eq!(&buffer[..2], b"lo");
    assert_eq!(file.read(&host, 5, &mut buffer), 0);
    assert_eq!(file.read(&host, 50, &mut buffer), 0);
    assert_eq!(file.read(&host, 0, &mut []), 0);
}

#[test]
fn read_only_files() {
    let host = TestHost::new();
    let file = readable(b"abc");
    let stat = file.stat();
    assert_eq!(stat.size, 3);
    assert!(!stat.writable);
    assert_eq!(file.resize(&host, 1), Err(FsError::ReadOnly));
    assert_eq!(file.fill(1, None), Err(FsError::ReadOnly));
    assert_eq!(file.replace(&host, &readable(b"x")), Err(FsError::ReadOnly));
    // Setting the time of a read-only file does nothing, and succeeds.
    assert_eq!(file.set_last_modified(5), Ok(()));
    assert_eq!(file.stat().last_modified, 0);
    file.count_towards_isolate_limit(&host);
    assert_eq!(host.charged.get(), 0);
    assert_eq!(file.memory_tracked_size(), 0);
}

#[test]
fn resize_truncates_and_extends_with_zeros() {
    let host = TestHost::new();
    host.blob_size_limit.set(8);
    let file = File::new_writable(&host, None);
    file.write_all(&host, b"abcdef").unwrap();
    file.resize(&host, 3).unwrap();
    assert_eq!(contents(&host, &file), b"abc");
    file.resize(&host, 5).unwrap();
    assert_eq!(contents(&host, &file), b"abc\0\0");
    assert_eq!(file.resize(&host, 9), Err(FsError::FileSizeLimitExceeded));
    assert_eq!(host.charged.get(), 5 + file.memory_self_size());
}

#[test]
fn fill_from_an_offset() {
    let host = TestHost::new();
    let file = File::new_writable(&host, Some(4));
    file.fill(7, Some(2)).unwrap();
    assert_eq!(contents(&host, &file), [0, 0, 7, 7]);
    file.fill(1, None).unwrap();
    assert_eq!(contents(&host, &file), [1; 4]);
    // An offset past the end changes nothing.
    file.fill(2, Some(4)).unwrap();
    file.fill(2, Some(u32::MAX)).unwrap();
    assert_eq!(contents(&host, &file), [1; 4]);
}

#[test]
fn clone_copies_into_a_writable_file() {
    let host = TestHost::new();
    let original = readable(b"abc");
    let copy = original.clone_file(&host).unwrap();
    assert!(!copy.is_same_node(&original));
    assert!(copy.stat().writable);
    copy.write(&host, 0, b"x").unwrap();
    assert_eq!(contents(&host, &copy), b"xbc");
    assert_eq!(contents(&host, &original), b"abc");
    // A copy's contents are charged, but not the copy itself.
    assert_eq!(host.charged.get(), 3);

    host.blob_size_limit.set(2);
    assert!(matches!(
        original.clone_file(&host),
        Err(FsError::FileSizeLimitExceeded)
    ));
}

#[test]
fn replace_takes_contents_and_time() {
    let host = TestHost::new();
    let source = File::new_writable(&host, None);
    source.write_all(&host, b"source").unwrap();
    source.set_last_modified(42).unwrap();
    let target = File::new_writable(&host, Some(100));
    target.replace(&host, &source).unwrap();
    assert_eq!(contents(&host, &target), b"source");
    assert_eq!(target.stat().last_modified, 42);
    assert_eq!(host.charged.get(), 12 + 2 * target.memory_self_size());

    // Replacing a file with itself keeps its contents.
    target.replace(&host, &target).unwrap();
    assert_eq!(contents(&host, &target), b"source");
}

#[test]
fn devices() {
    let host = TestHost::new();
    let mut buffer = [5; 4];
    for file in [
        File::new_dev_null(),
        File::new_dev_zero(),
        File::new_dev_full(),
        File::new_dev_random(),
    ] {
        let stat = file.stat();
        assert!(stat.device && stat.writable);
        assert_eq!(stat.size, 0);
        // Copying a device yields the device itself.
        assert!(file.clone_file(&host).unwrap().is_same_node(&file));
    }

    let null = File::new_dev_null();
    assert_eq!(null.read(&host, 0, &mut buffer), 0);
    assert_eq!(null.write(&host, 0, b"abc"), Ok(3));
    assert_eq!(null.resize(&host, 10), Ok(()));
    assert_eq!(null.fill(1, None), Ok(()));
    assert_eq!(null.replace(&host, &readable(b"x")), Ok(()));
    assert_eq!(null.set_last_modified(1), Ok(()));

    let zero = File::new_dev_zero();
    assert_eq!(zero.read(&host, 0, &mut buffer), 4);
    assert_eq!(buffer, [0; 4]);
    assert_eq!(zero.write(&host, 0, b"abc"), Ok(3));

    let full = File::new_dev_full();
    buffer = [5; 4];
    assert_eq!(full.read(&host, 0, &mut buffer), 4);
    assert_eq!(buffer, [0; 4]);
    assert_eq!(full.write(&host, 0, b"abc"), Err(FsError::NotPermitted));
    assert_eq!(full.resize(&host, 1), Err(FsError::NotPermitted));
    assert_eq!(full.fill(1, None), Err(FsError::NotPermitted));
    assert_eq!(full.replace(&host, &null), Err(FsError::NotPermitted));
    assert_eq!(full.set_last_modified(1), Ok(()));

    let random = File::new_dev_random();
    assert_eq!(random.read(&host, 0, &mut buffer), 4);
    assert_eq!(buffer, [9; 4]);
    assert_eq!(random.write(&host, 0, b"abc"), Err(FsError::NotPermitted));
    assert_eq!(random.set_last_modified(1), Err(FsError::NotPermitted));
    // Outside a request there is no randomness, and a read is at end of file.
    assert_eq!(random.read(&TestHost::outside_request(), 0, &mut buffer), 0);
}

#[test]
fn unique_ids_are_stable_and_need_a_request() {
    let outside = TestHost::outside_request();
    let file = File::new_dev_null();
    assert_eq!(file.unique_id(&outside), None);
    let host = TestHost::new();
    let id = file.unique_id(&host).unwrap();
    assert_eq!(file.unique_id(&host).unwrap(), id);
    // Once made, the identifier is available outside a request too.
    assert_eq!(file.unique_id(&outside).unwrap(), id);
    assert_ne!(File::new_dev_null().unique_id(&host).unwrap(), id);
}

fn stdout() -> File {
    File::new_stdio(Stdio::Out)
}

#[test]
fn stdio_logs_complete_lines() {
    let host = TestHost::new();
    let out = stdout();
    let stat = out.stat();
    assert!(stat.writable && !stat.device);
    assert_eq!(out.write(&host, 0, b"one\ntwo\n\nthree"), Ok(14));
    assert_eq!(
        host.logged_lines(),
        [
            (Stdio::Out, "one".to_owned()),
            (Stdio::Out, "two".to_owned()),
            (Stdio::Out, String::new())
        ]
    );
    // The unfinished line joins the next write's first line.
    out.write(&host, 0, b" four\n").unwrap();
    assert_eq!(
        host.logged_lines()[3],
        (Stdio::Out, "three four".to_owned())
    );
    let mut buffer = [0; 4];
    assert_eq!(out.read(&host, 0, &mut buffer), 0);
}

#[test]
fn stdio_stops_when_logging_fails() {
    let host = TestHost::new();
    let out = stdout();
    host.fail_log_after.set(Some(1));
    assert_eq!(
        out.write(&host, 0, b"one\ntwo\nthree\n"),
        Err(FsError::Failed)
    );
    assert_eq!(host.logged_lines(), [(Stdio::Out, "one".to_owned())]);
    // The file stays usable once logging works again.
    host.fail_log_after.set(None);
    assert_eq!(out.write(&host, 0, b"four\n"), Ok(5));
    assert_eq!(host.logged_lines()[1], (Stdio::Out, "four".to_owned()));
}

#[test]
fn stdio_flushes_an_unfinished_line_in_a_microtask() {
    let host = TestHost::new();
    let err = File::new_stdio(Stdio::Err);
    err.write(&host, 0, b"a").unwrap();
    err.write(&host, 0, b"b").unwrap();
    // One flush is scheduled for any number of writes.
    assert_eq!(host.pending_microtasks(), 1);
    assert!(host.logged_lines().is_empty());
    host.run_microtasks();
    assert_eq!(host.logged_lines(), [(Stdio::Err, "ab".to_owned())]);

    // A flush outside a request discards the line.
    err.write(&host, 0, b"lost").unwrap();
    host.in_request.set(false);
    host.run_microtasks();
    host.in_request.set(true);
    assert_eq!(host.logged_lines().len(), 1);
    err.write(&host, 0, b"x\n").unwrap();
    assert_eq!(host.logged_lines()[1], (Stdio::Err, "x".to_owned()));

    // A flush after the file is gone does nothing.
    err.write(&host, 0, b"gone").unwrap();
    drop(err);
    host.run_microtasks();
    assert_eq!(host.logged_lines().len(), 2);
}

#[test]
fn stdio_keeps_the_end_of_a_long_line() {
    let host = TestHost::new();
    let out = stdout();
    out.write(&host, 0, &[b'a'; 4000]).unwrap();
    out.write(&host, 0, &[b'b'; 200]).unwrap();
    out.write(&host, 0, b"\n").unwrap();
    let line = host.logged.borrow()[0].1.clone();
    assert_eq!(line.len(), MAX_LINE_BUFFER_SIZE);
    assert!(line.ends_with(&[b'b'; 200]));
    assert!(line.starts_with(&[b'a'; 3896]));

    out.write(&host, 0, &[b'c'; 5000]).unwrap();
    out.write(&host, 0, b"\n").unwrap();
    assert_eq!(host.logged.borrow()[1].1, [b'c'; MAX_LINE_BUFFER_SIZE]);
}

#[test]
fn stdio_truncates_large_writes() {
    let host = TestHost::new();
    let data = vec![b'x'; MAX_STDIO_WRITE_SIZE + 10];
    assert_eq!(
        stdout().write(&host, 0, &data),
        Ok(u32::try_from(MAX_STDIO_WRITE_SIZE).unwrap())
    );
    let stdin = File::new_stdio(Stdio::In);
    assert_eq!(stdin.write(&host, 0, b"ignored\n"), Ok(8));
    assert!(host.logged_lines().is_empty());
    assert_eq!(stdin.resize(&host, 0), Err(FsError::NotPermitted));
    assert_eq!(stdin.set_last_modified(1), Err(FsError::NotPermitted));
}

#[test]
fn stdio_allows_writes_from_the_logger() {
    let host = TestHost::new();
    let out = Rc::new(stdout());
    let reentered = out.clone();
    // The first line logged writes another line, as a console.log getter could.
    *host.on_log.borrow_mut() = Some(Box::new(move |host| {
        if host.logged.borrow().len() == 1 {
            reentered.write(host, 0, b"nested\npartial").unwrap();
        }
    }));
    out.write(&host, 0, b"first\nsecond\n").unwrap();
    assert_eq!(
        host.logged_lines(),
        [
            (Stdio::Out, "first".to_owned()),
            (Stdio::Out, "nested".to_owned()),
            // The nested write's partial line was buffered, and the outer write's next line
            // completes it.
            (Stdio::Out, "partialsecond".to_owned()),
        ]
    );
    *host.on_log.borrow_mut() = None;
}
