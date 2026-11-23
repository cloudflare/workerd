//! Parsing a MIME type, and extracting one from a `Content-Type` header value.

use std::convert::Infallible;

use crate::find;
use crate::is_param_value;
use crate::is_token;
use crate::skip_leading_whitespace;
use crate::trim_trailing_whitespace;

/// The type and subtype of a MIME type, as written in the input: valid, but not lowercased.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Essence<'a> {
    pub type_: &'a [u8],
    pub subtype: &'a [u8],
}

impl Essence<'_> {
    /// Whether two essences are the same once lowercased.
    #[must_use]
    pub fn eq_ignore_case(&self, other: &Essence<'_>) -> bool {
        self.type_.eq_ignore_ascii_case(other.type_)
            && self.subtype.eq_ignore_ascii_case(other.subtype)
    }

    fn is_wildcard(&self) -> bool {
        self.type_ == b"*" && self.subtype == b"*"
    }
}

/// Parses the type and subtype at the start of `input`. Returns them and the text of the
/// parameters, which follows the first `;`, or `None` if `input` is not a MIME type.
#[must_use]
pub fn parse_essence(input: &[u8]) -> Option<(Essence<'_>, &[u8])> {
    let input = skip_leading_whitespace(input);
    let slash = find(input, b'/')?;
    let type_ = &input[..slash];
    if !is_token(type_) {
        return None;
    }
    let input = &input[slash + 1..];
    let (subtype, params) = match find(input, b';') {
        Some(semicolon) => (&input[..semicolon], &input[semicolon + 1..]),
        None => (input, &input[input.len()..]),
    };
    let subtype = trim_trailing_whitespace(subtype);
    if !is_token(subtype) {
        return None;
    }
    Some((Essence { type_, subtype }, params))
}

/// Parses the parameters that follow a MIME type's essence.
///
/// Passes each valid one to `param` in the order they are written: its name as written and its
/// value with quotes and escapes removed. Invalid parameters are skipped. A name may repeat, in
/// any case.
///
/// `scratch` holds a value whose escapes have been removed; it is reused across parameters.
///
/// # Errors
///
/// Stops at, and returns, the first error `param` returns.
pub fn parse_params<E>(
    mut input: &[u8],
    scratch: &mut Vec<u8>,
    mut param: impl FnMut(&[u8], &[u8]) -> Result<(), E>,
) -> Result<(), E> {
    loop {
        input = skip_leading_whitespace(input);
        let Some(delimiter) = input.iter().position(|&c| c == b';' || c == b'=') else {
            // Text with neither is not a parameter, and ends the list.
            return Ok(());
        };
        if input[delimiter] == b';' {
            // A parameter without a value.
            input = &input[delimiter + 1..];
            continue;
        }
        let name = &input[..delimiter];
        input = &input[delimiter + 1..];
        if !is_token(name) {
            let Some(semicolon) = find(input, b';') else {
                return Ok(());
            };
            input = &input[semicolon + 1..];
            continue;
        }
        if input.is_empty() {
            return Ok(());
        }

        if input[0] == b'"' {
            let (value, rest) = collect_quoted_string(&input[1..], scratch);
            if is_param_value(value) {
                param(name, value)?;
            }
            // Anything between the closing quote and the next `;` is ignored.
            let Some(semicolon) = find(rest, b';') else {
                return Ok(());
            };
            input = &rest[semicolon + 1..];
        } else {
            let (value, rest) = match find(input, b';') {
                Some(semicolon) => (&input[..semicolon], Some(&input[semicolon + 1..])),
                None => (input, None),
            };
            let value = trim_trailing_whitespace(value);
            if !value.is_empty() && is_param_value(value) {
                param(name, value)?;
            }
            let Some(rest) = rest else {
                return Ok(());
            };
            input = rest;
        }
    }
}

/// Collects an HTTP quoted string (Fetch §2.6) with its value extracted, from just after its
/// opening quote. Returns the value and the input after the closing quote. A `\` escapes the
/// byte after it, and one at the end of the input stands for itself. A string without a closing
/// quote runs to the end of the input.
fn collect_quoted_string<'i: 'v, 's: 'v, 'v>(
    input: &'i [u8],
    scratch: &'s mut Vec<u8>,
) -> (&'v [u8], &'i [u8]) {
    let Some(special) = input.iter().position(|&c| c == b'"' || c == b'\\') else {
        return (input, &input[input.len()..]);
    };
    if input[special] == b'"' {
        return (&input[..special], &input[special + 1..]);
    }

    scratch.clear();
    scratch.extend_from_slice(&input[..special]);
    let mut rest = &input[special..];
    loop {
        match rest {
            [] => break,
            [b'"', tail @ ..] => {
                rest = tail;
                break;
            }
            [b'\\'] => {
                scratch.push(b'\\');
                rest = &[];
                break;
            }
            [b'\\', escaped, tail @ ..] | [escaped, tail @ ..] => {
                scratch.push(*escaped);
                rest = tail;
            }
        }
    }
    (scratch.as_slice(), rest)
}

/// Whether `name` is the `charset` parameter's name, in any case.
fn is_charset(name: &[u8]) -> bool {
    name.eq_ignore_ascii_case(b"charset")
}

/// Whether the parameters in `params` include a valid `charset`.
fn has_charset(params: &[u8], scratch: &mut Vec<u8>) -> bool {
    let mut found = false;
    let Ok(()) = parse_params::<Infallible>(params, scratch, |name, _| {
        found |= is_charset(name);
        Ok(())
    });
    found
}

/// A MIME type that `extract` has accepted.
#[derive(Clone, Copy)]
struct Candidate<'a> {
    essence: Essence<'a>,
    params: &'a [u8],
    has_charset: bool,
    /// The parameters its `charset` comes from: its own, or those of the latest of the MIME
    /// types of the same essence directly before it that has one.
    charset_from: Option<&'a [u8]>,
}

/// Extracts a MIME type from a `Content-Type` header value that may combine several.
///
/// This is the Fetch standard's "extract a MIME type": the last valid MIME type wins, `*/*`
/// aside, and when it has no `charset` it inherits one from the MIME types of the same essence
/// that directly precede it. Passes the winner's parameters to `param` as `parse_params` does,
/// followed by any inherited `charset`, and returns its essence.
///
/// Values are split at commas outside quotes. A `"` or `,` that follows a `\` is not special.
///
/// # Errors
///
/// Stops at, and returns, the first error `param` returns.
pub fn extract<'a, E>(
    mut input: &'a [u8],
    scratch: &mut Vec<u8>,
    mut param: impl FnMut(&[u8], &[u8]) -> Result<(), E>,
) -> Result<Option<Essence<'a>>, E> {
    let mut current: Option<Candidate<'a>> = None;
    while !input.is_empty() {
        let (part, rest) = match find_next_separator(input) {
            Some(comma) => (&input[..comma], Some(&input[comma + 1..])),
            None => (input, None),
        };
        if let Some((essence, params)) = parse_essence(part).filter(|(e, _)| !e.is_wildcard()) {
            let inherited = current
                .filter(|c| c.essence.eq_ignore_case(&essence))
                .and_then(|c| c.charset_from);
            if rest.is_none() {
                // The last part wins outright, so its parameters go straight to `param`.
                let mut own_charset = false;
                parse_params(params, scratch, |name, value| {
                    own_charset |= is_charset(name);
                    param(name, value)
                })?;
                if !own_charset && let Some(from) = inherited {
                    emit_charset(from, scratch, &mut param)?;
                }
                return Ok(Some(essence));
            }
            let has_charset = has_charset(params, scratch);
            current = Some(Candidate {
                essence,
                params,
                has_charset,
                charset_from: if has_charset { Some(params) } else { inherited },
            });
        }
        match rest {
            Some(rest) => input = rest,
            None => break,
        }
    }

    let Some(winner) = current else {
        return Ok(None);
    };
    parse_params(winner.params, scratch, &mut param)?;
    if !winner.has_charset
        && let Some(from) = winner.charset_from
    {
        emit_charset(from, scratch, &mut param)?;
    }
    Ok(Some(winner.essence))
}

/// Passes the first valid `charset` in `params` to `param`, under the name `charset`.
fn emit_charset<E>(
    params: &[u8],
    scratch: &mut Vec<u8>,
    param: &mut impl FnMut(&[u8], &[u8]) -> Result<(), E>,
) -> Result<(), E> {
    let mut done = false;
    parse_params(params, scratch, |name, value| {
        if !done && is_charset(name) {
            done = true;
            return param(b"charset", value);
        }
        Ok(())
    })
}

/// Finds the next comma that is not inside quotes and does not follow a `\`.
fn find_next_separator(input: &[u8]) -> Option<usize> {
    let mut i = 0;
    while i < input.len() {
        let escaped = i > 0 && input[i - 1] == b'\\';
        if input[i] == b'"' && !escaped {
            // Skip to the closing quote: a `"` that does not follow a `\`.
            i += 1;
            while i < input.len() && (input[i] != b'"' || input[i - 1] == b'\\') {
                i += 1;
            }
        } else if input[i] == b',' && !escaped {
            return Some(i);
        }
        i += 1;
    }
    None
}

#[cfg(test)]
#[path = "parse-test.rs"]
mod tests;
