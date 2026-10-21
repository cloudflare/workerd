// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// rustc borrow-checks only code that type-checks, so borrow errors need a snippet (or at least a
// function) without type errors. Some of them have no code and are matched by message.

pub fn use_after_move(v: Vec<u8>) -> (Vec<u8>, Vec<u8>) {
    (v, v) //~ ERROR E0382
}

pub fn shorten<'a, 'b>(a: &'a str) -> &'b str {
    a //~ ERROR lifetime may not live long enough
}

pub fn anywhere() -> &'static str {
    let s = String::new();
    &s
}
//~? ERROR E0515
