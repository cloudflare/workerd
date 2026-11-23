// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The aws-chunked framing decoder: a push parser that is fed the body as it arrives.
//!
//! The framing is that of HTTP/1.1 chunked transfer coding (RFC 9112 section 7.1):
//! `hex-size[;extensions] CRLF data CRLF` per chunk, then a zero-size chunk, the trailer
//! fields, and a final CRLF.

use std::fmt;

/// A size line's extensions hold at most a chunk signature in practice; this bounds the work
/// spent skipping them.
pub const MAX_EXTENSION_BYTES: usize = 4096;
/// 16 hex digits express any 64-bit size.
const MAX_SIZE_DIGITS: usize = 16;

/// Why the input is not a valid aws-chunked body. Every failure is a `TypeError` in
/// JavaScript.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Malformed(pub String);

impl fmt::Display for Malformed {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

fn malformed<T>(message: impl Into<String>) -> Result<T, Malformed> {
    Err(Malformed(message.into()))
}

/// A trailer field, with its name as sent and its value trimmed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Field {
    pub name: String,
    /// The value as it was sent. HTTP allows any byte from 0x80 up in a field value; a field that
    /// is kept must be UTF-8, so that it is returned unchanged.
    pub value: String,
}

#[derive(Clone, Debug)]
pub struct Options {
    /// The decoded length (`x-amz-decoded-content-length`). If set, any other length is an error.
    pub decoded_length: Option<u64>,
    /// The trailer fields to keep, compared case-insensitively. `None` keeps them all.
    pub allowed_trailers: Option<Vec<String>>,
    /// The most bytes the trailer section, with its final CRLF, may take.
    pub max_trailer_bytes: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    Size,
    /// Parsing the chunk extensions, as `Decoder::extension` says.
    Extension,
    SizeLf,
    Data,
    DataCr,
    DataLf,
    Trailer,
    Done,
}

/// Where the extension parser is in RFC 9112's
/// `chunk-ext = *( BWS ";" BWS chunk-ext-name [ BWS "=" BWS chunk-ext-val ] )`,
/// where `chunk-ext-name` is a token and `chunk-ext-val` is a token or a quoted-string.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Extension {
    /// After the size or an extension; a ";" or the line ending may follow.
    BeforeSemicolon,
    BeforeName,
    Name,
    /// After an extension's name and some whitespace; a "=" or ";" may follow.
    AfterName,
    BeforeValue,
    TokenValue,
    QuotedValue,
    /// After a backslash in a quoted value.
    QuotedPair,
}

/// RFC 9110 tchar.
const fn is_token_char(c: u8) -> bool {
    c.is_ascii_alphanumeric()
        || matches!(
            c,
            b'!' | b'#'
                | b'$'
                | b'%'
                | b'&'
                | b'\''
                | b'*'
                | b'+'
                | b'-'
                | b'.'
                | b'^'
                | b'_'
                | b'`'
                | b'|'
                | b'~'
        )
}

const fn is_whitespace(c: u8) -> bool {
    c == b' ' || c == b'\t'
}

/// RFC 9110 qdtext, the characters a quoted-string holds without escaping.
const fn is_quoted_text_char(c: u8) -> bool {
    matches!(c, b'\t' | b' ' | 0x21 | 0x23..=0x5b | 0x5d..=0x7e | 0x80..=0xff)
}

/// RFC 9110 quoted-pair: the characters a backslash may escape.
const fn is_quotable_pair_char(c: u8) -> bool {
    matches!(c, b'\t' | b' ' | 0x21..=0x7e | 0x80..=0xff)
}

/// The flexible-checksum trailers, `x-amz-checksum-<algorithm>`.
fn is_checksum_field(name: &[u8]) -> bool {
    const PREFIX: &[u8] = b"x-amz-checksum-";
    name.len() > PREFIX.len() && name[..PREFIX.len()].eq_ignore_ascii_case(PREFIX)
}

fn trim_whitespace(text: &[u8]) -> &[u8] {
    let start = text
        .iter()
        .position(|&c| !is_whitespace(c))
        .unwrap_or(text.len());
    let end = text
        .iter()
        .rposition(|&c| !is_whitespace(c))
        .map_or(start, |i| i + 1);
    &text[start..end]
}

fn malformed_extension<T>() -> Result<T, Malformed> {
    malformed("Malformed aws-chunked chunk extension.")
}

/// Decodes one aws-chunked body. Once a method has returned an error, the decoder must not be
/// used again.
pub struct Decoder {
    options: Options,
    state: State,
    size_digits: usize,
    extension: Extension,
    /// True when the extension parser has just passed whitespace, which the line ending may not
    /// follow.
    after_whitespace: bool,
    extension_bytes: usize,
    chunk_remaining: u64,
    decoded: u64,
    trailer_bytes: Vec<u8>,
    trailers: Vec<Field>,
}

impl Decoder {
    #[must_use]
    pub const fn new(options: Options) -> Self {
        Self {
            options,
            state: State::Size,
            size_digits: 0,
            extension: Extension::BeforeSemicolon,
            after_whitespace: false,
            extension_bytes: 0,
            chunk_remaining: 0,
            decoded: 0,
            trailer_bytes: Vec::new(),
            trailers: Vec::new(),
        }
    }

    /// The bytes the decoder holds between calls: its allowed trailer names and a partial
    /// trailer section.
    #[must_use]
    pub fn retained_bytes(&self) -> usize {
        let allowed = self.options.allowed_trailers.as_ref().map_or(0, |names| {
            names.capacity() * size_of::<String>()
                + names.iter().map(String::capacity).sum::<usize>()
        });
        allowed + self.trailer_bytes.capacity()
    }

    /// Decodes `input`, appending the chunk data in it to `output`. On failure, `output` holds
    /// the data decoded before the error.
    ///
    /// # Errors
    ///
    /// Fails if the input is not a valid continuation of the body.
    pub fn push(&mut self, mut input: &[u8], output: &mut Vec<u8>) -> Result<(), Malformed> {
        while let Some(&c) = input.first() {
            match self.state {
                State::Size => {
                    if let Some(digit) = char::from(c).to_digit(16) {
                        if self.size_digits == MAX_SIZE_DIGITS {
                            return malformed(format!(
                                "An aws-chunked chunk size has more than {MAX_SIZE_DIGITS} digits."
                            ));
                        }
                        self.chunk_remaining = self.chunk_remaining << 4 | u64::from(digit);
                        self.size_digits += 1;
                    } else if self.size_digits == 0
                        || !(c == b'\r' || c == b';' || is_whitespace(c))
                    {
                        return malformed("Malformed aws-chunked chunk size.");
                    } else if c == b'\r' {
                        self.state = State::SizeLf;
                    } else {
                        // The extensions start here, and the parser takes this byte too.
                        self.state = State::Extension;
                        self.extension = Extension::BeforeSemicolon;
                        self.after_whitespace = false;
                        continue;
                    }
                    input = &input[1..];
                }
                State::Extension => {
                    input = &input[1..];
                    if c == b'\r' {
                        self.end_extensions()?;
                    } else {
                        self.extension_bytes += 1;
                        if self.extension_bytes > MAX_EXTENSION_BYTES {
                            return malformed(format!(
                                "An aws-chunked chunk extension is longer than \
                                 {MAX_EXTENSION_BYTES} bytes."
                            ));
                        }
                        self.extension_byte(c)?;
                    }
                }
                State::SizeLf => {
                    if c != b'\n' {
                        return malformed("Malformed aws-chunked chunk size line.");
                    }
                    input = &input[1..];
                    self.start_chunk()?;
                }
                State::Data => {
                    let n = usize::try_from(self.chunk_remaining)
                        .map_or(input.len(), |remaining| remaining.min(input.len()));
                    output.extend_from_slice(&input[..n]);
                    self.decoded += n as u64;
                    self.chunk_remaining -= n as u64;
                    input = &input[n..];
                    if self.chunk_remaining == 0 {
                        self.state = State::DataCr;
                    }
                }
                State::DataCr => {
                    if c != b'\r' {
                        return malformed("An aws-chunked chunk is longer than its declared size.");
                    }
                    input = &input[1..];
                    self.state = State::DataLf;
                }
                State::DataLf => {
                    if c != b'\n' {
                        return malformed("Malformed aws-chunked chunk ending.");
                    }
                    input = &input[1..];
                    self.state = State::Size;
                }
                State::Trailer => input = self.consume_trailer(input)?,
                State::Done => {
                    return malformed("The aws-chunked body continues past its trailer.");
                }
            }
        }
        Ok(())
    }

    /// Finishes the body, returning its trailer fields.
    ///
    /// # Errors
    ///
    /// Fails if the body ended before its final chunk and trailer.
    pub fn end(&mut self) -> Result<Vec<Field>, Malformed> {
        if self.state != State::Done {
            return malformed("The aws-chunked body ended before its final chunk and trailer.");
        }
        Ok(std::mem::take(&mut self.trailers))
    }

    /// Advances the extension parser over one byte of the size line, other than its CR.
    fn extension_byte(&mut self, c: u8) -> Result<(), Malformed> {
        let whitespace = is_whitespace(c);
        self.extension = match (self.extension, c) {
            (
                Extension::BeforeSemicolon
                | Extension::Name
                | Extension::AfterName
                | Extension::TokenValue,
                b';',
            ) => Extension::BeforeName,
            (Extension::Name | Extension::AfterName, b'=') => Extension::BeforeValue,
            (Extension::BeforeValue, b'"') => Extension::QuotedValue,
            (Extension::QuotedValue, b'"') => Extension::BeforeSemicolon,
            (Extension::QuotedValue, b'\\') => Extension::QuotedPair,
            (Extension::Name, _) if whitespace => Extension::AfterName,
            (Extension::TokenValue, _) if whitespace => Extension::BeforeSemicolon,
            (
                state @ (Extension::BeforeSemicolon
                | Extension::BeforeName
                | Extension::AfterName
                | Extension::BeforeValue),
                _,
            ) if whitespace => state,
            (Extension::BeforeName | Extension::Name, _) if is_token_char(c) => Extension::Name,
            (Extension::BeforeValue | Extension::TokenValue, _) if is_token_char(c) => {
                Extension::TokenValue
            }
            (Extension::QuotedValue, _) if is_quoted_text_char(c) => Extension::QuotedValue,
            (Extension::QuotedPair, _) if is_quotable_pair_char(c) => Extension::QuotedValue,
            _ => return malformed_extension(),
        };
        self.after_whitespace = whitespace;
        Ok(())
    }

    /// Handles the CR that ends a size line with extensions. It may only follow a complete
    /// extension, with no whitespace before it.
    fn end_extensions(&mut self) -> Result<(), Malformed> {
        let complete = matches!(
            self.extension,
            Extension::Name | Extension::TokenValue | Extension::BeforeSemicolon
        );
        if !complete || self.after_whitespace {
            return malformed_extension();
        }
        self.state = State::SizeLf;
        Ok(())
    }

    fn start_chunk(&mut self) -> Result<(), Malformed> {
        self.size_digits = 0;
        self.extension_bytes = 0;
        if self.chunk_remaining == 0 {
            // The last chunk. Its trailer section follows.
            match self.options.decoded_length {
                Some(expected) if self.decoded != expected => {
                    return malformed(format!(
                        "The aws-chunked body decoded to {} bytes, but its decoded length is \
                         {expected}.",
                        self.decoded
                    ));
                }
                _ => {}
            }
            self.state = State::Trailer;
        } else {
            match self.options.decoded_length {
                Some(expected) if self.chunk_remaining > expected - self.decoded => {
                    return malformed(format!(
                        "The aws-chunked body decodes to more than its decoded length \
                         ({expected} bytes)."
                    ));
                }
                _ => {}
            }
            self.state = State::Data;
        }
        Ok(())
    }

    /// Buffers the trailer section until the empty line that ends it, then parses it. Returns the
    /// input left after the section.
    fn consume_trailer<'a>(&mut self, input: &'a [u8]) -> Result<&'a [u8], Malformed> {
        let max = self.options.max_trailer_bytes;
        let before = self.trailer_bytes.len();
        let take = input.len().min(max + 1 - before);
        self.trailer_bytes.extend_from_slice(&input[..take]);
        // The section ends at the first empty line: either it starts with one, or a line ending is
        // followed by one. Only the bytes just added, and the three before them, can complete it.
        let bytes = &self.trailer_bytes;
        let section_end = if bytes.starts_with(b"\r\n") {
            Some(2)
        } else {
            bytes[before.saturating_sub(3)..]
                .windows(4)
                .position(|window| window == b"\r\n\r\n")
                .map(|i| before.saturating_sub(3) + i + 4)
        };
        let too_long = || {
            malformed(format!(
                "The aws-chunked trailer is longer than {max} bytes."
            ))
        };
        if let Some(end) = section_end {
            if end > max {
                return too_long();
            }
            self.trailers = self.parse_trailer(&self.trailer_bytes[..end - 2])?;
            self.trailer_bytes = Vec::new();
            self.state = State::Done;
            return Ok(&input[end - before..]);
        }
        if self.trailer_bytes.len() > max {
            return too_long();
        }
        Ok(&input[take..])
    }

    /// Parses `name: value` lines, each ending in CRLF, which is all `section` holds.
    fn parse_trailer(&self, mut section: &[u8]) -> Result<Vec<Field>, Malformed> {
        let mut fields = Vec::new();
        while !section.is_empty() {
            let Some(end) = section.windows(2).position(|pair| pair == b"\r\n") else {
                return malformed("Malformed aws-chunked trailer field.");
            };
            let line = &section[..end];
            section = &section[end + 2..];
            let Some(colon) = line.iter().position(|&c| c == b':') else {
                return malformed("Malformed aws-chunked trailer field.");
            };
            let (name, mut value) = (&line[..colon], &line[colon + 1..]);
            if name.is_empty() || !name.iter().all(|&c| is_token_char(c)) {
                return malformed("Malformed aws-chunked trailer field name.");
            }
            // S3 accepts an LF between a checksum and its CRLF, as some clients send. Other
            // fields are held to HTTP's grammar, which has no bare LF.
            if is_checksum_field(name) {
                value = value.strip_suffix(b"\n").unwrap_or(value);
            }
            let value = trim_whitespace(value);
            if !value
                .iter()
                .all(|&c| c == b'\t' || (c >= 0x20 && c != 0x7f))
            {
                return malformed("Malformed aws-chunked trailer field value.");
            }
            // The name is token characters, which are ASCII.
            let name = String::from_utf8_lossy(name).into_owned();
            // A dropped field is held only to HTTP's grammar. A kept one must also be UTF-8, which
            // JavaScript reads back unchanged, rather than be returned altered.
            if self.is_allowed(&name) {
                let Ok(value) = String::from_utf8(value.to_vec()) else {
                    return malformed("aws-chunked trailer field value is not UTF-8.");
                };
                fields.push(Field { name, value });
            }
        }
        Ok(fields)
    }

    fn is_allowed(&self, name: &str) -> bool {
        self.options
            .allowed_trailers
            .as_ref()
            .is_none_or(|allowed| allowed.iter().any(|entry| entry.eq_ignore_ascii_case(name)))
    }
}

#[cfg(test)]
#[path = "decoder-test.rs"]
mod tests;
