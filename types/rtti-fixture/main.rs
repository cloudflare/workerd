// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::io::Write;

use jsg::ToJsTypedArray;
use jsg::TypeInfo;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_inspect_property;
use jsg_macros::jsg_method;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;
use jsg_macros::jsg_struct;

type Label = String;
type OptionalLabel = Option<Label>;

#[jsg_struct(name = "RustRttiResult")]
struct ResultData {
    pub label: String,
    pub count: u32,
    pub detail: jsg::Nullable<String>,
    pub note: jsg::NullOrUndefined<String>,
}

#[jsg_oneof]
enum Choice {
    Text(String),
    Number(f64),
}

/// A union that holds a handle, so it borrows the scope of its lock.
#[jsg_oneof]
enum Payload<'a> {
    Bytes(jsg::v8::Local<'a, jsg::v8::Uint8Array>),
    Text(String),
}

#[jsg_resource(name = "RustRttiFixture", ts_root)]
struct Fixture {
    label: String,
    child: Cell<Option<jsg::Rc<Self>>>,
}

#[jsg_resource(nested_types(Rows = NestedChild))]
impl Fixture {
    #[jsg_constructor]
    fn constructor(label: OptionalLabel) -> Self {
        Self {
            label: label.unwrap_or_default(),
            child: Cell::new(None),
        }
    }

    #[jsg_property(prototype, name = "title", readonly)]
    fn get_label(&self) -> String {
        self.label.clone()
    }

    #[jsg_static_constant(name = "HALF")]
    const FRACTION: f64 = 1.5;

    #[jsg_method(name = "lookup")]
    fn lookup_value(
        &self,
        _lock: &mut jsg::Lock,
        label: Label,
        detail: jsg::Nullable<String>,
    ) -> jsg::Result<ResultData> {
        if label.is_empty() {
            return Err(jsg::Error::new_type_error("label must not be empty"));
        }
        Ok(ResultData {
            label,
            count: 1,
            detail,
            note: jsg::NullOrUndefined::Null,
        })
    }

    #[jsg_method]
    fn copy_bytes(&self, bytes: &[u16]) -> Vec<u16> {
        bytes.to_vec()
    }

    #[jsg_method]
    fn typed_bytes<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        bytes: &[u16],
    ) -> jsg::v8::Local<'s, jsg::v8::Uint16Array> {
        bytes.to_js_typed_array(lock)
    }

    #[jsg_method]
    fn wide(&self, value: i64) -> u64 {
        value.unsigned_abs()
    }

    #[jsg_method]
    fn echo<'s>(
        &self,
        _lock: &mut jsg::Lock<'s>,
        value: jsg::v8::Local<'s, jsg::v8::Uint8Array>,
    ) -> jsg::v8::Local<'s, jsg::v8::Uint8Array> {
        value
    }

    #[jsg_method]
    fn present(&self, values: jsg::Dict<Option<u32>>) -> jsg::Dict<u32> {
        values
            .into_iter()
            .filter_map(|(key, value)| Some((key, value?)))
            .collect()
    }

    #[jsg_method]
    fn payload_kind(&self, payload: Payload<'_>) -> String {
        match payload {
            Payload::Bytes(_) => "bytes".to_owned(),
            Payload::Text(text) => text,
        }
    }

    #[jsg_method]
    fn labels(&self, labels: Vec<Label>) -> Vec<Label> {
        labels
    }

    #[jsg_method]
    fn choose(value: Choice) -> String {
        match value {
            Choice::Text(text) => text,
            Choice::Number(number) => format!("{number:?}"),
        }
    }

    #[jsg_method]
    fn reserved(
        &self,
        r#new: String,
        class: String,
        default: String,
        delete: String,
        // A first parameter named `this` receives the resource, so this one is raw.
        r#this: String,
    ) -> String {
        format!("{new}{class}{default}{delete}{this}")
    }

    #[jsg_method]
    fn parameter_names(&self, record_type: String, _unused_arg: String, r#void: String) -> String {
        format!("{record_type}{void}")
    }

    #[jsg_method]
    fn make_symbol<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::v8::Local<'s, jsg::v8::Symbol> {
        jsg::v8::Symbol::new(lock, None)
    }

    #[jsg_method]
    fn make_name<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::v8::Local<'s, jsg::v8::Name> {
        jsg::v8::Symbol::new(lock, None).into()
    }

    #[jsg_method]
    fn as_typed_array<'s>(
        &self,
        value: jsg::v8::Local<'s, jsg::v8::Uint8Array>,
    ) -> jsg::v8::Local<'s, jsg::v8::TypedArray> {
        value.into()
    }

    #[jsg_method]
    fn error_message(&self, error: jsg::Error) -> String {
        error.message
    }

    #[jsg_property(prototype)]
    fn get_child(&self) -> jsg::Nullable<jsg::Rc<Self>> {
        let child = self.child.take();
        let result = child.clone();
        self.child.set(child);
        match result {
            Some(child) => jsg::Nullable::Some(child),
            None => jsg::Nullable::Null,
        }
    }

    #[jsg_property(prototype)]
    fn set_child(&self, value: jsg::Nullable<jsg::Rc<Self>>) {
        self.child.set(match value {
            jsg::Nullable::Some(child) => Some(child),
            jsg::Nullable::Null => None,
        });
    }

    #[jsg_method]
    fn pick(&self, value: jsg::NullOrUndefined<String>) -> jsg::NullOrUndefined<String> {
        value
    }

    #[jsg_method]
    fn hint(&self, hint: Option<jsg::Nullable<String>>) -> Option<jsg::Nullable<String>> {
        hint
    }

    #[jsg_inspect_property]
    fn debug_label(&self) -> String {
        self.label.clone()
    }

    #[jsg_method]
    fn later(&self, when: std::time::SystemTime) -> std::time::SystemTime {
        when
    }

    #[jsg_method]
    fn with_this(this: jsg::Rc<Self>, _lock: &mut jsg::Lock, suffix: String) -> String {
        format!("{}{suffix}", this.label)
    }

    #[jsg_method]
    fn settle<'s>(
        &self,
        promise: jsg::v8::Local<'s, jsg::v8::Promise>,
    ) -> jsg::v8::Local<'s, jsg::v8::Promise> {
        promise
    }

    #[jsg_method]
    fn epoch<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
    ) -> jsg::Result<jsg::v8::Local<'s, jsg::v8::Date>> {
        jsg::v8::Date::new(lock, 0.0)
    }

    #[jsg_method]
    fn big_value<'s>(&self, lock: &mut jsg::Lock<'s>) -> jsg::v8::Local<'s, jsg::v8::BigInt> {
        jsg::v8::BigInt::new(lock, 1)
    }
}

/// Reachable only as a nested type of `Fixture`, under another name.
#[jsg_resource(name = "RustRttiChild")]
struct NestedChild;

#[jsg_resource]
impl NestedChild {
    // A constructor has no receiver, so a parameter named `this` is an argument.
    #[jsg_constructor]
    fn constructor(this: String) -> Self {
        drop(this);
        Self
    }

    #[jsg_method]
    fn detail(&self) -> ChildDetail {
        ChildDetail { depth: 1 }
    }

    #[jsg_property(lazy_instance, readonly)]
    fn get_kind(&self) -> String {
        "child".to_owned()
    }
}

/// Reachable only through `NestedChild`.
#[jsg_struct(name = "RustRttiChildDetail")]
struct ChildDetail {
    pub depth: u32,
}

#[jsg_resource]
struct OtherModule;

#[jsg_resource]
impl OtherModule {
    #[jsg_method]
    fn make_fixture(&self) -> jsg::Rc<Fixture> {
        jsg::Rc::new(Fixture::constructor(None))
    }
}

/// Registers members by compatibility flag, which RTTI follows.
#[jsg_resource(name = "RustRttiVersioned")]
struct Versioned;

#[jsg_resource(custom_members)]
impl Versioned {
    #[jsg_property(readonly)]
    fn get_mode(&self) -> String {
        "mode".to_owned()
    }

    #[jsg_method]
    fn cached(&self) -> bool {
        true
    }
}

impl jsg::Resource for Versioned {
    fn members(
        registry: &mut impl jsg::MemberRegistry,
        flags: jsg::feature_flags::compatibility_flags::Reader<'_>,
    ) {
        let placement = if flags.get_jsg_property_on_prototype_template() {
            jsg::PropertyPlacement::Prototype
        } else {
            jsg::PropertyPlacement::Instance
        };
        registry.add(Self::get_mode_member(placement));
        if flags.get_cache_option_enabled() {
            registry.add(Self::cached_member());
        }
    }
}

/// A root of its own, not reachable from the resource root.
#[jsg_struct(name = "RustRttiStructRoot", ts_root)]
struct StructRoot {
    pub label: String,
}

#[jsg_struct]
struct Unreachable {
    pub hidden: String,
}

/// Writes the fixture's RTTI. With `--flagged`, the export sets the compatibility flags that
/// `Versioned` registers by; otherwise every flag is false.
fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut cx = if std::env::args().any(|arg| arg == "--flagged") {
        jsg::rtti::Context::with_flags(|mut flags| {
            flags.set_cache_option_enabled(true);
            flags.set_jsg_property_on_prototype_template(true);
        })
    } else {
        jsg::rtti::Context::default()
    };
    cx.add_module::<Fixture>("test:first");
    cx.add_module::<OtherModule>("test:second");
    cx.add_module::<Versioned>("test:versioned");
    // A registered definition is not necessarily reachable from a public root.
    StructRoot::describe(&mut cx);
    Unreachable::describe(&mut cx);
    std::io::stdout().write_all(&cx.encode("rust-fixture"))?;
    Ok(())
}
