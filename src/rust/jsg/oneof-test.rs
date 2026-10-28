// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::panic::AssertUnwindSafe;
use std::time::SystemTime;

use super::*;
use crate::Dict;
use crate::FromJS;
use crate::NonCoercible;
use crate::Nullable;

/// The message `validate` panics with for `types`, or `None` if they are valid.
fn rejection(types: UnionTypes) -> Option<String> {
    let payload = std::panic::catch_unwind(AssertUnwindSafe(|| types.validate())).err()?;
    payload
        .downcast_ref::<&str>()
        .map(|message| (*message).to_owned())
        .or_else(|| payload.downcast_ref::<String>().cloned())
}

/// The types of a union of the types in `members`.
fn union(members: &[UnionTypes]) -> UnionTypes {
    members
        .iter()
        .fold(UnionTypes::OTHER, |sum, &member| sum.plus(member))
}

#[test]
fn valid_unions_pass() {
    for members in [
        vec![UnionTypes::STRING, UnionTypes::NUMERIC, UnionTypes::BOOLEAN],
        vec![
            UnionTypes::INTERFACE_LIKE,
            UnionTypes::INTERFACE_LIKE,
            UnionTypes::DICTIONARY,
            UnionTypes::SEQUENCE_LIKE,
            UnionTypes::STRING,
        ],
        vec![UnionTypes::OBJECT, UnionTypes::STRING, UnionTypes::OTHER],
        vec![UnionTypes::RECORD, UnionTypes::STRING.nullable()],
        vec![UnionTypes::OTHER, UnionTypes::OTHER],
    ] {
        assert_eq!(rejection(union(&members)), None, "{members:?}");
    }
}

/// Each rule of C++ JSG's `webidl::UnionTypeValidator`, broken by the smallest union.
#[test]
fn each_web_idl_rule_rejects_a_union() {
    let cases = [
        (
            vec![UnionTypes::DICTIONARY, UnionTypes::NUMERIC.nullable()],
            "at most one nullable or dictionary type",
        ),
        (
            vec![
                UnionTypes::STRING.nullable(),
                UnionTypes::NUMERIC.nullable(),
            ],
            "at most one nullable or dictionary type",
        ),
        (
            vec![UnionTypes::BOOLEAN, UnionTypes::BOOLEAN],
            "at most one boolean type",
        ),
        (
            vec![UnionTypes::NUMERIC, UnionTypes::NUMERIC],
            "at most one numeric type",
        ),
        (
            vec![UnionTypes::STRING, UnionTypes::STRING],
            "at most one string type",
        ),
        (
            vec![UnionTypes::OBJECT, UnionTypes::OBJECT],
            "at most one object type",
        ),
        (
            vec![UnionTypes::OBJECT, UnionTypes::INTERFACE_LIKE],
            "an object type only if",
        ),
        (
            vec![UnionTypes::OBJECT, UnionTypes::RECORD],
            "an object type only if",
        ),
        (
            vec![UnionTypes::OBJECT, UnionTypes::SEQUENCE_LIKE],
            "an object type only if",
        ),
        (
            vec![UnionTypes::DICTIONARY, UnionTypes::RECORD],
            "at most one dictionary-like type",
        ),
        (
            vec![UnionTypes::SEQUENCE_LIKE, UnionTypes::SEQUENCE_LIKE],
            "at most one sequence-like type",
        ),
        (
            vec![UnionTypes::STRING.optional()],
            "may not contain Option<T> or Lenient<T>",
        ),
    ];
    for (members, rule) in cases {
        let message = rejection(union(&members)).unwrap_or_default();
        assert!(message.contains(rule), "{members:?}: {message:?}");
    }
}

/// Web IDL flattens a nullable type to its inner type, and counts the nullable type as well.
#[test]
fn nullable_counts_its_inner_type() {
    let types = UnionTypes::STRING.nullable();
    assert_eq!(types.string, 1);
    assert_eq!(types.nullable, 1);
    assert!(rejection(union(&[types, UnionTypes::STRING])).is_some());
}

/// Asserts that `T`'s pass in the union conversion and its Web IDL categories are `member` and
/// `types`. The two are separate constants, `FromJS::UNION_MEMBER` and
/// `UnionVariant::UNION_TYPES`, so this table keeps them consistent with each other and with C++
/// JSG's predicates (`IsResourceType`, `IsFallibleType` and the like in jsg/value.h, and the
/// categories in jsg/web-idl.h).
fn assert_kind<T: UnionVariant + FromJS<'static>>(member: UnionMember, types: UnionTypes) {
    let name = std::any::type_name::<T>();
    assert_eq!(T::UNION_MEMBER, member, "{name}");
    assert_eq!(<T as UnionVariant>::UNION_TYPES, types, "{name}");
}

#[test]
fn each_type_has_the_kind_of_its_cpp_counterpart() {
    use UnionMember::Boolean;
    use UnionMember::Fallible;
    use UnionMember::Numeric;

    assert_kind::<bool>(Boolean, UnionTypes::BOOLEAN);
    assert_kind::<NonCoercible<bool>>(Boolean, UnionTypes::BOOLEAN);
    assert_kind::<String>(UnionMember::String, UnionTypes::STRING);
    assert_kind::<NonCoercible<String>>(UnionMember::String, UnionTypes::STRING);
    assert_kind::<f64>(Numeric, UnionTypes::NUMERIC);
    assert_kind::<u8>(Numeric, UnionTypes::NUMERIC);
    assert_kind::<u16>(Numeric, UnionTypes::NUMERIC);
    assert_kind::<u32>(Numeric, UnionTypes::NUMERIC);
    assert_kind::<i8>(Numeric, UnionTypes::NUMERIC);
    assert_kind::<i16>(Numeric, UnionTypes::NUMERIC);
    assert_kind::<i32>(Numeric, UnionTypes::NUMERIC);
    assert_kind::<i64>(Numeric, UnionTypes::NUMERIC);
    assert_kind::<u64>(Numeric, UnionTypes::NUMERIC);
    assert_kind::<SystemTime>(Fallible, UnionTypes::OTHER);
    assert_kind::<Vec<String>>(Fallible, UnionTypes::SEQUENCE_LIKE);
    assert_kind::<Vec<u8>>(Fallible, UnionTypes::SEQUENCE_LIKE);
    assert_kind::<Dict<f64>>(Fallible, UnionTypes::RECORD);
    assert_kind::<Nullable<String>>(Fallible, UnionTypes::STRING.nullable());
    assert_kind::<v8::Local<'static, v8::Value>>(Fallible, UnionTypes::OTHER);
    assert_kind::<v8::Local<'static, v8::String>>(UnionMember::String, UnionTypes::STRING);
    assert_kind::<v8::Local<'static, v8::BigInt>>(Numeric, UnionTypes::NUMERIC);
    assert_kind::<v8::Local<'static, v8::Object>>(Fallible, UnionTypes::OBJECT);
    assert_kind::<v8::Local<'static, v8::Function>>(Fallible, UnionTypes::OTHER);
    assert_kind::<v8::Local<'static, v8::Promise>>(Fallible, UnionTypes::OTHER);
    assert_kind::<v8::Local<'static, v8::Uint8Array>>(Fallible, UnionTypes::OTHER);
    assert_kind::<v8::Local<'static, v8::TypedArray>>(Fallible, UnionTypes::OTHER);
    assert_kind::<v8::Local<'static, v8::ArrayBuffer>>(Fallible, UnionTypes::OTHER);
}
