// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A callback is called through a plain function pointer, so it cannot capture state.
pub fn t() -> jsg::RawFunctionCallback {
    let name = String::from("captured");
    jsg::function_callback(move |lock, args| {
        //~^ ERROR E0277
        let value = lock.new_object();
        if value.set(lock, &name, args.this()).is_ok() {
            args.set_return_value(value.into());
        }
    })
}
