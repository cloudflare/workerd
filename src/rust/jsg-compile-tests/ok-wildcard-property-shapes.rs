// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A wildcard property method may take a lock, and may return an error of any type that converts
// into `jsg::Error`.

use jsg_macros::jsg_resource;
use jsg_macros::jsg_wildcard_property;

pub struct LookupError;

impl From<LookupError> for jsg::Error {
    fn from(_: LookupError) -> Self {
        Self::new_type_error("lookup failed")
    }
}

#[jsg_resource]
pub struct WithLock;

#[jsg_resource]
impl WithLock {
    #[jsg_wildcard_property]
    pub fn get(&self, lock: &mut jsg::Lock, name: String) -> Result<Option<String>, LookupError> {
        let _ = lock;
        if name.is_empty() {
            Err(LookupError)
        } else {
            Ok(Some(name))
        }
    }
}

#[jsg_resource]
pub struct Borrowed;

#[jsg_resource]
impl Borrowed {
    #[jsg_wildcard_property]
    pub fn get<'a>(self: &'a Self, name: &str) -> Option<f64> {
        let _ = (self, name);
        None
    }
}
