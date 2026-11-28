// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The status of a span, `tracing::SpanStatus`.

pub use trace_capnp::SpanStatusCode;
use trace_capnp::span_status;

/// A span's status. Only an error has a message; any other code drops the one it is given, as
/// C++ does. The message is bytes, since a `kj::String` need not be UTF-8.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SpanStatus {
    code: SpanStatusCode,
    message: Option<Vec<u8>>,
}

impl SpanStatus {
    pub fn new(code: SpanStatusCode, message: Option<Vec<u8>>) -> Self {
        Self {
            code,
            message: message.filter(|_| code == SpanStatusCode::Error),
        }
    }

    pub fn code(&self) -> SpanStatusCode {
        self.code
    }

    pub fn message(&self) -> Option<&[u8]> {
        self.message.as_deref()
    }

    /// The bytes of the message, which is what C++'s `size()` counts.
    pub fn size(&self) -> usize {
        self.message.as_ref().map_or(0, Vec::len)
    }

    pub fn from_capnp(reader: span_status::Reader<'_>) -> capnp::Result<Self> {
        // An unknown code reads as unset, as in C++.
        let code = reader.get_code().unwrap_or(SpanStatusCode::Unset);
        let message = match reader.get_message().which() {
            Ok(span_status::message::Text(text)) if code == SpanStatusCode::Error => {
                Some(text?.as_bytes().to_vec())
            }
            _ => None,
        };
        Ok(Self { code, message })
    }

    pub fn to_capnp(&self, mut builder: span_status::Builder<'_>) {
        builder.set_code(self.code);
        let mut message = builder.init_message();
        match &self.message {
            Some(text) => message.set_text(capnp::text::Reader::from(text.as_slice())),
            None => message.set_none(()),
        }
    }
}

#[cfg(test)]
#[path = "status-test.rs"]
mod tests;
