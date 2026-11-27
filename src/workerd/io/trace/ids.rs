// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Trace ids, span ids and trace flags, with the text and byte forms of `workerd/io/trace.h`.

use std::fmt;

/// The text of an id or a `traceparent`, kept on the stack. The longest, a `traceparent`, has
/// 55 characters.
#[derive(Clone, Copy)]
pub struct IdText {
    bytes: [u8; 55],
    len: u8,
}

const HEX_DIGITS: &[u8; 16] = b"0123456789abcdef";

impl IdText {
    fn new() -> Self {
        Self {
            bytes: [0; 55],
            len: 0,
        }
    }

    /// Appends `value` as 16 lowercase hex digits.
    fn hex64(self, value: u64) -> Self {
        self.hex(value, 16)
    }

    /// Appends the low `digits` hex digits of `value`, in lowercase.
    fn hex(mut self, value: u64, digits: usize) -> Self {
        let start = usize::from(self.len);
        for (i, out) in self.bytes[start..start + digits].iter_mut().enumerate() {
            let shift = 4 * (digits - 1 - i);
            *out = HEX_DIGITS[usize::try_from((value >> shift) & 0xf).unwrap_or_default()];
        }
        self.len += u8::try_from(digits).unwrap_or(u8::MAX);
        self
    }

    fn push(mut self, byte: u8) -> Self {
        self.bytes[usize::from(self.len)] = byte;
        self.len += 1;
        self
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes[..usize::from(self.len)]
    }

    pub fn as_str(&self) -> &str {
        // Only ASCII hex digits and dashes are ever written.
        std::str::from_utf8(self.as_bytes()).unwrap_or_default()
    }
}

impl fmt::Display for IdText {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.as_str())
    }
}

impl fmt::Debug for IdText {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Debug::fmt(self.as_str(), f)
    }
}

impl PartialEq<&str> for IdText {
    fn eq(&self, other: &&str) -> bool {
        self.as_str() == *other
    }
}

fn hex_digit(c: u8) -> Option<u64> {
    match c {
        b'0'..=b'9' => Some(u64::from(c - b'0')),
        b'a'..=b'f' => Some(u64::from(c - b'a' + 10)),
        b'A'..=b'F' => Some(u64::from(c - b'A' + 10)),
        _ => None,
    }
}

/// Parses up to 16 hex digits, in either case. An empty input is 0; a longer one is `None`.
pub fn parse_hex64(s: &[u8]) -> Option<u64> {
    if s.len() > 16 {
        return None;
    }
    s.iter()
        .try_fold(0u64, |value, &c| Some((value << 4) | hex_digit(c)?))
}

/// A 128-bit trace id, `tracing::TraceId`. It identifies a trace, and for tail workers also an
/// invocation. The zero id is the null id, which only tests should use.
#[derive(Clone, Copy, Default, PartialEq, Eq, Hash)]
pub struct TraceId {
    pub high: u64,
    pub low: u64,
}

impl TraceId {
    pub const NULL: Self = Self { high: 0, low: 0 };

    pub fn is_null(self) -> bool {
        self == Self::NULL
    }

    /// The id as Jaeger's Go library writes it: 16 lowercase hex digits if `high` is 0, 32
    /// otherwise.
    ///
    /// Reference: <https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L50>
    pub fn to_go_text(self) -> IdText {
        if self.high == 0 {
            IdText::new().hex64(self.low)
        } else {
            IdText::new().hex64(self.high).hex64(self.low)
        }
    }

    /// The id as W3C Trace Context writes it: 32 lowercase hex digits.
    ///
    /// Reference: <https://www.w3.org/TR/trace-context/#trace-id>
    pub fn to_w3c_text(self) -> IdText {
        IdText::new().hex64(self.high).hex64(self.low)
    }

    /// Parses the id as Jaeger's Go library does: up to 32 hex digits in either case, the last
    /// 16 being `low`. An empty string is the null id.
    ///
    /// Reference: <https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L58>
    pub fn from_go_text(s: &[u8]) -> Option<Self> {
        let n = s.len();
        if n > 32 {
            None
        } else if n <= 16 {
            Some(Self {
                high: 0,
                low: parse_hex64(s)?,
            })
        } else {
            let (high, low) = s.split_at(n - 16);
            Some(Self {
                high: parse_hex64(high)?,
                low: parse_hex64(low)?,
            })
        }
    }

    /// Parses exactly 32 hex digits, in either case. Zero is allowed.
    pub fn from_w3c_text(s: &[u8]) -> Option<Self> {
        if s.len() != 32 {
            return None;
        }
        Some(Self {
            high: parse_hex64(&s[..16])?,
            low: parse_hex64(&s[16..])?,
        })
    }

    /// The id as Jaeger's protobuf bytes: `high` then `low`, big-endian.
    ///
    /// Reference: <https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L111>
    pub fn to_protobuf(self) -> [u8; 16] {
        let mut out = [0; 16];
        out[..8].copy_from_slice(&self.high.to_be_bytes());
        out[8..].copy_from_slice(&self.low.to_be_bytes());
        out
    }

    /// Parses Jaeger's protobuf bytes, which must be exactly 16.
    ///
    /// Reference: <https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L81>
    pub fn from_protobuf(buf: &[u8]) -> Option<Self> {
        let buf: &[u8; 16] = buf.try_into().ok()?;
        let (high, low) = buf.split_at(8);
        Some(Self {
            high: u64::from_be_bytes(high.try_into().ok()?),
            low: u64::from_be_bytes(low.try_into().ok()?),
        })
    }
}

impl fmt::Display for TraceId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.to_go_text().fmt(f)
    }
}

impl fmt::Debug for TraceId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "TraceId({})", self.to_w3c_text())
    }
}

/// A 64-bit span id, `tracing::SpanId`. Zero is the null id: C++ uses it for "no span" in some
/// places and lets it through from capnp in others, so it is a value here too.
#[derive(Clone, Copy, Default, PartialEq, Eq, Hash)]
pub struct SpanId(pub u64);

impl SpanId {
    pub const NULL: Self = Self(0);

    pub fn is_null(self) -> bool {
        self.0 == 0
    }

    /// The id as Jaeger's Go library writes it: 16 lowercase hex digits.
    ///
    /// Reference: <https://github.com/jaegertracing/jaeger/blob/e46f8737/model/ids.go#L165>
    pub fn to_go_text(self) -> IdText {
        IdText::new().hex64(self.0)
    }

    /// Parses exactly 16 hex digits, in either case, as W3C writes a parent id. Zero is
    /// allowed.
    pub fn from_w3c_text(s: &[u8]) -> Option<Self> {
        if s.len() != 16 {
            return None;
        }
        Some(Self(parse_hex64(s)?))
    }
}

impl fmt::Display for SpanId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.to_go_text().fmt(f)
    }
}

impl fmt::Debug for SpanId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "SpanId({})", self.to_go_text())
    }
}

/// W3C trace flags, `tracing::TraceFlags`. Where none may have been chosen, it is an `Option`.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct TraceFlags(pub u8);

impl TraceFlags {
    /// The W3C bit by which the caller asks for the trace to be recorded.
    const SAMPLED: u8 = 0x01;

    pub fn is_sampled(self) -> bool {
        self.0 & Self::SAMPLED != 0
    }
}

/// The `traceparent` header (`{version}-{trace-id}-{parent-id}-{flags}`) of a trace id, span id
/// and flags, with version 00.
///
/// Reference: <https://www.w3.org/TR/trace-context/#traceparent-header>
pub fn format_traceparent(trace_id: TraceId, span_id: SpanId, flags: TraceFlags) -> IdText {
    IdText::new()
        .push(b'0')
        .push(b'0')
        .push(b'-')
        .hex64(trace_id.high)
        .hex64(trace_id.low)
        .push(b'-')
        .hex64(span_id.0)
        .push(b'-')
        .hex(u64::from(flags.0), 2)
}

#[cfg(test)]
#[path = "ids-test.rs"]
mod tests;
