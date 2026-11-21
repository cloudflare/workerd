// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! [`Frankenvalue`], C++ `workerd::Frankenvalue`, for APIs implemented in Rust: a JavaScript
//! value serialized for another isolate, such as the `props` of a stub or the `env` of a dynamic
//! Worker, with the capabilities in it, such as `Fetcher`s, in its cap table.
//!
//! Rust builds one from JavaScript with [`Frankenvalue::from_js`], which calls C++
//! `Frankenvalue::fromJs`, so the value and its capabilities serialize exactly as in C++. It holds
//! it as a `kj_rs::KjOwn<Frankenvalue>`, which it can query, clone, turn back into JavaScript, and
//! pass to C++ that takes a `Frankenvalue` by value, which moves it out with `kj::mv(*value)`.
//!
//! Another bridge that passes or returns a `KjOwn<Frankenvalue>` aliases this crate's type
//! (`type Frankenvalue = frankenvalue::Frankenvalue;` under `#[namespace = "workerd"]`) rather than
//! declaring `workerd::Frankenvalue` itself: the bridge that declares a type defines its
//! `KjOwn` drop, and a binary may link only one.
//!
//! `KjOwn<Frankenvalue>` is neither `Send` nor `Sync` (see [`Frankenvalue`]). Code that needs one
//! after a wait builds it on the `IoContext`'s thread, under the isolate lock, and can't carry it
//! through a `Send` future.
//!
//! The C++ side, `ffi.h` and `ffi.c++`, is the per-type shims of `Frankenvalue`: C++ API code,
//! which lives here with the class rather than in Rust JSG.

mod ffi;

pub use ffi::bridge::Frankenvalue;
