// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::v8;

// The closure cannot run JavaScript or allocate, which could move the characters it reads.
pub fn t<'s>(lock: &mut jsg::Lock<'s>, string: v8::Local<'s, v8::String>) {
    string.with_value_view(lock, |_| {
        //~^ ERROR E0500
        //~| ERROR E0501
        lock.new_object();
    });
}
