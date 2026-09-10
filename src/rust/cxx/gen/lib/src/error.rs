// We can expose more detail on the error as the need arises, but start with an
// opaque error type for now.

use std::error::Error as StdError;
use std::fmt::Debug;
use std::fmt::Display;
use std::fmt::{self};
use std::iter;

/// An error produced while generating C++ bridge code.
pub struct Error {
    pub(crate) err: r#gen::Error,
}

impl Error {
    /// Returns the span of the error, if available.
    pub fn span(&self) -> Option<proc_macro2::Span> {
        match &self.err {
            r#gen::Error::Syn(err) => Some(err.span()),
            _ => None,
        }
    }
}

impl From<r#gen::Error> for Error {
    fn from(err: r#gen::Error) -> Self {
        Self { err }
    }
}

impl Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        Display::fmt(&self.err, f)
    }
}

impl Debug for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        Debug::fmt(&self.err, f)
    }
}

impl StdError for Error {
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        self.err.source()
    }
}

impl IntoIterator for Error {
    type Item = Self;
    type IntoIter = IntoIter;

    fn into_iter(self) -> Self::IntoIter {
        match self.err {
            r#gen::Error::Syn(err) => IntoIter::Syn(err.into_iter()),
            _ => IntoIter::Other(iter::once(self)),
        }
    }
}

pub enum IntoIter {
    Syn(<syn::Error as IntoIterator>::IntoIter),
    Other(iter::Once<Error>),
}

impl Iterator for IntoIter {
    type Item = Error;

    fn next(&mut self) -> Option<Self::Item> {
        match self {
            Self::Syn(iter) => iter
                .next()
                .map(|syn_err| Error::from(r#gen::Error::Syn(syn_err))),
            Self::Other(iter) => iter.next(),
        }
    }
}
