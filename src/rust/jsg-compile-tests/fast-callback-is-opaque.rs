// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// V8 calls a fast callback with the C signature registered for it, so a member takes only a
// callback and signature that `jsg::fast_api` made from one closure, and neither can be changed.
use jsg::fast_api::FastApiType;
use jsg::v8::ffi::FastApiDescriptor;

pub fn handwritten() -> jsg::Member {
    jsg::Member::StaticMethod {
        name: "f".to_owned(),
        callback: jsg::function_callback(|_lock, _args| {}),
        fast: Some(FastApiDescriptor {
            //~^ ERROR E0308
            callback: 0,
            return_type: FastApiType::Bool,
            arg_types: vec![FastApiType::Uint32],
        }),
        length: 1,
    }
}

pub fn changed() -> impl Sized {
    let mut fast = jsg::fast_api::static_method(|value: bool| value);
    fast.arg_types[0] = FastApiType::Uint32; //~ ERROR E0609
    fast
}
