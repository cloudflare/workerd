// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The text forms of trace, span and script version IDs, as `tracing::TraceId`,
//! `tracing::SpanId` and `workerd::UUID` write them.

/// The text of an ID, kept on the stack: the longest, a UUID, has 36 characters.
pub struct IdText {
    bytes: [u8; 36],
    len: usize,
}

impl IdText {
    fn new() -> Self {
        Self {
            bytes: [0; 36],
            len: 0,
        }
    }

    /// Appends the low `digits` hex digits of `value`, in lowercase.
    fn hex(mut self, value: u64, digits: usize) -> Self {
        const DIGITS: &[u8; 16] = b"0123456789abcdef";
        for i in 0..digits {
            let nibble = (value >> (4 * (digits - 1 - i))) & 0xf;
            self.bytes[self.len + i] = DIGITS[usize::try_from(nibble).unwrap_or_default()];
        }
        self.len += digits;
        self
    }

    fn dash(mut self) -> Self {
        self.bytes[self.len] = b'-';
        self.len += 1;
        self
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes[..self.len]
    }

    pub fn as_str(&self) -> &str {
        // Only ASCII hex digits and dashes are ever written.
        std::str::from_utf8(self.as_bytes()).unwrap_or_default()
    }
}

impl std::fmt::Debug for IdText {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.as_str().fmt(f)
    }
}

impl PartialEq<&str> for IdText {
    fn eq(&self, other: &&str) -> bool {
        self.as_str() == *other
    }
}

/// The trace ID `high`:`low` in lowercase hex: 16 digits if `high` is 0, 32 otherwise, as
/// `TraceId::toGoString()` writes it.
pub fn trace_id_go(high: u64, low: u64) -> IdText {
    if high == 0 {
        IdText::new().hex(low, 16)
    } else {
        IdText::new().hex(high, 16).hex(low, 16)
    }
}

/// The trace ID `high`:`low` in 32 lowercase hex digits, as `TraceId::toW3C()` writes it.
pub fn trace_id_w3c(high: u64, low: u64) -> IdText {
    IdText::new().hex(high, 16).hex(low, 16)
}

/// The span ID in 16 lowercase hex digits, as `SpanId::toGoString()` writes it.
pub fn span_id_go(id: u64) -> IdText {
    IdText::new().hex(id, 16)
}

/// The UUID `upper`:`lower` as lowercase 8-4-4-4-12 hex groups, or `None` if both halves are 0,
/// as `UUID::fromUpperLower()` and `UUID::toString()` do.
pub fn uuid(upper: u64, lower: u64) -> Option<IdText> {
    if upper == 0 && lower == 0 {
        return None;
    }
    Some(
        IdText::new()
            .hex(upper >> 32, 8)
            .dash()
            .hex(upper >> 16, 4)
            .dash()
            .hex(upper, 4)
            .dash()
            .hex(lower >> 48, 4)
            .dash()
            .hex(lower, 12),
    )
}

#[cfg(test)]
#[path = "ids-test.rs"]
mod tests;
