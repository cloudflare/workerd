// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A hand-written member gets its fast callback from a closure that captures nothing.
pub fn t() -> jsg::Member {
    jsg::Member::StaticMethod {
        name: "isEven".to_owned(),
        callback: jsg::function_callback(|_lock, _args| {}),
        fast: Some(jsg::fast_api::static_method(|value: u32| value % 2 == 0)),
        length: 1,
    }
}
