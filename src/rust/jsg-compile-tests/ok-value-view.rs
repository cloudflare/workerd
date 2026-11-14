// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::v8;

pub fn t<'s>(lock: &mut jsg::Lock<'s>, string: v8::Local<'s, v8::String>) -> usize {
    let length = string.with_value_view(lock, |view| match view {
        v8::StringView::OneByte(characters) => characters.len(),
        v8::StringView::TwoByte(characters) => characters.len(),
    });
    lock.new_object();
    length
}
