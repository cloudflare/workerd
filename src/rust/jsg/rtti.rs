// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! JavaScript type descriptions shared with the C++ JSG RTTI exporter.
//!
//! Descriptions contain no V8 handles and can be built without an isolate. Named
//! structures are interned before their members are visited, allowing recursive
//! resource graphs. Rust type identities and TypeScript declaration names are
//! separate: aliases resolve through trait dispatch rather than source parsing.

use std::any::TypeId;
use std::any::type_name;
use std::collections::HashMap;
use std::rc::Rc;

pub use rtti_capnp::builtin_type::Type as Builtin;

use crate::FeatureFlags;
use crate::MemberDefinition;
use crate::MemberRegistry;
use crate::Resource;
use crate::feature_flags::compatibility_flags;
use crate::v8;

/// The JavaScript representation of a Rust binding type.
pub trait TypeInfo {
    fn describe(cx: &mut Context) -> Type;
}

#[derive(Debug)]
pub enum Type {
    Unknown,
    Void,
    Boolean,
    Number,
    /// Encoded as C++ `jsg::JsBigInt`, which TypeScript declares as `number | bigint`.
    BigInt,
    String,
    Object,
    Builtin(Builtin),
    Array(Box<Self>),
    /// A record with string keys, like C++ `jsg::Dict<V>`.
    Dict(Box<Self>),
    Promise(Box<Self>),
    Maybe {
        value: Box<Self>,
        allows_null: bool,
        allows_undefined: bool,
    },
    OneOf(Vec<Self>),
    Structure {
        identity: &'static str,
        name: &'static str,
    },
}

pub struct Parameter {
    pub name: &'static str,
    pub ty: Type,
}

pub enum Member {
    Constructor(Vec<Parameter>),
    Method {
        name: &'static str,
        args: Vec<Parameter>,
        result: Type,
        is_static: bool,
    },
    Property {
        name: &'static str,
        ty: Type,
        readonly: bool,
        prototype: bool,
        /// Whether the property is an own data property of each instance, computed on first
        /// access.
        lazy: bool,
    },
    /// A nested resource type, exposed as a constructor property named `name`.
    Nested {
        name: String,
        identity: &'static str,
    },
    Constant {
        name: &'static str,
        value: crate::ConstantValue,
    },
}

struct Structure {
    identity: &'static str,
    name: &'static str,
    members: Vec<Member>,
    root: bool,
}

/// A deterministic, cycle-safe collection of reachable binding definitions.
///
/// Resource types are described with the members they register under the context's
/// compatibility flags, as the C++ RTTI builder runs `registerMembers` with its flags.
pub struct Context {
    structures: Vec<Structure>,
    // Structures are interned by `TypeId`, since `type_name` is not guaranteed to be unique.
    // `type_name` is the `fullyQualifiedName` that the generator identifies structures by,
    // so it must be unique too.
    indices: HashMap<TypeId, usize>,
    identities: HashMap<&'static str, usize>,
    modules: Vec<(String, &'static str)>,
    // Shared so that a resource's registration can read the flags while describing members
    // into the context.
    flags: Rc<FeatureFlags>,
}

/// A context whose compatibility flags are all false.
impl Default for Context {
    fn default() -> Self {
        Self::with_flags(|_| {})
    }
}

impl Context {
    /// Creates a context whose compatibility flags are set by `set_flags`. All flags start
    /// false; no compatibility date or flag implications are applied.
    pub fn with_flags(set_flags: impl FnOnce(compatibility_flags::Builder<'_>)) -> Self {
        let mut message = capnp::message::Builder::new_default();
        set_flags(message.init_root::<compatibility_flags::Builder<'_>>());
        Self::new(FeatureFlags::from_message(&message))
    }

    fn new(flags: FeatureFlags) -> Self {
        Self {
            structures: Vec::new(),
            indices: HashMap::new(),
            identities: HashMap::new(),
            modules: Vec::new(),
            flags: Rc::new(flags),
        }
    }

    /// Registers a module in the same type graph as every other module in this export.
    pub fn add_module<T: TypeInfo>(&mut self, specifier: &str) {
        let identity = self.describe_structure::<T>();
        self.modules.push((specifier.to_owned(), identity));
    }

    /// Describes `T`, which must be a named structure such as a resource type, and returns
    /// its identity.
    pub(crate) fn describe_structure<T: TypeInfo>(&mut self) -> &'static str {
        let Type::Structure { identity, .. } = T::describe(self) else {
            unreachable!("{} must describe a named structure", type_name::<T>());
        };
        identity
    }

    /// Describes the structure `T` the first time it is reached, and returns a reference to it.
    ///
    /// The structure is interned before `members` runs, so members can refer to it
    /// recursively.
    ///
    /// # Panics
    ///
    /// Panics if another type has the same [`type_name`] as `T`.
    pub fn structure<T: ?Sized + 'static>(
        &mut self,
        name: &'static str,
        root: bool,
        members: impl FnOnce(&mut Self) -> Vec<Member>,
    ) -> Type {
        let identity = type_name::<T>();
        if !self.indices.contains_key(&TypeId::of::<T>()) {
            let index = self.structures.len();
            assert!(
                self.identities.insert(identity, index).is_none(),
                "two Rust binding types are both named {identity}"
            );
            self.indices.insert(TypeId::of::<T>(), index);
            self.structures.push(Structure {
                identity,
                name,
                members: Vec::new(),
                root,
            });
            let members = members(self);
            self.structures[index].members = members;
        }
        Type::Structure { identity, name }
    }

    /// Serializes the collected definitions. A module export identifies its root
    /// without making that root an ambient global declaration.
    pub fn encode(&self, group: &str) -> Vec<u8> {
        let mut message = capnp::message::Builder::new_default();
        let mut root = message.init_root::<rtti_capnp::structure_groups::Builder>();
        let mut groups = root.reborrow().init_groups(1);
        let mut group_builder = groups.reborrow().get(0);
        group_builder.set_name(group);
        let mut structures = group_builder.init_structures(length(self.structures.len()));
        for i in 0..self.structures.len() {
            self.write_structure(i, structures.reborrow().get(length(i)), &mut Vec::new());
        }
        let mut modules = root.init_modules(length(self.modules.len()));
        for (i, (specifier, identity)) in self.modules.iter().enumerate() {
            let mut module = modules.reborrow().get(length(i));
            module.set_specifier(specifier.as_str());
            module.set_structure_name(*identity);
        }
        capnp::serialize::write_message_to_words(&message)
    }

    /// Writes the structure at `index`, within the nested types at the indices in `nesting`.
    ///
    /// As in C++, a nested type's member contains the nested type's whole structure, through
    /// which the generator reaches the types that the nested type refers to.
    fn write_structure(
        &self,
        index: usize,
        mut builder: rtti_capnp::structure::Builder<'_>,
        nesting: &mut Vec<usize>,
    ) {
        let structure = &self.structures[index];
        builder.set_name(structure.name);
        builder.set_ts_name(structure.name);
        builder.set_fully_qualified_name(structure.identity);
        // Template creation rejects a type that nests itself; its nested member only names it.
        if nesting.contains(&index) {
            return;
        }
        nesting.push(index);
        builder.set_ts_root(structure.root);
        let mut members = builder.init_members(length(structure.members.len()));
        for (i, member) in structure.members.iter().enumerate() {
            member.write(self, members.reborrow().get(length(i)), nesting);
        }
        nesting.pop();
    }
}

/// The registry through which RTTI export collects the members of a resource type.
struct DescribedMembers<'a> {
    cx: &'a mut Context,
    members: Vec<Member>,
}

impl MemberRegistry for DescribedMembers<'_> {
    fn add(&mut self, member: impl MemberDefinition) {
        if let Some(member) = member.describe(self.cx) {
            self.members.push(member);
        }
    }
}

/// Describes the members that `R` registers under the context's compatibility flags, in
/// registration order. Hand-built members other than nested types carry no description and
/// are omitted.
pub fn resource_members<R: Resource>(cx: &mut Context) -> Vec<Member> {
    let flags = Rc::clone(&cx.flags);
    let mut registry = DescribedMembers {
        cx,
        members: Vec::new(),
    };
    R::members(&mut registry, flags.reader());
    registry.members
}

pub(crate) fn new_rtti_context(feature_flags_data: &[u8]) -> Box<Context> {
    Box::new(Context::new(FeatureFlags::from_bytes(feature_flags_data)))
}

#[expect(
    clippy::expect_used,
    reason = "binding metadata cannot contain more than u32::MAX entries"
)]
fn length(value: usize) -> u32 {
    u32::try_from(value).expect("RTTI list length exceeds Cap'n Proto limit")
}

impl Type {
    fn write(&self, mut builder: rtti_capnp::type_::Builder<'_>) {
        match self {
            Self::Unknown => builder.set_unknown(()),
            Self::Void => builder.set_voidt(()),
            Self::Boolean => builder.set_boolt(()),
            Self::Number => builder.init_number().set_name("number"),
            Self::BigInt => builder.init_number().set_name("jsg::JsBigInt"),
            Self::String => builder.init_string().set_name("string"),
            Self::Object => builder.set_object(()),
            Self::Builtin(ty) => builder.init_builtin().set_type(*ty),
            Self::Array(element) => element.write(builder.init_array().init_element()),
            Self::Dict(value) => {
                let mut dict = builder.init_dict();
                Self::String.write(dict.reborrow().init_key());
                value.write(dict.init_value());
            }
            Self::Promise(value) => value.write(builder.init_promise().init_value()),
            Self::Maybe {
                value,
                allows_null,
                allows_undefined,
            } => {
                let mut maybe = builder.init_maybe();
                maybe.set_allows_null(*allows_null);
                maybe.set_allows_undefined(*allows_undefined);
                value.write(maybe.init_value());
            }
            Self::OneOf(variants) => {
                let mut list = builder.init_one_of().init_variants(length(variants.len()));
                for (i, variant) in variants.iter().enumerate() {
                    variant.write(list.reborrow().get(length(i)));
                }
            }
            Self::Structure { identity, name } => {
                let mut structure = builder.init_structure();
                structure.set_name(*name);
                structure.set_ts_name(*name);
                structure.set_fully_qualified_name(*identity);
            }
        }
    }
}

fn write_args(
    args: &[Parameter],
    mut builder: capnp::struct_list::Builder<'_, rtti_capnp::type_::Owned>,
) {
    for (i, arg) in args.iter().enumerate() {
        arg.ty.write(builder.reborrow().get(length(i)));
    }
}

fn write_arg_names(args: &[Parameter], mut builder: capnp::text_list::Builder<'_>) {
    for (i, arg) in args.iter().enumerate() {
        builder.set(length(i), arg.name);
    }
}

impl Member {
    fn write(
        &self,
        cx: &Context,
        builder: rtti_capnp::member::Builder<'_>,
        nesting: &mut Vec<usize>,
    ) {
        match self {
            Self::Constructor(args) => {
                let mut constructor = builder.init_constructor();
                write_args(args, constructor.reborrow().init_args(length(args.len())));
                write_arg_names(args, constructor.init_arg_names(length(args.len())));
            }
            Self::Method {
                name,
                args,
                result,
                is_static,
            } => {
                let mut method = builder.init_method();
                method.set_name(*name);
                method.set_static(*is_static);
                write_args(args, method.reborrow().init_args(length(args.len())));
                write_arg_names(args, method.reborrow().init_arg_names(length(args.len())));
                result.write(method.init_return_type());
            }
            Self::Property {
                name,
                ty,
                readonly,
                prototype,
                lazy,
            } => {
                let mut property = builder.init_property();
                property.set_name(*name);
                property.set_readonly(*readonly);
                property.set_prototype(*prototype);
                property.set_lazy(*lazy);
                ty.write(property.init_type());
            }
            Self::Nested { name, identity } => {
                let mut nested = builder.init_nested();
                nested.set_name(name.as_str());
                cx.write_structure(cx.identities[identity], nested.init_structure(), nesting);
            }
            Self::Constant { name, value } => {
                let mut constant = builder.init_constant();
                constant.set_name(*name);
                match value {
                    crate::ConstantValue::Number(value) => constant.set_number_value(*value),
                }
            }
        }
    }
}

macro_rules! primitive {
    ($ty:ty, $description:expr) => {
        impl TypeInfo for $ty {
            fn describe(_: &mut Context) -> Type {
                $description
            }
        }
    };
}

primitive!((), Type::Void);
primitive!(bool, Type::Boolean);
primitive!(String, Type::String);
primitive!(str, Type::String);
primitive!(std::time::SystemTime, Type::Builtin(Builtin::KjDate));

impl<T: TypeInfo + ?Sized> TypeInfo for &T {
    fn describe(cx: &mut Context) -> Type {
        T::describe(cx)
    }
}

/// `T | undefined`. An `Option<Nullable<T>>` is a single `T | null | undefined`, so an
/// optional parameter of that type is declared optional.
impl<T: TypeInfo> TypeInfo for Option<T> {
    fn describe(cx: &mut Context) -> Type {
        match T::describe(cx) {
            Type::Maybe {
                value, allows_null, ..
            } => Type::Maybe {
                value,
                allows_null,
                allows_undefined: true,
            },
            value => Type::Maybe {
                value: Box::new(value),
                allows_null: false,
                allows_undefined: true,
            },
        }
    }
}

/// `T | null`. Conversion from JavaScript also reads `undefined` as null, as C++
/// `kj::Maybe<T>` does, which C++ RTTI also describes as `T | null`; a parameter of this type
/// is still required.
impl<T: TypeInfo> TypeInfo for crate::Nullable<T> {
    fn describe(cx: &mut Context) -> Type {
        Type::Maybe {
            value: Box::new(T::describe(cx)),
            allows_null: true,
            allows_undefined: false,
        }
    }
}

/// `T | null | undefined`, like `Option<Nullable<T>>`.
impl<T: TypeInfo> TypeInfo for crate::Lenient<T> {
    fn describe(cx: &mut Context) -> Type {
        Option::<crate::Nullable<T>>::describe(cx)
    }
}

/// `T | null | undefined`, like `Option<Nullable<T>>`, so a parameter of this type is declared
/// optional.
impl<T: TypeInfo> TypeInfo for crate::NullOrUndefined<T> {
    fn describe(cx: &mut Context) -> Type {
        Option::<crate::Nullable<T>>::describe(cx)
    }
}

impl<T: TypeInfo> TypeInfo for crate::NonCoercible<T> {
    fn describe(cx: &mut Context) -> Type {
        T::describe(cx)
    }
}

impl<T: TypeInfo, E> TypeInfo for Result<T, E> {
    fn describe(cx: &mut Context) -> Type {
        T::describe(cx)
    }
}

impl<T: crate::Resource + TypeInfo> TypeInfo for crate::Rc<T> {
    fn describe(cx: &mut Context) -> Type {
        T::describe(cx)
    }
}

/// A JavaScript array, numbers included; typed arrays are their `v8` handle types.
impl<T: TypeInfo> TypeInfo for Vec<T> {
    fn describe(cx: &mut Context) -> Type {
        Type::Array(Box::new(T::describe(cx)))
    }
}

impl<V: TypeInfo> TypeInfo for crate::Dict<V> {
    fn describe(cx: &mut Context) -> Type {
        Type::Dict(Box::new(V::describe(cx)))
    }
}

impl<T: TypeInfo> TypeInfo for [T] {
    fn describe(cx: &mut Context) -> Type {
        Type::Array(Box::new(T::describe(cx)))
    }
}

// Integers of up to 32 bits convert to numbers, and `i64` and `u64` to BigInts, which accept
// numbers too, as C++ RTTI describes `int64_t` and `uint64_t`.
primitive!(u8, Type::Number);
primitive!(u16, Type::Number);
primitive!(u32, Type::Number);
primitive!(i8, Type::Number);
primitive!(i16, Type::Number);
primitive!(i32, Type::Number);
primitive!(f32, Type::Number);
primitive!(f64, Type::Number);
primitive!(i64, Type::BigInt);
primitive!(u64, Type::BigInt);

primitive!(v8::Uint8Array, Type::Builtin(Builtin::V8Uint8Array));
primitive!(v8::Uint16Array, Type::Builtin(Builtin::V8Uint16Array));
primitive!(v8::Uint32Array, Type::Builtin(Builtin::V8Uint32Array));
primitive!(v8::Int8Array, Type::Builtin(Builtin::V8Int8Array));
primitive!(v8::Int16Array, Type::Builtin(Builtin::V8Int16Array));
primitive!(v8::Int32Array, Type::Builtin(Builtin::V8Int32Array));
primitive!(v8::Float32Array, Type::Builtin(Builtin::V8Float32Array));
primitive!(v8::Float64Array, Type::Builtin(Builtin::V8Float64Array));
primitive!(v8::BigInt64Array, Type::Builtin(Builtin::V8BigInt64Array));
primitive!(v8::BigUint64Array, Type::Builtin(Builtin::V8BigUint64Array));

primitive!(v8::Value, Type::Unknown);
primitive!(v8::Object, Type::Object);
primitive!(v8::String, Type::String);
primitive!(v8::Boolean, Type::Boolean);
primitive!(v8::Number, Type::Number);
primitive!(v8::Integer, Type::Number);
primitive!(v8::Int32, Type::Number);
primitive!(v8::Uint32, Type::Number);
primitive!(v8::Symbol, Type::Builtin(Builtin::V8Symbol));
primitive!(v8::BigInt, Type::BigInt);
primitive!(
    v8::Name,
    Type::OneOf(vec![Type::String, Type::Builtin(Builtin::V8Symbol)])
);
primitive!(v8::TypedArray, Type::Builtin(Builtin::V8TypedArray));
primitive!(crate::Error, Type::Builtin(Builtin::JsgError));
primitive!(v8::Array, Type::Array(Box::new(Type::Unknown)));
primitive!(v8::Function, Type::Builtin(Builtin::V8Function));
// As in C++, an untyped promise resolves to an unknown value.
primitive!(v8::Promise, Type::Promise(Box::new(Type::Unknown)));
primitive!(v8::ArrayBuffer, Type::Builtin(Builtin::V8ArrayBuffer));
// As C++ describes `jsg::JsDate`.
primitive!(v8::Date, Type::Builtin(Builtin::KjDate));
primitive!(
    v8::ArrayBufferView,
    Type::Builtin(Builtin::V8ArrayBufferView)
);
primitive!(
    v8::Uint8ClampedArray,
    Type::Builtin(Builtin::V8Uint8ClampedArray)
);

impl<T: TypeInfo> TypeInfo for v8::Local<'_, T> {
    fn describe(cx: &mut Context) -> Type {
        T::describe(cx)
    }
}

impl<T: TypeInfo> TypeInfo for v8::Global<T> {
    fn describe(cx: &mut Context) -> Type {
        T::describe(cx)
    }
}

#[cfg(test)]
#[path = "rtti-test.rs"]
mod tests;
