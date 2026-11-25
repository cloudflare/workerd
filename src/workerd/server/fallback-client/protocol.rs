// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The fallback service's protocol: the HTTP request for a module, and what its response means.
//!
//! Specifiers, referrers, attributes and redirects are bytes, passed through unchanged as the C++
//! module registries hand them over: a `kj::StringPtr` need not be UTF-8.

use std::fmt::Write as _;

use workerd_capnp::worker::module;

use crate::client::Payload;
use crate::json;
use crate::json::Json;

/// The version of the fallback service protocol; see the crate documentation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Version {
    V1,
    V2,
}

/// How the module being resolved is imported.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImportType {
    /// A static or dynamic `import`.
    Import,
    /// A CommonJS-style `require()`.
    Require,
    /// An import from inside the runtime.
    Internal,
}

impl ImportType {
    const fn as_str(self) -> &'static str {
        match self {
            Self::Import => "import",
            Self::Require => "require",
            Self::Internal => "internal",
        }
    }
}

/// A module to resolve.
#[derive(Clone, Debug)]
pub struct Request {
    pub version: Version,
    pub import_type: ImportType,
    pub specifier: Vec<u8>,
    /// The specifier as written in the importing module, if known.
    pub raw_specifier: Option<Vec<u8>>,
    /// The importing module; empty if there is none.
    pub referrer: Vec<u8>,
    /// The import attributes, as name and value. V1 does not send them.
    pub attributes: Vec<(Vec<u8>, Vec<u8>)>,
}

/// What the fallback service answered.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Resolution {
    /// The service has no such module, or could not be asked; the reason has been logged.
    NotFound,
    /// The specifier of the module to resolve instead: the `Location` of a 301 redirect.
    Redirect(Vec<u8>),
    /// The module, as a `Worker.Module` (workerd.capnp) in a capnp message in the standard
    /// (unpacked) serialization, which `capnp::FlatArrayMessageReader` reads.
    Module(Vec<u8>),
    /// The client's thread has exited, so nothing can be resolved.
    ShutDown,
}

/// The HTTP request for a module.
#[derive(Debug, PartialEq, Eq)]
pub struct HttpRequest {
    pub method: http::Method,
    /// The request target: the path and query.
    pub target: String,
    /// The `x-resolve-method` header, sent in V1 only.
    pub resolve_method: Option<&'static str>,
    /// The body of a `POST`; `None` for a `GET`, which has none.
    pub body: Option<Vec<u8>>,
    /// The module name the response must carry, if it names the module at all.
    pub expected_name: Vec<u8>,
}

impl HttpRequest {
    /// The request, with the headers kj's HTTP client sends: `Host`, `x-resolve-method` in V1,
    /// and `Content-Length` for a `POST` only.
    pub fn to_http(&self) -> Result<http::Request<Payload>, http::Error> {
        let mut builder = http::Request::builder()
            .method(self.method.clone())
            .uri(&self.target)
            .header(http::header::HOST, "localhost");
        if let Some(method) = self.resolve_method {
            builder = builder.header("x-resolve-method", method);
        }
        if let Some(body) = &self.body {
            builder = builder.header(http::header::CONTENT_LENGTH, body.len());
        }
        builder.body(Payload::new(self.body.clone()))
    }

    /// What the C++ client's log lines named the request by: the target in V1, the specifier in
    /// V2.
    pub fn describe(&self) -> String {
        if self.method == http::Method::GET {
            self.target.clone()
        } else {
            String::from_utf8_lossy(&self.expected_name).into_owned()
        }
    }
}

/// The prefixes of the built-in modules. V1 sends only the last segment of a specifier that ends
/// in one (`/bundle/node:fs` asks for `node:fs`).
const BUILTIN_PREFIXES: [&[u8]; 3] = [b"node:", b"cloudflare:", b"workerd:"];

/// The HTTP request that asks the service for `request`.
#[must_use]
pub fn encode(request: &Request) -> HttpRequest {
    match request.version {
        Version::V1 => encode_v1(request),
        Version::V2 => encode_v2(request),
    }
}

fn encode_v1(request: &Request) -> HttpRequest {
    let specifier = request.specifier.as_slice();
    let builtin = specifier
        .iter()
        .rposition(|&c| c == b'/')
        .map(|slash| &specifier[slash + 1..])
        .filter(|segment| BUILTIN_PREFIXES.iter().any(|p| segment.starts_with(p)));
    let (sent, expected_name) = match builtin {
        Some(segment) => (segment, segment),
        None => (specifier, specifier.strip_prefix(b"/").unwrap_or(specifier)),
    };
    let raw_specifier = request.raw_specifier.as_deref().unwrap_or(b"");
    let target = format!(
        "/?specifier={}&referrer={}&rawSpecifier={}",
        www_form(sent),
        www_form(&request.referrer),
        www_form(raw_specifier),
    );
    HttpRequest {
        method: http::Method::GET,
        target,
        resolve_method: Some(request.import_type.as_str()),
        body: None,
        expected_name: expected_name.to_vec(),
    }
}

/// The body is a `FallbackServiceRequest` (workerd.capnp), as `JsonCodec` encodes one: fields in
/// the schema's order, without those whose pointers are null (an absent raw specifier, no
/// attributes), and no whitespace.
fn encode_v2(request: &Request) -> HttpRequest {
    let mut body = Vec::new();
    body.extend_from_slice(b"{\"type\":");
    json::encode_string(request.import_type.as_str().as_bytes(), &mut body);
    body.extend_from_slice(b",\"specifier\":");
    json::encode_string(&request.specifier, &mut body);
    if let Some(raw_specifier) = &request.raw_specifier {
        body.extend_from_slice(b",\"rawSpecifier\":");
        json::encode_string(raw_specifier, &mut body);
    }
    body.extend_from_slice(b",\"referrer\":");
    json::encode_string(&request.referrer, &mut body);
    if !request.attributes.is_empty() {
        body.extend_from_slice(b",\"attributes\":[");
        for (i, (name, value)) in request.attributes.iter().enumerate() {
            if i > 0 {
                body.push(b',');
            }
            body.extend_from_slice(b"{\"name\":");
            json::encode_string(name, &mut body);
            body.extend_from_slice(b",\"value\":");
            json::encode_string(value, &mut body);
            body.push(b'}');
        }
        body.push(b']');
    }
    body.push(b'}');
    HttpRequest {
        method: http::Method::POST,
        target: "/".to_owned(),
        resolve_method: None,
        body: Some(body),
        expected_name: request.specifier.clone(),
    }
}

/// `bytes` as `application/x-www-form-urlencoded`, byte for byte as `kj::encodeWwwForm()` writes
/// it.
fn www_form(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len());
    for &b in bytes {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'_' | b'.' | b'*' => {
                out.push(char::from(b));
            }
            b' ' => out.push('+'),
            _ => {
                let _ = write!(out, "%{b:02X}");
            }
        }
    }
    out
}

/// Why a response is not a module.
#[derive(Debug, PartialEq, Eq)]
pub enum DecodeError {
    /// The body is not a `Worker.Module` in capnp's JSON: what `JsonCodec` would have thrown.
    Invalid(String),
    /// The body names another module than the one asked for.
    NameMismatch { name: Vec<u8> },
}

impl std::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Invalid(message) => f.write_str(message),
            Self::NameMismatch { name } => write!(
                f,
                "returned module name does not match specifier: {}",
                String::from_utf8_lossy(name)
            ),
        }
    }
}

fn invalid(message: impl Into<String>) -> DecodeError {
    DecodeError::Invalid(message.into())
}

/// The module in a 200 response's body, named `expected_name` if the body does not name it.
///
/// The body is decoded as `JsonCodec::decode()` decodes a `Worker.Module` (see [`json`] for the
/// parser): members it does not know are ignored, a `null` member is left unset, `Data` is an
/// array of numbers that are whole and in [0, 255], and of several members of the union the last
/// wins. A body that names a different module is an error.
pub fn decode_module(body: &[u8], expected_name: &[u8]) -> Result<Vec<u8>, DecodeError> {
    let Json::Object(members) = json::parse(body).map_err(DecodeError::Invalid)? else {
        return Err(invalid("Expected object value"));
    };
    let mut message = capnp::message::Builder::new_default();
    let mut module = message.init_root::<module::Builder<'_>>();
    let mut name = None;
    for (key, value) in &members {
        if *value == Json::Null {
            continue;
        }
        match key.as_slice() {
            b"name" => name = Some(text(value)?),
            b"esModule" => module.set_es_module(text(value)?),
            b"commonJsModule" => module.set_common_js_module(text(value)?),
            b"text" => module.set_text(text(value)?),
            b"data" => module.set_data(&data(value)?),
            b"wasm" => module.set_wasm(&data(value)?),
            b"json" => module.set_json(text(value)?),
            b"obsolete" => module.set_obsolete(text(value)?),
            b"pythonModule" => module.set_python_module(text(value)?),
            b"obsoletePythonRequirement" => module.set_obsolete_python_requirement(text(value)?),
            b"bundled" => module.set_bundled(text(value)?),
            b"namedExports" => {
                let Json::Array(names) = value else {
                    return Err(invalid("Expected list value"));
                };
                let mut list = module.reborrow().init_named_exports(len(names.len())?);
                for (i, export) in names.iter().enumerate() {
                    list.set(len(i)?, text(export)?);
                }
            }
            _ => {}
        }
    }
    match name {
        Some(name) if name.as_bytes() != expected_name => {
            return Err(DecodeError::NameMismatch {
                name: name.as_bytes().to_vec(),
            });
        }
        Some(name) => module.set_name(name),
        None => module.set_name(capnp::text::Reader::from(expected_name)),
    }
    Ok(capnp::serialize::write_message_to_words(&message))
}

fn text(value: &Json) -> Result<capnp::text::Reader<'_>, DecodeError> {
    match value {
        Json::String(s) => Ok(capnp::text::Reader::from(s.as_slice())),
        _ => Err(invalid("Expected text value")),
    }
}

#[expect(
    clippy::cast_sign_loss,
    clippy::cast_possible_truncation,
    reason = "each value is checked to be a whole number in [0, 255] before the cast"
)]
fn data(value: &Json) -> Result<Vec<u8>, DecodeError> {
    let Json::Array(bytes) = value else {
        return Err(invalid("Expected data value"));
    };
    bytes
        .iter()
        .map(|byte| match byte {
            Json::Number(x) if x.fract() == 0.0 && (0.0..=255.0).contains(x) => Ok(*x as u8),
            Json::Number(_) => Err(invalid(
                "Number in byte array is not an integer in [0, 255]",
            )),
            // `JsonValue::getNumber()` on a value of another kind.
            _ => Err(invalid("Expected number value")),
        })
        .collect()
}

fn len(n: usize) -> Result<u32, DecodeError> {
    u32::try_from(n).map_err(|_| invalid("list too long"))
}

#[cfg(test)]
#[path = "protocol-test.rs"]
mod tests;
