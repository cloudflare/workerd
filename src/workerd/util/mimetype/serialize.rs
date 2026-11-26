//! Serializing a MIME type, as the MIME Sniffing standard does (§4.6), with one addition: a
//! `boundary` parameter is preceded by a space, as browsers write `multipart/form-data` and as
//! Web Platform Tests expect (<https://github.com/web-platform-tests/wpt/pull/29554>).
//!
//! The caller asks for the length first and provides a buffer of exactly that length, so that
//! the result needs one allocation and no copy.

use crate::is_token;

/// A parameter as the caller stores it: its name lowercased, its value without quotes or
/// escapes.
pub trait Param {
    fn name(&self) -> &[u8];
    fn value(&self) -> &[u8];
}

/// The length of `type/subtype` followed by `;` and the parameters, if there are any.
#[must_use]
pub fn serialized_len<P: Param>(type_: &[u8], subtype: &[u8], params: &[P]) -> usize {
    let essence = type_.len() + 1 + subtype.len();
    if params.is_empty() {
        essence
    } else {
        essence + 1 + params_serialized_len(params)
    }
}

/// Writes `type/subtype` followed by `;` and the parameters, if there are any, to `out`.
///
/// `out`'s length is `serialized_len()` of the same arguments. Returns the length written, which
/// is `out.len()` unless the lengths disagree; what does not fit is dropped.
#[must_use]
pub fn serialize<P: Param>(type_: &[u8], subtype: &[u8], params: &[P], out: &mut [u8]) -> usize {
    let mut out = Writer { out, pos: 0 };
    out.put(type_);
    out.put(b"/");
    out.put(subtype);
    if !params.is_empty() {
        out.put(b";");
        write_params(params, &mut out);
    }
    out.pos
}

/// The length of the parameters, separated by `;`.
#[must_use]
pub fn params_serialized_len<P: Param>(params: &[P]) -> usize {
    let separators = params.len().saturating_sub(1);
    params.iter().fold(separators, |len, param| {
        let name = param.name();
        len + usize::from(is_boundary(name)) + name.len() + 1 + value_len(param.value())
    })
}

/// Writes the parameters, separated by `;`, to `out`.
///
/// `out`'s length is `params_serialized_len()` of the same parameters. Returns the length
/// written, as `serialize` does.
#[must_use]
pub fn serialize_params<P: Param>(params: &[P], out: &mut [u8]) -> usize {
    let mut out = Writer { out, pos: 0 };
    write_params(params, &mut out);
    out.pos
}

fn is_boundary(name: &[u8]) -> bool {
    name == b"boundary"
}

fn needs_escape(c: u8) -> bool {
    c == b'"' || c == b'\\'
}

/// The length of a value as written: as is if it is a token, otherwise quoted, with `"` and `\`
/// escaped.
fn value_len(value: &[u8]) -> usize {
    if is_token(value) {
        value.len()
    } else {
        2 + value.len() + value.iter().filter(|&&c| needs_escape(c)).count()
    }
}

fn write_params<P: Param>(params: &[P], out: &mut Writer<'_>) {
    for (i, param) in params.iter().enumerate() {
        if i > 0 {
            out.put(b";");
        }
        let name = param.name();
        if is_boundary(name) {
            out.put(b" ");
        }
        out.put(name);
        out.put(b"=");
        let value = param.value();
        if is_token(value) {
            out.put(value);
            continue;
        }
        out.put(b"\"");
        let mut rest = value;
        while let Some(i) = rest.iter().position(|&c| needs_escape(c)) {
            out.put(&rest[..i]);
            out.put(b"\\");
            out.put(&rest[i..=i]);
            rest = &rest[i + 1..];
        }
        out.put(rest);
        out.put(b"\"");
    }
}

/// Writes to a buffer of a precomputed length. Bytes past its end are dropped rather than
/// panicking.
struct Writer<'a> {
    out: &'a mut [u8],
    pos: usize,
}

impl Writer<'_> {
    fn put(&mut self, bytes: &[u8]) {
        let end = self.pos + bytes.len();
        if let Some(dst) = self.out.get_mut(self.pos..end) {
            dst.copy_from_slice(bytes);
        }
        self.pos = end;
    }
}

#[cfg(test)]
#[path = "serialize-test.rs"]
mod tests;
