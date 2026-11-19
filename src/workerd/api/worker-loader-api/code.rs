// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The types of a module object's `data` and `wasm` fields, which C++ declares as
//! `kj::Array<const byte>` and `kj::OneOf<kj::Array<const byte>, jsg::V8Ref<v8::WasmModuleObject>>`.
//! Rust JSG has no type that takes an `ArrayBuffer` or an `ArrayBufferView` as bytes, so these
//! convert as C++ JSG converts those types, with the names C++ JSG gives them in its errors.

use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;

/// The bytes of an `ArrayBuffer` or an `ArrayBufferView`, copied out of the V8 backing store,
/// which a resizable or transferable buffer can lose before the Worker is compiled.
fn bytes_of(value: v8::Local<'_, v8::Value>) -> Option<Vec<u8>> {
    if let Ok(buffer) = v8::Local::<v8::ArrayBuffer>::try_from(value) {
        return Some(buffer.to_vec());
    }
    if let Ok(view) = v8::Local::<v8::ArrayBufferView>::try_from(value) {
        return Some(view.to_vec());
    }
    None
}

/// A `data` module's body: the bytes of an `ArrayBuffer` or an `ArrayBufferView`, which it
/// imports as an `ArrayBuffer`.
pub struct Bytes(pub Vec<u8>);

jsg::jsg_traced_unchecked!(Bytes);

impl jsg::TypeInfo for Bytes {
    fn describe(cx: &mut jsg::rtti::Context) -> jsg::rtti::Type {
        <v8::ArrayBuffer as jsg::TypeInfo>::describe(cx)
    }
}

impl<'s> FromJS<'s> for Bytes {
    type ResultType = Self;

    // Reading a buffer's bytes runs no script.
    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = false;

    // As C++ JSG names `kj::Array<const byte>`.
    fn type_name() -> String {
        "Array".to_owned()
    }

    fn from_js(lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) -> jsg::Result<Self> {
        Self::try_from_js(lock, value)?.ok_or_else(|| {
            jsg::Error::new_type_error(format!("Expected Array but got {}", value.type_of(lock)))
        })
    }

    fn try_from_js(
        _lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<Option<Self>> {
        Ok(bytes_of(value).map(Self))
    }
}

impl<'s> ToJS<'s> for Bytes {
    fn to_js(self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Value>> {
        Ok(v8::ArrayBuffer::new(lock, &self.0).into())
    }
}

/// A `wasm` module's body: bytes to compile, or a `WebAssembly.Module` whose compiled code the
/// Worker shares.
pub enum Wasm {
    Bytes(Vec<u8>),
    Module(v8::TracedGlobal<v8::WasmModuleObject>),
}

impl jsg::Traced for Wasm {
    fn trace(&self, visitor: &mut v8::GcVisitor) {
        if let Self::Module(module) = self {
            module.trace(visitor);
        }
    }
}

impl jsg::TypeInfo for Wasm {
    fn describe(cx: &mut jsg::rtti::Context) -> jsg::rtti::Type {
        <v8::WasmModuleObject as jsg::TypeInfo>::describe(cx)
    }
}

impl<'s> FromJS<'s> for Wasm {
    type ResultType = Self;

    // Reading a buffer's bytes or checking for a module object runs no script.
    const OBJECT_CONVERSION_MAY_RUN_SCRIPT: bool = false;

    // As C++ JSG names `kj::OneOf<kj::Array<const byte>, jsg::V8Ref<v8::WasmModuleObject>>`.
    fn type_name() -> String {
        "Array or WasmModuleObject".to_owned()
    }

    fn from_js(lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) -> jsg::Result<Self> {
        Self::try_from_js(lock, value)?.ok_or_else(|| {
            jsg::Error::new_type_error(format!(
                "Expected Array or WasmModuleObject but got {}",
                value.type_of(lock)
            ))
        })
    }

    fn try_from_js(
        lock: &mut jsg::Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<Option<Self>> {
        if let Ok(module) = v8::Local::<v8::WasmModuleObject>::try_from(value) {
            return Ok(Some(Self::Module(module.to_traced_global(lock))));
        }
        Ok(bytes_of(value).map(Self::Bytes))
    }
}

impl<'s> ToJS<'s> for Wasm {
    fn to_js(self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Value>> {
        match self {
            Self::Bytes(bytes) => Ok(v8::ArrayBuffer::new(lock, &bytes).into()),
            Self::Module(module) => Ok(module.as_local(lock).into()),
        }
    }
}
