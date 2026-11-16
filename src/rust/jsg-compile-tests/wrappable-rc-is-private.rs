// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `WrappableRc` changes a `Wrappable`'s `kj::Rc` count, which only the isolate lock guards, when
// it is cloned or dropped, so only `jsg::Rc` and `jsg::Weak`, which take the lock, hold one.
use jsg::v8::WrappableRc; //~ ERROR E0603
