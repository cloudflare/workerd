// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Every handle type upcasts with `From` to each of its supertypes, not just the direct one,
// and downcasts from each of them with a `TryFrom` that fails with a `DataError`, which is a
// `std::error::Error`.
use jsg::v8;
use jsg::v8::Local;

fn casts<A, B>()
where
    for<'a> Local<'a, B>: From<Local<'a, A>>,
    for<'a> Local<'a, A>: TryFrom<Local<'a, B>, Error = v8::DataError>,
{
}

macro_rules! supertypes {
    ($from:ident: $($to:ident),+) => {
        $(casts::<v8::$from, v8::$to>();)+
    };
}

pub fn all() {
    supertypes!(Name: Value);
    supertypes!(String: Name, Value);
    supertypes!(Symbol: Name, Value);
    supertypes!(BigInt: Value);
    supertypes!(Object: Value);
    supertypes!(Date: Object, Value);
    supertypes!(Function: Object, Value);
    supertypes!(Array: Object, Value);
    supertypes!(ArrayBuffer: Object, Value);
    supertypes!(ArrayBufferView: Object, Value);
    supertypes!(TypedArray: ArrayBufferView, Object, Value);
    supertypes!(Uint8Array: TypedArray, ArrayBufferView, Object, Value);
    supertypes!(Uint8ClampedArray: TypedArray, ArrayBufferView, Object, Value);
    supertypes!(Uint16Array: TypedArray, ArrayBufferView, Object, Value);
    supertypes!(Uint32Array: TypedArray, ArrayBufferView, Object, Value);
    supertypes!(Int8Array: TypedArray, ArrayBufferView, Object, Value);
    supertypes!(Int16Array: TypedArray, ArrayBufferView, Object, Value);
    supertypes!(Int32Array: TypedArray, ArrayBufferView, Object, Value);
    supertypes!(Float32Array: TypedArray, ArrayBufferView, Object, Value);
    supertypes!(Float64Array: TypedArray, ArrayBufferView, Object, Value);
    supertypes!(BigInt64Array: TypedArray, ArrayBufferView, Object, Value);
    supertypes!(BigUint64Array: TypedArray, ArrayBufferView, Object, Value);
}

pub fn down(
    value: Local<'_, v8::Value>,
) -> Result<Local<'_, v8::Uint8Array>, Box<dyn std::error::Error>> {
    let object: Local<'_, v8::Object> = value.try_into()?;
    Ok(Local::<v8::Uint8Array>::try_from(object)?)
}
