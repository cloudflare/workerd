// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! JSON as capnp's C++ `JsonCodec` reads and writes it (capnp/compat/json.c++), so that the
//! fallback service sees the same requests and gets the same leniency as from C++.
//!
//! The reader follows the codec's parser, quirks included:
//!
//! - Strings are bytes: raw bytes, valid UTF-8 or not, and control characters pass through.
//! - Each `\uXXXX` escape is decoded on its own, as `kj::decodeUtf16()` decodes one UTF-16 unit:
//!   a surrogate, paired or not, becomes its own three-byte sequence.
//! - A NUL byte ends the input, so anything after one is ignored.
//! - Whitespace is space, tab, CR and LF; numbers are `strtod()`'s reading of the JSON number
//!   grammar with optional fraction and exponent digits (`1.` is a number); arrays and objects
//!   nest at most 64 deep; an object's members keep their order and duplicates.

/// A JSON value, as the codec's `JsonValue` holds it.
#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Number(f64),
    String(Vec<u8>),
    Array(Vec<Self>),
    Object(Vec<(Vec<u8>, Self)>),
}

/// `JsonCodec`'s default nesting limit.
const MAX_NESTING_DEPTH: usize = 64;

/// Parses `input` as `JsonCodec::decodeRaw()` does.
pub fn parse(input: &[u8]) -> Result<Json, String> {
    let mut parser = Parser {
        input,
        nesting_depth: 0,
    };
    let value = parser.parse_value()?;
    if !parser.exhausted() {
        return Err("Input remains after parsing JSON.".to_owned());
    }
    Ok(value)
}

struct Parser<'a> {
    input: &'a [u8],
    nesting_depth: usize,
}

fn premature() -> String {
    "JSON message ends prematurely.".to_owned()
}

fn unexpected() -> String {
    "Unexpected input in JSON message.".to_owned()
}

impl Parser<'_> {
    fn exhausted(&self) -> bool {
        self.input.first().is_none_or(|&c| c == 0)
    }

    fn next_char(&self) -> Result<u8, String> {
        match self.input.first() {
            Some(&c) if c != 0 => Ok(c),
            _ => Err(premature()),
        }
    }

    fn advance(&mut self, n: usize) -> Result<(), String> {
        self.input = self.input.get(n..).ok_or_else(premature)?;
        Ok(())
    }

    fn consume_char(&mut self, expected: u8) -> Result<(), String> {
        if self.next_char()? != expected {
            return Err(unexpected());
        }
        self.advance(1)
    }

    fn consume_literal(&mut self, expected: &[u8]) -> Result<(), String> {
        if !self.input.starts_with(expected) {
            return Err(unexpected());
        }
        self.advance(expected.len())
    }

    fn try_consume(&mut self, expected: u8) -> bool {
        let found = !self.exhausted() && self.input.first() == Some(&expected);
        if found {
            self.input = &self.input[1..];
        }
        found
    }

    fn consume_while(&mut self, predicate: impl Fn(u8) -> bool) -> &[u8] {
        let original = self.input;
        let mut n = 0;
        while original.get(n).is_some_and(|&c| c != 0 && predicate(c)) {
            n += 1;
        }
        self.input = &original[n..];
        &original[..n]
    }

    fn consume_whitespace(&mut self) {
        self.consume_while(|c| matches!(c, b' ' | b'\n' | b'\r' | b'\t'));
    }

    fn parse_value(&mut self) -> Result<Json, String> {
        self.consume_whitespace();
        if self.exhausted() {
            return Err(premature());
        }
        let value = match self.next_char()? {
            b'n' => {
                self.consume_literal(b"null")?;
                Json::Null
            }
            b'f' => {
                self.consume_literal(b"false")?;
                Json::Bool(false)
            }
            b't' => {
                self.consume_literal(b"true")?;
                Json::Bool(true)
            }
            b'"' => Json::String(self.consume_quoted_string()?),
            b'[' => self.parse_array()?,
            b'{' => self.parse_object()?,
            b'-' | b'0'..=b'9' => Json::Number(self.consume_number()?),
            _ => return Err(unexpected()),
        };
        self.consume_whitespace();
        Ok(value)
    }

    fn enter(&mut self) -> Result<(), String> {
        self.nesting_depth += 1;
        if self.nesting_depth > MAX_NESTING_DEPTH {
            return Err("JSON message nested too deeply.".to_owned());
        }
        Ok(())
    }

    fn parse_array(&mut self) -> Result<Json, String> {
        let mut values = Vec::new();
        self.consume_char(b'[')?;
        self.enter()?;
        loop {
            self.consume_whitespace();
            if self.next_char()? == b']' {
                break;
            }
            if !values.is_empty() {
                self.consume_whitespace();
                self.consume_char(b',')?;
                self.consume_whitespace();
            }
            values.push(self.parse_value()?);
        }
        self.nesting_depth -= 1;
        self.consume_char(b']')?;
        Ok(Json::Array(values))
    }

    fn parse_object(&mut self) -> Result<Json, String> {
        let mut members = Vec::new();
        self.consume_char(b'{')?;
        self.enter()?;
        loop {
            self.consume_whitespace();
            if self.next_char()? == b'}' {
                break;
            }
            if !members.is_empty() {
                self.consume_whitespace();
                self.consume_char(b',')?;
                self.consume_whitespace();
            }
            let name = self.consume_quoted_string()?;
            self.consume_whitespace();
            self.consume_char(b':')?;
            self.consume_whitespace();
            let value = self.parse_value()?;
            members.push((name, value));
        }
        self.nesting_depth -= 1;
        self.consume_char(b'}')?;
        Ok(Json::Object(members))
    }

    fn consume_quoted_string(&mut self) -> Result<Vec<u8>, String> {
        self.consume_char(b'"')?;
        let mut decoded = Vec::new();
        loop {
            decoded.extend_from_slice(self.consume_while(|c| c != b'"' && c != b'\\'));
            if self.next_char()? == b'\\' {
                self.advance(1)?;
                let escaped = match self.next_char()? {
                    b'"' => b'"',
                    b'\\' => b'\\',
                    b'/' => b'/',
                    b'b' => 0x08,
                    b'f' => 0x0c,
                    b'n' => b'\n',
                    b'r' => b'\r',
                    b't' => b'\t',
                    b'u' => {
                        self.advance(1)?;
                        let hex = self.input.get(..4).ok_or_else(premature)?;
                        unescape_and_append(hex, &mut decoded)?;
                        self.advance(4)?;
                        continue;
                    }
                    _ => return Err("Invalid escape in JSON string.".to_owned()),
                };
                decoded.push(escaped);
                self.advance(1)?;
            } else {
                break;
            }
        }
        self.consume_char(b'"')?;
        Ok(decoded)
    }

    fn consume_number(&mut self) -> Result<f64, String> {
        let start = self.input;
        self.try_consume(b'-');
        if !self.try_consume(b'0') {
            if !matches!(self.next_char()?, b'1'..=b'9') {
                return Err(unexpected());
            }
            self.advance(1)?;
            self.consume_while(|c| c.is_ascii_digit());
        }
        if self.try_consume(b'.') {
            self.consume_while(|c| c.is_ascii_digit());
        }
        if self.try_consume(b'e') || self.try_consume(b'E') {
            let _ = self.try_consume(b'+') || self.try_consume(b'-');
            self.consume_while(|c| c.is_ascii_digit());
        }
        let text = &start[..start.len() - self.input.len()];
        // Only ASCII digits, signs, '.', 'e' and 'E' were consumed.
        let text = std::str::from_utf8(text).map_err(|_| unexpected())?;
        // `strtod()` reads all of what the grammar above accepts except an exponent without digits
        // (`1e`), which `kj::StringPtr::parseAs<double>()` rejects; so does Rust's parser.
        text.parse::<f64>()
            .map_err(|_| format!("String does not contain valid floating number; s = {text}"))
    }
}

/// Appends one `\uXXXX` escape, as the codec's `unescapeAndAppend()` does: a code point below
/// 0x80 as itself, anything else as `kj::decodeUtf16()` encodes a single UTF-16 unit.
fn unescape_and_append(hex: &[u8], target: &mut Vec<u8>) -> Result<(), String> {
    let mut unit: u16 = 0;
    for &c in hex {
        let digit = match c {
            b'0'..=b'9' => c - b'0',
            b'a'..=b'f' => c - b'a' + 10,
            b'A'..=b'F' => c - b'A' + 10,
            _ => {
                return Err(format!(
                    "Invalid hex digit in unicode escape.; c = {}",
                    char::from(c)
                ));
            }
        };
        unit = (unit << 4) | u16::from(digit);
    }
    let [high, low] = unit.to_be_bytes();
    if unit < 0x80 {
        target.push(low);
    } else if unit < 0x800 {
        target.push(0xc0 | (high << 2) | (low >> 6));
        target.push(0x80 | (low & 0x3f));
    } else {
        // Three bytes, surrogates included: a lone unit never forms a pair.
        target.push(0xe0 | (high >> 4));
        target.push(0x80 | ((high & 0x0f) << 2) | (low >> 6));
        target.push(0x80 | (low & 0x3f));
    }
    Ok(())
}

/// Appends `chars` as a JSON string, escaped as `JsonCodec`'s `encodeString()` escapes it.
///
/// `"`, `\` and the control characters below 0x20 are escaped, with the short escapes where JSON
/// has one; every other byte is written as it is.
pub fn encode_string(chars: &[u8], out: &mut Vec<u8>) {
    const HEX_DIGITS: &[u8; 16] = b"0123456789abcdef";
    out.push(b'"');
    for &c in chars {
        match c {
            b'"' => out.extend_from_slice(b"\\\""),
            b'\\' => out.extend_from_slice(b"\\\\"),
            0x08 => out.extend_from_slice(b"\\b"),
            0x0c => out.extend_from_slice(b"\\f"),
            b'\n' => out.extend_from_slice(b"\\n"),
            b'\r' => out.extend_from_slice(b"\\r"),
            b'\t' => out.extend_from_slice(b"\\t"),
            c if c < 0x20 => {
                out.extend_from_slice(b"\\u00");
                out.push(HEX_DIGITS[usize::from(c / 16)]);
                out.push(HEX_DIGITS[usize::from(c % 16)]);
            }
            c => out.push(c),
        }
    }
    out.push(b'"');
}

#[cfg(test)]
#[path = "json-test.rs"]
mod tests;
