// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A resource type has at most one wildcard property, which takes `&self`, optionally a lock,
// and the property name, and returns `Option<T>` or `Result<Option<T>, E>`.

use jsg_macros::jsg_resource;
use jsg_macros::jsg_wildcard_property;

#[jsg_resource]
pub struct Twice;

#[jsg_resource]
impl Twice {
    #[jsg_wildcard_property]
    pub fn first(&self, name: String) -> Option<String> {
        Some(name)
    }

    #[jsg_wildcard_property]
    pub fn second(&self, name: String) -> Option<String> {
        //~^ ERROR a resource type has at most one #[jsg_wildcard_property]
        Some(name)
    }
}

#[jsg_resource]
pub struct Shapes;

#[jsg_resource]
impl Shapes {
    #[jsg_wildcard_property]
    pub fn mutable(&mut self, name: String) -> Option<String> {
        //~^ ERROR a #[jsg_wildcard_property] method takes `&self`
        Some(name)
    }
}

#[jsg_resource]
pub struct NoName;

#[jsg_resource]
impl NoName {
    #[jsg_wildcard_property]
    pub fn get(&self) -> Option<String> {
        //~^ ERROR the property name as a `String` or `&str`
        None
    }
}

#[jsg_resource]
pub struct NotOptional;

#[jsg_resource]
impl NotOptional {
    #[jsg_wildcard_property]
    pub fn get(&self, name: String) -> String {
        //~^ ERROR returns `Option<T>` or `Result<Option<T>, E>`
        name
    }
}

#[jsg_resource]
pub struct ByValue;

#[jsg_resource]
impl ByValue {
    #[jsg_wildcard_property]
    pub fn get(self, name: String) -> Option<String> {
        //~^ ERROR a #[jsg_wildcard_property] method takes `&self`
        Some(name)
    }
}

#[jsg_resource]
pub struct Static;

#[jsg_resource]
impl Static {
    #[jsg_wildcard_property]
    pub fn get(name: String) -> Option<String> {
        //~^ ERROR a #[jsg_wildcard_property] method takes `&self`
        Some(name)
    }
}

#[jsg_resource]
pub struct BorrowedString;

#[jsg_resource]
impl BorrowedString {
    #[jsg_wildcard_property]
    pub fn get(&self, name: &String) -> Option<String> {
        //~^ ERROR the property name as a `String` or `&str`
        Some(name.clone())
    }
}

#[jsg_resource]
pub struct ExtraParameter;

#[jsg_resource]
impl ExtraParameter {
    #[jsg_wildcard_property]
    pub fn get(&self, name: String, extra: String) -> Option<String> {
        //~^ ERROR the property name as a `String` or `&str`
        Some(name + &extra)
    }
}

#[jsg_resource]
pub struct Arguments;

#[jsg_resource]
impl Arguments {
    #[jsg_wildcard_property(name = "x")]
    //~^ ERROR #[jsg_wildcard_property] takes no arguments
    pub fn get(&self, name: String) -> Option<String> {
        Some(name)
    }
}
