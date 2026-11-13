#![allow(
    clippy::items_after_statements,
    clippy::uninlined_format_args,
    clippy::unused_async
)]

use std::fmt::Write as _;
use std::hint::black_box;
use std::panic;

use cxx::CxxString;
use cxx::let_cxx_string;

#[test]
fn test_async_cxx_string() {
    async fn f() {
        let_cxx_string!(s = "...");

        async fn g(_: &CxxString) {}
        g(&s).await;
    }

    // https://github.com/dtolnay/cxx/issues/693
    fn assert_send(_: impl Send) {}
    assert_send(f());
}

#[test]
fn test_display() {
    let_cxx_string!(s = b"w\"x\'y\xF1\x80\xF1\x80z");

    assert_eq!(format!("{}", s), "w\"x'y\u{fffd}\u{fffd}z");
}

#[test]
fn test_debug() {
    let_cxx_string!(s = b"w\"x\'y\xF1\x80z");

    assert_eq!(format!("{:?}", s), r#""w\"x'y\xf1\x80z""#);
}

#[test]
fn test_fmt_write() {
    let_cxx_string!(s = "");

    let name = "world";
    write!(s, "Hello, {name}!").unwrap();
    assert_eq!(s.to_str(), Ok("Hello, world!"));
}

#[test]
fn test_io_write() {
    let_cxx_string!(s = "");
    let mut reader: &[u8] = b"Hello, world!";

    std::io::copy(&mut reader, &mut s).unwrap();
    assert_eq!(s.to_str(), Ok("Hello, world!"));
}

// Leaves bytes that read as a heap-allocated string in the stack memory that the next call's
// frames occupy, so that destroying a string there that was never constructed would fail.
#[inline(never)]
fn dirty_stack() {
    let mut region = [0xff_u8; 4096];
    black_box(&mut region);
}

#[test]
fn test_initializer_returns_early() {
    #[inline(never)]
    fn len(value: Option<&str>) -> Option<usize> {
        let_cxx_string!(s = value?);
        Some(s.len())
    }

    dirty_stack();
    assert_eq!(len(None), None);
    assert_eq!(len(Some("example")), Some(7));
}

#[test]
fn test_initializer_panics() {
    #[inline(never)]
    fn len(value: Option<&[u8]>) -> usize {
        let_cxx_string!(s = value.unwrap());
        s.len()
    }

    dirty_stack();
    assert!(panic::catch_unwind(|| len(None)).is_err());
}
