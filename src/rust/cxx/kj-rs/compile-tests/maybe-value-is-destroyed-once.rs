// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A `KjMaybe` destroys the value it holds when it is dropped, and nothing else can: destroying
// the value earlier would leave a `KjMaybe` that still says it holds one.
use kj_rs::KjMaybe;
use kj_rs::maybe::MaybeItem;

pub fn t() {
    let mut maybe = KjMaybe::Some(String::from("value"));
    <String as MaybeItem>::drop_in_place(&mut maybe); //~ ERROR E0576
}
