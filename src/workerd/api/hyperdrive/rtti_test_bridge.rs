// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Rust half of `rtti-test.c++`: the RTTI that the Rust `Hyperdrive` exports, for the test to
//! compare with the C++ `Hyperdrive`'s.

#[cxx::bridge(namespace = "workerd::api::hyperdrive_test")]
mod bridge {
    extern "Rust" {
        /// A serialized `StructureGroups` with one group, which holds the Rust `Hyperdrive` and
        /// every Rust structure it refers to, described with every compatibility flag false.
        fn hyperdrive_rtti() -> Vec<u8>;
    }
}

fn hyperdrive_rtti() -> Vec<u8> {
    let mut cx = jsg::rtti::Context::default();
    <hyperdrive::Hyperdrive as jsg::TypeInfo>::describe(&mut cx);
    cx.encode("hyperdrive")
}
