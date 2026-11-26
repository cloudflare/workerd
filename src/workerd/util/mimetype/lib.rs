//! A MIME type parser and serializer for `MimeType` in `workerd/util/mimetype.h`, after the
//! WHATWG MIME Sniffing standard (<https://mimesniff.spec.whatwg.org/>) and the Fetch standard's
//! extraction of a MIME type from a `Content-Type` header.
//!
//! It works on bytes, since a header value need not be UTF-8, and allocates nothing for input
//! without escapes: what it parses is handed out as slices of the input, in the case it was
//! written in. The caller owns the parameters it is given, lowercases what the standard
//! lowercases, and keeps the first value of a repeated parameter name.

mod ffi;
mod parse;
mod serialize;

pub use parse::Essence;
pub use parse::extract;
pub use parse::parse_essence;
pub use parse::parse_params;
pub use serialize::Param;
pub use serialize::params_serialized_len;
pub use serialize::serialize;
pub use serialize::serialize_params;
pub use serialize::serialized_len;

/// HTTP whitespace as MIME type parsing skips it: CR, LF, tab and space.
const fn is_whitespace(c: u8) -> bool {
    matches!(c, b'\r' | b'\n' | b'\t' | b' ')
}

const TOKEN_CHARS: [bool; 256] = {
    let mut table = [false; 256];
    let punctuation = b"!#$%&'*+\\-.^_`|~";
    let mut i = 0;
    while i < punctuation.len() {
        table[punctuation[i] as usize] = true;
        i += 1;
    }
    let mut c = 0;
    while c < 256 {
        if (c as u8).is_ascii_alphanumeric() {
            table[c] = true;
        }
        c += 1;
    }
    table
};

/// Whether `c` is an HTTP token code point. The set includes `\`, which RFC 9110 leaves out.
const fn is_token_char(c: u8) -> bool {
    TOKEN_CHARS[c as usize]
}

/// Whether `c` may appear in a parameter value: tab, `0x20` to `0x7E`, and `0x80` to `0xFE`.
/// `0xFF` is not one of them.
const fn is_param_value_char(c: u8) -> bool {
    c == b'\t' || (c >= 0x20 && c <= 0x7e) || (c >= 0x80 && c < 0xff)
}

/// Whether `s` is a valid type, subtype or parameter name: one or more token code points.
#[must_use]
pub fn is_token(s: &[u8]) -> bool {
    !s.is_empty() && s.iter().all(|&c| is_token_char(c))
}

/// Whether `s` is a valid parameter value. The empty value is.
#[must_use]
pub fn is_param_value(s: &[u8]) -> bool {
    s.iter().all(|&c| is_param_value_char(c))
}

fn skip_leading_whitespace(s: &[u8]) -> &[u8] {
    let start = s.iter().position(|&c| !is_whitespace(c)).unwrap_or(s.len());
    &s[start..]
}

fn trim_trailing_whitespace(s: &[u8]) -> &[u8] {
    let end = s
        .iter()
        .rposition(|&c| !is_whitespace(c))
        .map_or(0, |i| i + 1);
    &s[..end]
}

fn find(s: &[u8], c: u8) -> Option<usize> {
    s.iter().position(|&b| b == c)
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
