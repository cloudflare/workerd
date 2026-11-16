// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A V8 marker type names a kind of handle and has no value of its own to carry, so a typed promise
// of one does not exist; a promise of a JavaScript value is a `jsg::Promise<v8::Value>`.
use jsg::v8;

pub fn t(_promise: jsg::Promise<'_, v8::Uint8Array>) {}
//~^ ERROR E0277
