//! The HTTP/1.1 text codec: byte-exact, wholly-safe Rust ports of kj-http's HttpHeaders
//! serialization, in-place header parsing, request-URL/status-text validation, and
//! Range-header parsing (kj/compat/http.c++). Under `--//:io_backend=rust`
//! the C++ codec TU (kj-http-impl) is not linked; workerd's kj-http shim
//! (src/workerd/util/kj-http-tokio.c++) defines the evicted `kj::HttpHeaders` members and free
//! functions over these ports instead. Every output byte, every in-place buffer mutation, and
//! every accept/reject decision matches kj's implementation exactly — kj's own tests (and
//! workerd's, which assert exact wire bytes) are the spec.
//!
//! This module is wholly-safe (no `unsafe`, per the crate-root `#![deny(unsafe_code)]`); the
//! bridge declarations live in the http.rs FFI island.

use crate::http::ffi::HttpHeaderParseResult;
use crate::http::ffi::HttpHeaderSpan;
use crate::http::ffi::HttpRangeSpan;
use crate::http::ffi::HttpRangesResult;

// =======================================================================================
// Serialization (kj::HttpHeaders::serialize())

/// Incremental serializer for one HTTP/1.1 message head (kj::HttpHeaders::serialize()'s output
/// format): construction emits the start line (unless empty — kj's `serialize(nullptr, ...)`
/// toString() form), [`add_header`](Self::add_header) emits one `Name: value\r\n` field line,
/// and [`finish`](Self::finish) appends the final blank line and yields the bytes. The C++ side
/// (the shim's `HttpHeaders::serialize`) drives it with kj's exact flattening order:
/// connection-header overrides, indexed headers in table order, then unindexed headers.
pub struct HttpSerializer {
    buf: Vec<u8>,
}

/// Create a serializer. If `word1` is non-empty, emits the start line `word1 word2 word3\r\n`
/// (kj emits it iff word1 is non-null; all callers pass either three real words or three
/// nulls). Boxed because opaque Rust types cross the cxx bridge by `Box`.
pub fn new_http_serializer(word1: &[u8], word2: &[u8], word3: &[u8]) -> Box<HttpSerializer> {
    let mut buf = Vec::new();
    if !word1.is_empty() {
        buf.extend_from_slice(word1);
        buf.push(b' ');
        buf.extend_from_slice(word2);
        buf.push(b' ');
        buf.extend_from_slice(word3);
        buf.extend_from_slice(b"\r\n");
    }
    Box::new(HttpSerializer { buf })
}

impl HttpSerializer {
    /// Append one `name: value\r\n` field line.
    pub fn add_header(&mut self, name: &[u8], value: &[u8]) {
        self.buf.extend_from_slice(name);
        self.buf.extend_from_slice(b": ");
        self.buf.extend_from_slice(value);
        self.buf.extend_from_slice(b"\r\n");
    }

    /// Append the final `\r\n` and take the serialized bytes.
    pub fn finish(&mut self) -> Vec<u8> {
        let mut buf = std::mem::take(&mut self.buf);
        buf.extend_from_slice(b"\r\n");
        buf
    }
}

// =======================================================================================
// Start-line validation (the `isValidRequestUrl` / `isValidStatusText` statics)

/// kj's `isValidRequestUrl`: the request-target (URL) appears in the request line as
/// `METHOD SP request-target SP version`. It must not contain whitespace (which would introduce
/// extra tokens into the request line) nor any control characters (in particular CR or LF,
/// which would allow injecting additional headers or entire requests). Rejects any byte
/// <= 0x20 (space, tab, CR, LF, NUL) as well as 0x7f (DEL); bytes >= 0x80 are permitted since
/// some callers pass pre-encoded or non-ASCII targets, which cannot cause desync.
pub fn is_valid_request_url(url: &[u8]) -> bool {
    url.iter().all(|&c| c > 0x20 && c != 0x7f)
}

/// kj's `isValidStatusText`: the status text (reason-phrase) appears at the end of the response
/// status line. It must not contain CR, LF, or NUL, which would allow injecting additional
/// headers or corrupt the framing.
pub fn is_valid_status_text(text: &[u8]) -> bool {
    !text.iter().any(|&c| c == 0 || c == b'\r' || c == b'\n')
}

// =======================================================================================
// Header parsing (kj::HttpHeaders::tryParse() / parseHeaders(), http.c++'s
// trimHeaderEnding/consumeHeaderName/consumeLine)
//
// kj parses headers IN PLACE: the parsed name/value StringPtrs borrow the request buffer, with
// a '\0' written over each field's terminator so they are NUL-terminated in-buffer. That
// mutation is part of the contract here too — the returned spans index into the (mutated)
// buffer, and the C++ caller forms kj::StringPtrs over them, exactly as kj's parseHeaders()
// hands addNoCheck() in-buffer strings.

/// An HTTP token char per kj's HTTP_TOKEN_CHARS (RFC2616 section 2.2): everything except
/// control chars (0x00-0x1f), space, DEL (0x7f), and the separators. Bytes >= 0x80 count as
/// token chars, matching kj's 256-bit CharGroup table.
fn is_token_char(c: u8) -> bool {
    !matches!(
        c,
        0..=0x20
            | 0x7f
            | b'('
            | b')'
            | b'<'
            | b'>'
            | b'@'
            | b','
            | b';'
            | b':'
            | b'\\'
            | b'"'
            | b'/'
            | b'['
            | b']'
            | b'?'
            | b'='
            | b'{'
            | b'}'
    )
}

/// kj's `skipSpace` over a bounded buffer: advance past ' ' and '\t'.
fn skip_space(buffer: &[u8], mut pos: usize) -> usize {
    while let Some(&c) = buffer.get(pos) {
        if c == b' ' || c == b'\t' {
            pos += 1;
        } else {
            break;
        }
    }
    pos
}

fn parse_failed() -> HttpHeaderParseResult {
    HttpHeaderParseResult {
        ok: false,
        spans: Vec::new(),
    }
}

/// One consumed header-value line (kj's `consumeLine`): the value span, the resume position,
/// and whether obsolete line folding was seen.
struct Line {
    value_begin: usize,
    value_end: usize,
    next: usize,
    saw_folding: bool,
}

/// kj's `consumeLine`: skip leading spaces/tabs, then scan to the line ending ('\0', "\r\n", or
/// "\n"), writing '\0' over the terminator. An obsolete "line folding" continuation (the line
/// ending followed by a space or tab) keeps scanning with the ending bytes REPLACED BY SPACES —
/// kj performs that mutation before its caller checks the folding flag and rejects, so the
/// buffer bytes stay identical either way. Returns `None` only if the scan runs off the buffer
/// (impossible for the '\0'-sentinel-terminated buffers the entry points construct); the caller
/// maps that to a failed parse.
fn consume_line(buffer: &mut [u8], pos: usize) -> Option<Line> {
    let start = skip_space(buffer, pos);
    let mut p = start;
    let mut saw_folding = false;

    loop {
        match *buffer.get(p)? {
            0 => {
                return Some(Line {
                    value_begin: start,
                    value_end: p,
                    next: p,
                    saw_folding,
                });
            }
            b'\r' => {
                let end = p;
                p += 1;
                if buffer.get(p) == Some(&b'\n') {
                    p += 1;
                }

                if matches!(*buffer.get(p)?, b' ' | b'\t') {
                    // Whoa, continuation line. These are deprecated, but historically a line
                    // starting with a space was treated as a continuation of the previous line.
                    // The behavior should be the same as if the \r\n were replaced with spaces,
                    // so let's do that here to prevent confusion later.
                    saw_folding = true;
                    buffer[end] = b' ';
                    buffer[p - 1] = b' ';
                } else {
                    buffer[end] = 0;
                    return Some(Line {
                        value_begin: start,
                        value_end: end,
                        next: p,
                        saw_folding,
                    });
                }
            }
            b'\n' => {
                let end = p;
                p += 1;

                if matches!(*buffer.get(p)?, b' ' | b'\t') {
                    // Whoa, continuation line (see above): behave as if the \n were replaced
                    // with a space.
                    saw_folding = true;
                    buffer[end] = b' ';
                } else {
                    buffer[end] = 0;
                    return Some(Line {
                        value_begin: start,
                        value_end: end,
                        next: p,
                        saw_folding,
                    });
                }
            }
            _ => p += 1,
        }
    }
}

/// kj's `parseHeaders(ptr, end)` loop over the trimmed region `[0, end_pos]` of `buffer`, where
/// `buffer[end_pos]` is the '\0' sentinel `trimHeaderEnding` wrote. Loops consuming one header
/// name (kj's `consumeHeaderName`: token chars, nonempty, immediately followed by ':' — no
/// whitespace before the colon, per RFC 9112 section 5.1's smuggling defense — with '\0'
/// written over the ':') and one value line per iteration; succeeds iff it stops exactly at the
/// sentinel. Obsolete line folding rejects the whole parse (RFC 9112 section 7.1.4 lets a
/// server reject with 400; folding is never used legitimately and has historically been a
/// source of HTTP desync) — after `consume_line` has already applied kj's fold mutations.
fn parse_trimmed_region(buffer: &mut [u8], end_pos: usize) -> HttpHeaderParseResult {
    let mut spans = Vec::new();
    let mut pos = 0;

    loop {
        match buffer.get(pos) {
            None => return parse_failed(),
            Some(0) => break,
            Some(_) => {}
        }

        // consumeHeaderName. NOTE: no leading-space skip — leading whitespace indicates a
        // continuation line, handled (and rejected) via consume_line's folding flag.
        let name_begin = pos;
        while pos < buffer.len() && is_token_char(buffer[pos]) {
            pos += 1;
        }
        let name_end = pos;
        if name_end == name_begin || buffer.get(pos) != Some(&b':') {
            return parse_failed();
        }
        buffer[name_end] = 0; // NUL-terminate the name in place (overwrites the ':').
        pos = skip_space(buffer, pos + 1);

        let Some(line) = consume_line(buffer, pos) else {
            return parse_failed();
        };
        if line.saw_folding {
            return parse_failed();
        }
        spans.push(HttpHeaderSpan {
            name_begin,
            name_end,
            value_begin: line.value_begin,
            value_end: line.value_end,
        });
        pos = line.next;
    }

    HttpHeaderParseResult {
        ok: pos == end_pos,
        spans,
    }
}

/// kj's `HttpHeaders::tryParse`: trim the trailing "\r\n" (or "\n") off the header blob —
/// kj's `trimHeaderEnding`, writing the '\0' sentinel over the new end — then parse the trimmed
/// region in place. On `ok`, each span is one header's name/value byte ranges, NUL-terminated
/// in-buffer.
pub fn parse_http_headers(buffer: &mut [u8]) -> HttpHeaderParseResult {
    // trimHeaderEnding: remove the trailing \r\n and replace with the \0 sentinel.
    let len = buffer.len();
    if len < 2 {
        return parse_failed();
    }
    if buffer[len - 1] != b'\n' {
        return parse_failed();
    }
    let mut end = len - 1;
    if buffer[end - 1] == b'\r' {
        end -= 1;
    }
    buffer[end] = 0;

    parse_trimmed_region(buffer, end)
}

/// kj's `HttpHeaders::parseHeaders(ptr, end)` entry: the buffer was ALREADY trimmed by
/// `trimHeaderEnding`, and `end` points at its '\0' sentinel — the caller passes the region
/// `[ptr, end]` INCLUSIVE of the sentinel, i.e. `buffer.last()` is the sentinel.
pub fn parse_http_headers_trimmed(buffer: &mut [u8]) -> HttpHeaderParseResult {
    match buffer.len().checked_sub(1) {
        Some(end_pos) => parse_trimmed_region(buffer, end_pos),
        None => parse_failed(),
    }
}

// =======================================================================================
// Range-header parsing (kj::tryParseHttpRangeHeader() and its consume* statics)
//
// The functions below parse HTTP "ranges specifiers" set in `Range` headers and defined by
// RFC9110 section 14.1: https://www.rfc-editor.org/rfc/rfc9110#section-14.1.
//
// Ranges specifiers consist of a case-insensitive "range unit", followed by an '=', followed
// by a comma separated list of "range specs". We currently only support byte ranges, with a
// range unit of "bytes". A byte range spec can either be:
//
// - An "int range" consisting of an inclusive start index, followed by a '-', and optionally
//   an inclusive end index (e.g. "2-5", "7-7", "9-"). Satisfiable if the start index is less
//   than the content length. Note the end index defaults to, and is clamped to the content
//   length.
// - A "suffix range" consisting of a '-', followed by a suffix length (e.g. "-5"). Satisfiable
//   if the suffix length is not 0. Note the suffix length is clamped to the content length.
//
// A full ranges specifier might look something like "bytes=2-4,-1", which requests bytes 2
// through 4, and the last byte.
//
// A range spec is invalid if it doesn't match the above structure, or if it is an int range
// with an end index > start index. A ranges specifier is invalid if any of its range specs
// are. A byte ranges specifier is satisfiable if at least one of its range specs are.
//
// kj scans the header's NUL-terminated in-buffer value; `at()` reproduces those semantics over
// a bounded slice by reading position `len` (and beyond) as the NUL terminator, so every
// cursor comparison — in particular the final `p == value.end()` acceptance check — lands
// exactly where kj's does.

/// `HttpRangesResult::kind`: an array of satisfiable ranges.
pub const HTTP_RANGES_KIND_RANGES: u8 = 0;
/// `HttpRangesResult::kind`: kj's `HttpEverythingRange` (a range spec covered the full body).
pub const HTTP_RANGES_KIND_EVERYTHING: u8 = 1;
/// `HttpRangesResult::kind`: kj's `HttpUnsatisfiableRange` (invalid or nothing satisfiable).
pub const HTTP_RANGES_KIND_UNSATISFIABLE: u8 = 2;

/// Read the byte at `pos`, with everything at/past the end reading as the NUL terminator kj's
/// pointer scan would see.
fn at(value: &[u8], pos: usize) -> u8 {
    value.get(pos).copied().unwrap_or(0)
}

/// kj's `skipSpace` over the virtually-NUL-terminated value.
fn skip_space_nul(value: &[u8], mut pos: usize) -> usize {
    while matches!(at(value, pos), b' ' | b'\t') {
        pos += 1;
    }
    pos
}

/// kj's `consumeNumber64`: accumulate a decimal number into a u64, rejecting (rather than
/// silently wrapping) numbers that don't fit; `pos` advances only on success.
fn consume_number64(value: &[u8], pos: &mut usize) -> Option<u64> {
    let start = skip_space_nul(value, *pos);
    let mut p = start;

    let mut result: u64 = 0;
    loop {
        let c = at(value, p);
        if c.is_ascii_digit() {
            let digit = u64::from(c - b'0');
            // Reject before `result * 10 + digit` would exceed a u64.
            if result > u64::MAX / 10 || (result == u64::MAX / 10 && digit > u64::MAX % 10) {
                return None;
            }
            result = result * 10 + digit;
            p += 1;
        } else {
            if p == start {
                return None;
            }
            *pos = p;
            return Some(result);
        }
    }
}

/// kj's `consumeByteRangeUnit`: case-insensitive "bytes", with surrounding optional whitespace.
fn consume_byte_range_unit(value: &[u8], pos: &mut usize) -> bool {
    let mut p = skip_space_nul(value, *pos);

    // Match case-insensitive "bytes"
    for &expected in b"bytes" {
        if at(value, p).to_ascii_lowercase() != expected {
            return false;
        }
        p += 1;
    }

    p = skip_space_nul(value, p);
    *pos = p;
    true
}

/// kj's `consumeIntRange`. `content_length.wrapping_sub(1)` reproduces kj's unsigned
/// `contentLength - 1` exactly, including the contentLength == 0 wraparound (whose out-of-order
/// endpoints the caller's `start <= end` satisfiability check then discards, as in kj).
fn consume_int_range(value: &[u8], pos: &mut usize, content_length: u64) -> Option<HttpRangeSpan> {
    let mut p = skip_space_nul(value, *pos);
    let first_pos = consume_number64(value, &mut p)?;
    p = skip_space_nul(value, p);
    if at(value, p) != b'-' {
        return None;
    }
    p += 1;
    p = skip_space_nul(value, p);
    let maybe_last_pos = consume_number64(value, &mut p);
    p = skip_space_nul(value, p);

    match maybe_last_pos {
        Some(mut last_pos) => {
            // "An int-range is invalid if the last-pos value is present and less than the
            // first-pos"
            if first_pos > last_pos {
                return None;
            }
            // "if the value is greater than or equal to the current length of the
            // representation data ... interpreted as the remainder of the representation"
            if last_pos >= content_length {
                last_pos = content_length.wrapping_sub(1);
            }
            *pos = p;
            Some(HttpRangeSpan {
                start: first_pos,
                end: last_pos,
            })
        }
        None => {
            // "if the last-pos value is absent ... interpreted as the remainder of the
            // representation"
            *pos = p;
            Some(HttpRangeSpan {
                start: first_pos,
                end: content_length.wrapping_sub(1),
            })
        }
    }
}

/// kj's `consumeSuffixRange`.
fn consume_suffix_range(
    value: &[u8],
    pos: &mut usize,
    content_length: u64,
) -> Option<HttpRangeSpan> {
    let mut p = skip_space_nul(value, *pos);
    if at(value, p) != b'-' {
        return None;
    }
    p += 1;
    p = skip_space_nul(value, p);
    let suffix_length = consume_number64(value, &mut p)?;
    p = skip_space_nul(value, p);

    *pos = p;
    if suffix_length >= content_length {
        // "if the selected representation is shorter than the specified suffix-length, the
        // entire representation is used"
        Some(HttpRangeSpan {
            start: 0,
            end: content_length.wrapping_sub(1),
        })
    } else {
        Some(HttpRangeSpan {
            start: content_length - suffix_length,
            end: content_length - 1,
        })
    }
}

/// kj's `consumeRangeSpec`: an int range, else a suffix range.
fn consume_range_spec(value: &[u8], pos: &mut usize, content_length: u64) -> Option<HttpRangeSpan> {
    match consume_int_range(value, pos, content_length) {
        Some(range) => Some(range),
        // If we failed to consume an int range, try consume a suffix range instead
        None => consume_suffix_range(value, pos, content_length),
    }
}

/// kj's `tryParseHttpRangeHeader`: returns the satisfiable ranges (`kind` 0), kj's
/// `HttpEverythingRange` (`kind` 1) if a range spec covered the full body, or kj's
/// `HttpUnsatisfiableRange` (`kind` 2) if the ranges specifier is invalid or nothing is
/// satisfiable.
pub fn parse_range_header(value: &[u8], content_length: u64) -> HttpRangesResult {
    let unsatisfiable = || HttpRangesResult {
        kind: HTTP_RANGES_KIND_UNSATISFIABLE,
        ranges: Vec::new(),
    };

    let mut pos = 0;
    if !consume_byte_range_unit(value, &mut pos) {
        return unsatisfiable();
    }
    if at(value, pos) != b'=' {
        return unsatisfiable();
    }
    pos += 1;

    let mut full_range = false;
    let mut satisfiable_ranges = Vec::new();
    loop {
        match consume_range_spec(value, &mut pos, content_length) {
            Some(range) => {
                // Don't record more ranges if we've already recorded a full range
                if !full_range && range.start <= range.end {
                    if range.start == 0 && range.end == content_length.wrapping_sub(1) {
                        // A range evaluated to the full range, but still need to check rest are
                        // valid
                        full_range = true;
                    } else {
                        // "a valid bytes range-spec is satisfiable if it is either:
                        // - an int-range with a first-pos that is less than the current length
                        //   of the selected representation or
                        // - a suffix-range with a non-zero suffix-length"
                        satisfiable_ranges.push(range);
                    }
                }
            }
            None => {
                // If we failed to parse a range, the whole range specification is invalid
                return unsatisfiable();
            }
        }
        // kj: `} while (*(p++) == ',');` then `if ((--p) != value.end())` — the loop leaves the
        // cursor one past the non-comma byte; success requires that byte to be the terminator.
        let c = at(value, pos);
        pos += 1;
        if c != b',' {
            break;
        }
    }

    pos -= 1;
    if pos != value.len() {
        return unsatisfiable();
    }
    if full_range {
        return HttpRangesResult {
            kind: HTTP_RANGES_KIND_EVERYTHING,
            ranges: Vec::new(),
        };
    }
    // "A valid ranges-specifier is "satisfiable" if it contains at least one range-spec that is
    // satisfiable"
    if satisfiable_ranges.is_empty() {
        return unsatisfiable();
    }
    HttpRangesResult {
        kind: HTTP_RANGES_KIND_RANGES,
        ranges: satisfiable_ranges,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn serialize(word1: &[u8], word2: &[u8], word3: &[u8], headers: &[(&[u8], &[u8])]) -> Vec<u8> {
        let mut serializer = new_http_serializer(word1, word2, word3);
        for (name, value) in headers {
            serializer.add_header(name, value);
        }
        serializer.finish()
    }

    #[test]
    fn serializes_request_head() {
        assert_eq!(
            serialize(
                b"GET",
                b"/path",
                b"HTTP/1.1",
                &[(b"Host", b"example.com"), (b"Foo-Header", b"baz")]
            ),
            b"GET /path HTTP/1.1\r\nHost: example.com\r\nFoo-Header: baz\r\n\r\n"
        );
    }

    #[test]
    fn serializes_headers_only() {
        // kj's toString(): serialize(nullptr, ...) emits no start line.
        assert_eq!(
            serialize(b"", b"", b"", &[(b"Foo", b"bar")]),
            b"Foo: bar\r\n\r\n"
        );
        assert_eq!(serialize(b"", b"", b"", &[]), b"\r\n");
    }

    #[test]
    fn validates_request_url() {
        assert!(is_valid_request_url(b"/"));
        assert!(is_valid_request_url(b"/foo?bar=baz#qux"));
        assert!(is_valid_request_url("/\u{00e9}".as_bytes())); // >= 0x80 permitted
        assert!(!is_valid_request_url(b"/foo bar"));
        assert!(!is_valid_request_url(b"/foo\rbar"));
        assert!(!is_valid_request_url(b"/foo\nbar"));
        assert!(!is_valid_request_url(b"/foo\x7fbar"));
        assert!(!is_valid_request_url(b"/foo\0bar"));
    }

    #[test]
    fn validates_status_text() {
        assert!(is_valid_status_text(b"OK"));
        assert!(is_valid_status_text(b"Not  Found\t!"));
        assert!(!is_valid_status_text(b"O\rK"));
        assert!(!is_valid_status_text(b"O\nK"));
        assert!(!is_valid_status_text(b"O\0K"));
    }

    fn spans_to_pairs(buffer: &[u8], result: &HttpHeaderParseResult) -> Vec<(Vec<u8>, Vec<u8>)> {
        result
            .spans
            .iter()
            .map(|span| {
                (
                    buffer[span.name_begin..span.name_end].to_vec(),
                    buffer[span.value_begin..span.value_end].to_vec(),
                )
            })
            .collect()
    }

    #[test]
    fn parses_headers_in_place() {
        let mut buffer = b"Host: example.com\r\nFoo:  bar \r\n\r\n".to_vec();
        let result = parse_http_headers(&mut buffer);
        assert!(result.ok);
        assert_eq!(
            spans_to_pairs(&buffer, &result),
            [
                (b"Host".to_vec(), b"example.com".to_vec()),
                (b"Foo".to_vec(), b"bar ".to_vec()),
            ]
        );
        // kj parity: each field NUL-terminated in place (the ':' and the '\r' of each line
        // ending overwritten), and the trailing blank line's '\r' replaced by the sentinel.
        assert_eq!(buffer, *b"Host\0 example.com\0\nFoo\0  bar \0\n\0\n");
    }

    #[test]
    fn parses_bare_lf_line_endings() {
        let mut buffer = b"Foo: bar\nBaz: qux\n\n".to_vec();
        let result = parse_http_headers(&mut buffer);
        assert!(result.ok);
        assert_eq!(
            spans_to_pairs(&buffer, &result),
            [
                (b"Foo".to_vec(), b"bar".to_vec()),
                (b"Baz".to_vec(), b"qux".to_vec()),
            ]
        );
    }

    #[test]
    fn rejects_malformed_headers() {
        // Too short / missing trailing newline.
        assert!(!parse_http_headers(&mut []).ok);
        assert!(!parse_http_headers(&mut b"\r".to_vec()).ok);
        assert!(!parse_http_headers(&mut b"Foo: bar\r\n\r".to_vec()).ok);
        // Empty header name.
        assert!(!parse_http_headers(&mut b": bar\r\n\r\n".to_vec()).ok);
        // Whitespace before the colon (RFC 9112 section 5.1: reject).
        assert!(!parse_http_headers(&mut b"Foo : bar\r\n\r\n".to_vec()).ok);
        // Non-token byte in the name.
        assert!(!parse_http_headers(&mut b"Fo@o: bar\r\n\r\n".to_vec()).ok);
    }

    #[test]
    fn rejects_obsolete_line_folding_after_mutating() {
        let mut buffer = b"Foo: line one\r\n  line two\r\n\r\n".to_vec();
        assert!(!parse_http_headers(&mut buffer).ok);
        // kj parity: the fold's \r\n was already replaced by spaces (and the name and final
        // line ending NUL-terminated) before the rejection.
        assert_eq!(buffer, *b"Foo\0 line one    line two\0\n\0\n");
    }

    #[test]
    fn parses_trimmed_region() {
        // parseHeaders(ptr, end) contract: already-trimmed buffer, sentinel at the last index.
        let mut buffer = b"Foo: bar\r\n\0".to_vec();
        let result = parse_http_headers_trimmed(&mut buffer);
        assert!(result.ok);
        assert_eq!(
            spans_to_pairs(&buffer, &result),
            [(b"Foo".to_vec(), b"bar".to_vec())]
        );
        assert!(!parse_http_headers_trimmed(&mut []).ok);
    }

    #[track_caller]
    fn expect_ranges(value: &[u8], content_length: u64, expected: &[(u64, u64)]) {
        let result = parse_range_header(value, content_length);
        assert_eq!(result.kind, HTTP_RANGES_KIND_RANGES);
        let ranges: Vec<(u64, u64)> = result.ranges.iter().map(|r| (r.start, r.end)).collect();
        assert_eq!(ranges, expected);
    }

    // The cases below mirror kj's "HttpRanges" test in kj/compat/http-test.c++.
    #[test]
    fn parses_range_headers() {
        expect_ranges(b"bytes=2-5", 8, &[(2, 5)]);
        expect_ranges(b"bytes=2-5, -3", 8, &[(2, 5), (5, 7)]);
        expect_ranges(b" byTES = 2 - 5 , - 3 ", 8, &[(2, 5), (5, 7)]);
        expect_ranges(b"bytes=6-", 8, &[(6, 7)]);
        // Clamped end.
        expect_ranges(b"bytes=2-9", 8, &[(2, 7)]);
        // Oversized suffix -> everything.
        assert_eq!(
            parse_range_header(b"bytes=-12", 8).kind,
            HTTP_RANGES_KIND_EVERYTHING
        );
        assert_eq!(
            parse_range_header(b"bytes=0-7", 8).kind,
            HTTP_RANGES_KIND_EVERYTHING
        );
        // Unsatisfiable / invalid.
        assert_eq!(
            parse_range_header(b"bytes=8-", 8).kind,
            HTTP_RANGES_KIND_UNSATISFIABLE
        );
        assert_eq!(
            parse_range_header(b"bytes=-0", 8).kind,
            HTTP_RANGES_KIND_UNSATISFIABLE
        );
        assert_eq!(
            parse_range_header(b"bytes=5-2", 8).kind,
            HTTP_RANGES_KIND_UNSATISFIABLE
        );
        assert_eq!(
            parse_range_header(b"bites=2-5", 8).kind,
            HTTP_RANGES_KIND_UNSATISFIABLE
        );
        assert_eq!(
            parse_range_header(b"bytes=2-5,junk", 8).kind,
            HTTP_RANGES_KIND_UNSATISFIABLE
        );
        assert_eq!(
            parse_range_header(b"bytes=", 8).kind,
            HTTP_RANGES_KIND_UNSATISFIABLE
        );
        // One satisfiable spec among unsatisfiable-but-valid ones is enough.
        expect_ranges(b"bytes=8-,2-5", 8, &[(2, 5)]);
    }
}
