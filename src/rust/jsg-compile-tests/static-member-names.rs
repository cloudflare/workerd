// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The static methods and static constants of a resource type are properties of its constructor,
// so they cannot be named `length`, `name` or `prototype`, which every function already has, and
// with automatic registration no two of them can share a name.

use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;

#[jsg_resource]
pub struct StaticName;

#[jsg_resource]
impl StaticName {
    #[jsg_method]
    pub fn name() -> String {
        //~^ ERROR static member `name` cannot be named "name"
        String::new()
    }
}

#[jsg_resource]
pub struct RenamedLength;

#[jsg_resource]
impl RenamedLength {
    #[jsg_method(name = "length")]
    pub fn size() -> String {
        //~^ ERROR static member `size` cannot be named "length"
        String::new()
    }
}

#[jsg_resource]
pub struct ConstantPrototype;

#[jsg_resource]
impl ConstantPrototype {
    #[jsg_static_constant(name = "prototype")]
    pub const PROTO: u32 = 1;
    //~^ ERROR static member `PROTO` cannot be named "prototype"
}

#[jsg_resource]
pub struct Duplicate;

#[jsg_resource]
impl Duplicate {
    #[jsg_method]
    pub fn dup() -> String {
        //~^ ERROR `dup` declared here
        String::new()
    }

    #[jsg_static_constant(name = "dup")]
    pub const DUPLICATE: u32 = 1;
    //~^ ERROR static member `DUPLICATE` has the same JavaScript name "dup" as `dup`
}
