// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Only a byte slice converts to an `ArrayBuffer`, as only `kj::Array<kj::byte>` does in C++ JSG.
// A slice of wider numbers has a typed array, whose `buffer` holds its bytes.
use jsg::ToJsArrayBuffer;
use jsg::ToJsTypedArray;
use jsg::v8;

pub fn t<'s>(lock: &mut jsg::Lock<'s>) {
    let _: v8::Local<'s, v8::ArrayBuffer> = [1u8, 2].to_js_array_buffer(lock);
    let _: v8::Local<'s, v8::ArrayBuffer> = vec![1u8, 2].to_js_array_buffer(lock);
    let _: v8::Local<'s, v8::Uint32Array> = [1u32, 2].to_js_typed_array(lock);
    let _ = [1u32, 2].to_js_array_buffer(lock); //~ ERROR E0599
    let _ = vec![1i8].to_js_array_buffer(lock); //~ ERROR E0599
    let _ = [1.0f64].to_js_array_buffer(lock); //~ ERROR E0599
}
