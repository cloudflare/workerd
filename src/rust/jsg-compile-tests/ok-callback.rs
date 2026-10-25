// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A function callback reads its arguments and sets its return value in its own scope, without
// `unsafe`.
pub fn t() -> jsg::RawFunctionCallback {
    jsg::function_callback(|lock, args| {
        let object = lock.new_object();
        if object.set(lock, "this", args.this()).is_ok() {
            args.set_return_value(object.into());
        }
    })
}
