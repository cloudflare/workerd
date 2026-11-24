// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Handles of the enclosing scope can be used in `with_handle_scope`, with the new scope's lock,
// and the closure can return what holds no handle of the new scope, including handles of the
// enclosing one. With `with_escapable_handle_scope`, it can return a handle the new scope
// escaped, in a `Result`.
use jsg::v8;

pub fn t<'s>(
    lock: &mut jsg::Lock<'s>,
    f: v8::Local<'s, v8::Function>,
) -> jsg::Result<v8::Local<'s, v8::Function>> {
    let calls = lock.with_handle_scope(|lock, _| -> jsg::Result<u32> {
        f.call(lock, None, &[])?;
        Ok(1)
    })?;
    let _ = calls;
    Ok(lock.with_handle_scope(|_, _| f))
}

pub fn escaped<'s>(lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Object>> {
    lock.with_escapable_handle_scope(|lock, scope| -> jsg::Result<_> {
        let object = lock.new_object();
        let value = v8::Number::new(lock, 1.0).into();
        object.set(lock, "value", value)?;
        Ok(scope.escape(object))
    })
}
