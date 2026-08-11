use std::fmt::Display;

use quote::ToTokens;
use syn::Error;
use syn::Result;

pub struct Errors {
    errors: Vec<Error>,
}

impl Errors {
    pub fn new() -> Self {
        Self { errors: Vec::new() }
    }

    pub fn error(&mut self, sp: impl ToTokens, msg: impl Display) {
        self.errors.push(Error::new_spanned(sp, msg));
    }

    pub fn push(&mut self, error: Error) {
        self.errors.push(error);
    }

    pub fn propagate(&mut self) -> Result<()> {
        let mut iter = self.errors.drain(..);
        let Some(mut all_errors) = iter.next() else {
            return Ok(());
        };
        for err in iter {
            all_errors.combine(err);
        }
        Err(all_errors)
    }
}
