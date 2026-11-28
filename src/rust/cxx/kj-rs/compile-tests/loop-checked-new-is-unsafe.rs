// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `LoopChecked` is `Send` whatever it wraps, so the caller vouches that the value has no affinity
// to its OS thread, which the compiler cannot check.
use std::rc::Rc;

pub fn t(value: Rc<u32>) -> kj_rs::LoopChecked<Rc<u32>> {
    kj_rs::LoopChecked::new(value) //~ ERROR E0133
}
