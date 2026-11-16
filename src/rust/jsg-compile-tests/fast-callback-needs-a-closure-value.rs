// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A fast callback recreates its closure from the closure's type, so making one takes a value of
// that type: a closure that captures an uninhabited type is zero-sized and has no values.
use std::convert::Infallible;

use jsg::fast_api::FastStaticMethod;

fn fast_callback_of<F: FastStaticMethod<()>>(_: Option<F>) -> impl Sized {
    F::descriptor() //~ ERROR E0061
}

pub fn t() -> impl Sized {
    fast_callback_of(None::<Infallible>.map(|never| move || -> bool { match never {} }))
}
