// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The argument of `writeDataPoint()`, converted from JavaScript as C++ JSG converts the
//! `AnalyticsEngine::AnalyticsEngineEvent` struct in `analytics-engine.h`.

use jsg::FromJS;
use jsg::Lock;
use jsg::Nullable;
use jsg::Type;
use jsg::TypeInfo;
use jsg::rtti;
use jsg::v8;

/// A data point as JavaScript describes it, before its limits are checked.
#[derive(Debug, Default, PartialEq)]
pub struct DataPoint {
    pub indexes: Option<Vec<Nullable<Bytes>>>,
    pub doubles: Option<Vec<f64>>,
    pub blobs: Option<Vec<Nullable<Bytes>>>,
}

const NAME: &str = "AnalyticsEngineEvent";

// Describes the data point as a C++ JSG_STRUCT does: by the JavaScript names of its fields, in
// declaration order.
impl TypeInfo for DataPoint {
    fn describe(cx: &mut rtti::Context) -> rtti::Type {
        cx.structure::<Self>(NAME, rtti::TypeScript::default(), |cx, _| {
            vec![
                property("indexes", <Option<Vec<Nullable<Bytes>>>>::describe(cx)),
                property("doubles", <Option<Vec<f64>>>::describe(cx)),
                property("blobs", <Option<Vec<Nullable<Bytes>>>>::describe(cx)),
            ]
        })
    }
}

fn property(name: &'static str, ty: rtti::Type) -> rtti::Member {
    rtti::Member::Property {
        name,
        ty,
        readonly: false,
        prototype: false,
        lazy: false,
    }
}

impl Type for DataPoint {
    fn class_name() -> &'static str {
        NAME
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_object()
    }
}

// Like a C++ JSG_STRUCT: null and undefined are an empty data point, other non-objects are not
// one, and each field is read and converted in declaration order.
impl<'s> FromJS<'s> for DataPoint {
    type ResultType = Self;

    fn type_name() -> String {
        NAME.to_owned()
    }

    fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> jsg::Result<Self> {
        let type_name = value.type_of(lock);
        Self::try_from_js(lock, value)?.ok_or_else(|| {
            jsg::Error::new_type_error(format!("Expected {NAME} but got {type_name}"))
        })
    }

    fn try_from_js(
        lock: &mut Lock<'s>,
        value: v8::Local<'s, v8::Value>,
    ) -> jsg::Result<Option<Self>> {
        if value.is_null_or_undefined() {
            return Ok(Some(Self::default()));
        }
        let Ok(object) = v8::Local::<v8::Object>::try_from(value) else {
            return Ok(None);
        };
        Ok(Some(Self {
            indexes: field::<Option<Vec<Nullable<Bytes>>>>(lock, object, "indexes")?,
            doubles: field::<Option<Vec<f64>>>(lock, object, "doubles")?,
            blobs: field::<Option<Vec<Nullable<Bytes>>>>(lock, object, "blobs")?,
        }))
    }
}

/// Reads field `name` of `object` as a `T`, with C++ JSG's error for a value of the wrong type.
fn field<'s, T: FromJS<'s>>(
    lock: &mut Lock<'s>,
    object: v8::Local<'s, v8::Object>,
    name: &str,
) -> jsg::Result<T::ResultType> {
    let value = object.get(lock, name)?;
    T::try_from_js(lock, value)?.ok_or_else(|| {
        jsg::Error::new_type_error(format!(
            "Incorrect type for the '{name}' field on '{NAME}': the provided value is not of type \
             '{}'.",
            T::type_name()
        ))
    })
}

/// An index or blob: the bytes of a buffer source, or of any other value as a UTF-8 string, like
/// a C++ `kj::OneOf<kj::Array<kj::byte>, kj::String>`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Bytes(pub Vec<u8>);

impl Type for Bytes {
    fn class_name() -> &'static str {
        "BufferSource or string"
    }

    fn is_exact(value: v8::Local<'_, v8::Value>) -> bool {
        value.is_array_buffer() || value.is_array_buffer_view() || value.is_string()
    }
}

impl TypeInfo for Bytes {
    fn describe(_: &mut rtti::Context) -> rtti::Type {
        rtti::Type::OneOf(vec![
            rtti::Type::Builtin(rtti::Builtin::JsgBufferSource),
            rtti::Type::String,
        ])
    }
}

impl<'s> FromJS<'s> for Bytes {
    type ResultType = Self;

    fn type_name() -> String {
        Self::class_name().to_owned()
    }

    fn from_js(lock: &mut Lock<'s>, value: v8::Local<'s, v8::Value>) -> jsg::Result<Self> {
        if let Ok(buffer) = v8::Local::<v8::ArrayBuffer>::try_from(value) {
            return Ok(Self(buffer.as_slice(lock).to_vec()));
        }
        if let Ok(view) = v8::Local::<v8::ArrayBufferView>::try_from(value) {
            return Ok(Self(view.as_slice(lock).to_vec()));
        }
        Ok(Self(String::from_js(lock, value)?.into_bytes()))
    }
}
