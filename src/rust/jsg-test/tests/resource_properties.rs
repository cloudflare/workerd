// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `#[jsg_property(prototype)]`, `#[jsg_property(instance)]`, and
//! `#[jsg_inspect_property]`.

use std::cell::Cell;
use std::cell::RefCell;

use jsg::Number;
use jsg::ToJS;
use jsg_macros::jsg_inspect_property;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;

// =============================================================================
// Shared test resources
// =============================================================================

/// A counter with a read/write value, a read-only label, and an inspect property.
#[jsg_resource]
struct Counter {
    value: Cell<f64>,
    label: RefCell<String>,
}

#[jsg_resource]
impl Counter {
    // Read/write prototype property — detected from get_/set_ prefix.
    #[jsg_property(prototype)]
    pub fn get_value(&self) -> Number {
        Number::new(self.value.get())
    }

    #[jsg_property(prototype)]
    pub fn set_value(&self, v: Number) {
        self.value.set(v.value());
    }

    // Read-only prototype property (no matching set_).
    #[jsg_property(prototype, readonly)]
    pub fn get_label(&self) -> String {
        self.label.borrow().clone()
    }

    // A regular method coexisting with properties.
    #[jsg_method]
    pub fn reset(&self) {
        self.value.set(0.0);
    }

    // Inspect property — hidden from normal enumeration.
    #[jsg_inspect_property]
    pub fn debug_info(&self) -> String {
        format!(
            "Counter(value={}, label={})",
            self.value.get(),
            self.label.borrow()
        )
    }

    // Static constant coexisting with properties.
    #[jsg_static_constant]
    pub const MAX_VALUE: f64 = 1_000_000.0;
}

impl Counter {
    fn new(value: f64, label: impl Into<String>) -> Self {
        Self {
            value: Cell::new(value),
            label: RefCell::new(label.into()),
        }
    }
}

/// A token with a read/write instance property and a read-only one.
#[jsg_resource]
struct Token {
    id: RefCell<String>,
    kind: String,
}

#[jsg_resource]
impl Token {
    #[jsg_property(instance)]
    pub fn get_id(&self) -> String {
        self.id.borrow().clone()
    }

    #[jsg_property(instance)]
    pub fn set_id(&self, v: String) {
        *self.id.borrow_mut() = v;
    }

    // Read-only instance property.
    #[jsg_property(instance, readonly)]
    pub fn get_kind(&self) -> String {
        self.kind.clone()
    }
}

impl Token {
    fn new(id: impl Into<String>, kind: impl Into<String>) -> Self {
        Self {
            id: RefCell::new(id.into()),
            kind: kind.into(),
        }
    }
}

/// A resource with multi-word `snake_case` property names to test camelCase conversion.
#[jsg_resource]
struct MultiWord {
    first_name: RefCell<String>,
    last_name: RefCell<String>,
}

#[jsg_resource]
impl MultiWord {
    #[jsg_property(prototype)]
    pub fn get_first_name(&self) -> String {
        self.first_name.borrow().clone()
    }

    #[jsg_property(prototype)]
    pub fn set_first_name(&self, v: String) {
        *self.first_name.borrow_mut() = v;
    }

    #[jsg_property(prototype, readonly)]
    pub fn get_last_name(&self) -> String {
        self.last_name.borrow().clone()
    }
}

impl MultiWord {
    fn new(first: impl Into<String>, last: impl Into<String>) -> Self {
        Self {
            first_name: RefCell::new(first.into()),
            last_name: RefCell::new(last.into()),
        }
    }
}

/// A resource with an explicit name override.
#[jsg_resource]
struct ExplicitName {
    x: Cell<f64>,
}

#[jsg_resource]
impl ExplicitName {
    #[jsg_property(prototype, name = "myValue")]
    pub fn get_something(&self) -> Number {
        Number::new(self.x.get())
    }

    #[jsg_property(prototype, name = "myValue")]
    pub fn set_something(&self, v: Number) {
        self.x.set(v.value());
    }

    #[jsg_inspect_property(name = "debugX")]
    pub fn internal_debug(&self) -> Number {
        Number::new(self.x.get())
    }
}

impl ExplicitName {
    fn new(x: f64) -> Self {
        Self { x: Cell::new(x) }
    }
}

/// A resource where a property returns `Option<T>` (can be null in JS).
#[jsg_resource]
struct MaybeHolder {
    inner: Cell<Option<f64>>,
}

#[jsg_resource]
impl MaybeHolder {
    #[jsg_property(prototype)]
    pub fn get_value(&self) -> Option<Number> {
        self.inner.get().map(Number::new)
    }

    #[jsg_property(prototype)]
    pub fn set_value(&self, v: Number) {
        self.inner.set(Some(v.value()));
    }
}

impl MaybeHolder {
    fn with_value(v: f64) -> Self {
        Self {
            inner: Cell::new(Some(v)),
        }
    }

    fn empty() -> Self {
        Self {
            inner: Cell::new(None),
        }
    }
}

// =============================================================================
// Prototype property — getter
// =============================================================================

#[test]
fn prototype_getter_returns_initial_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(42.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        let v: Number = ctx.eval(lock, "c.value").unwrap();
        assert!((v.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn prototype_getter_reflects_rust_side_mutation() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "x"));
        let r2 = r.clone();
        ctx.set_global("c", r.to_js(lock));
        r2.value.set(99.0);
        let v: Number = ctx.eval(lock, "c.value").unwrap();
        assert!((v.value() - 99.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn prototype_getter_returns_string_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(0.0, "hello"));
        ctx.set_global("c", r.to_js(lock));
        let v: String = ctx.eval(lock, "c.label").unwrap();
        assert_eq!(v, "hello");
        Ok(())
    });
}

// =============================================================================
// Prototype property — setter
// =============================================================================

#[test]
fn prototype_setter_updates_value_visible_via_getter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(0.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        ctx.eval_raw("c.value = 77").unwrap();
        let v: Number = ctx.eval(lock, "c.value").unwrap();
        assert!((v.value() - 77.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn prototype_setter_mutation_visible_from_rust() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(0.0, "x"));
        let r2 = r.clone();
        ctx.set_global("c", r.to_js(lock));
        ctx.eval_raw("c.value = 55").unwrap();
        assert!((r2.value.get() - 55.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn prototype_setter_called_multiple_times() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(0.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        ctx.eval_raw("c.value = 1; c.value = 2; c.value = 3")
            .unwrap();
        let v: Number = ctx.eval(lock, "c.value").unwrap();
        assert!((v.value() - 3.0).abs() < f64::EPSILON);
        Ok(())
    });
}

// =============================================================================
// Prototype property — read-only enforcement
// =============================================================================

#[test]
fn prototype_readonly_property_throws_in_strict_mode() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "lbl"));
        ctx.set_global("c", r.to_js(lock));
        let result = ctx.eval_raw("'use strict'; c.label = 'changed'");
        assert!(
            result.is_err(),
            "expected TypeError assigning to read-only prototype property"
        );
        Ok(())
    });
}

#[test]
fn prototype_readonly_property_silently_ignored_in_sloppy_mode() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "original"));
        ctx.set_global("c", r.to_js(lock));
        // In sloppy mode, the assignment is silently ignored; value unchanged.
        ctx.eval_raw("c.label = 'changed'").unwrap();
        let v: String = ctx.eval(lock, "c.label").unwrap();
        assert_eq!(v, "original");
        Ok(())
    });
}

// =============================================================================
// Prototype property — enumerability / prototype chain
// =============================================================================

#[test]
fn prototype_property_not_in_object_keys() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        let keys: String = ctx.eval(lock, "Object.keys(c).join(',')").unwrap();
        assert_eq!(
            keys, "",
            "prototype properties must not appear in Object.keys()"
        );
        Ok(())
    });
}

#[test]
fn prototype_property_found_by_in_operator() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        let found: bool = ctx.eval(lock, "'value' in c").unwrap();
        assert!(found, "'in' must find prototype properties");
        Ok(())
    });
}

#[test]
fn prototype_property_not_an_own_property() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        let own: bool = ctx
            .eval(lock, "Object.prototype.hasOwnProperty.call(c, 'value')")
            .unwrap();
        assert!(!own, "prototype property must NOT be an own property");
        Ok(())
    });
}

// =============================================================================
// Prototype property — enumerability (matches C++ JSG_PROTOTYPE_PROPERTY)
// =============================================================================

#[test]
fn prototype_property_is_enumerable() {
    // C++ registerPrototypeProperty uses v8::PropertyAttribute::None for normal
    // (non-Unimplemented) properties, making them enumerable.  Verify the Rust
    // layer matches this behaviour.
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        // value is a read/write prototype property.
        let enumerable: bool = ctx
            .eval(
                lock,
                "Object.getOwnPropertyDescriptor(Object.getPrototypeOf(c), 'value').enumerable",
            )
            .unwrap();
        assert!(
            enumerable,
            "read-write prototype property must be enumerable (matching C++ JSG)"
        );
        Ok(())
    });
}

#[test]
fn prototype_readonly_property_is_enumerable() {
    // C++ registerReadonlyPrototypeProperty uses v8::PropertyAttribute::ReadOnly
    // (without DontEnum) for normal properties, making them enumerable.
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        // label is a read-only prototype property.
        let enumerable: bool = ctx
            .eval(
                lock,
                "Object.getOwnPropertyDescriptor(Object.getPrototypeOf(c), 'label').enumerable",
            )
            .unwrap();
        assert!(
            enumerable,
            "read-only prototype property must be enumerable (matching C++ JSG)"
        );
        Ok(())
    });
}

#[test]
fn prototype_property_appears_in_for_in() {
    // Enumerable prototype properties must appear in for...in loops.
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        let found: bool = ctx
            .eval(
                lock,
                "(function() { for (var k in c) { if (k === 'value') return true; } return false; })()",
            )
            .unwrap();
        assert!(found, "prototype property 'value' must appear in for...in");
        Ok(())
    });
}

// =============================================================================
// Prototype property — multiple instances are independent
// =============================================================================

#[test]
fn prototype_property_independent_across_instances() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let a = jsg::Rc::new(Counter::new(10.0, "a"));
        let b = jsg::Rc::new(Counter::new(20.0, "b"));
        ctx.set_global("a", a.to_js(lock));
        ctx.set_global("b", b.to_js(lock));
        let va: Number = ctx.eval(lock, "a.value").unwrap();
        let vb: Number = ctx.eval(lock, "b.value").unwrap();
        assert!((va.value() - 10.0).abs() < f64::EPSILON);
        assert!((vb.value() - 20.0).abs() < f64::EPSILON);
        ctx.eval_raw("a.value = 99").unwrap();
        let va2: Number = ctx.eval(lock, "a.value").unwrap();
        let vb2: Number = ctx.eval(lock, "b.value").unwrap();
        assert!((va2.value() - 99.0).abs() < f64::EPSILON);
        assert!(
            (vb2.value() - 20.0).abs() < f64::EPSILON,
            "setting a.value must not affect b"
        );
        Ok(())
    });
}

// =============================================================================
// Prototype property — coexistence with methods and constants
// =============================================================================

#[test]
fn prototype_property_coexists_with_method() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(5.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        ctx.eval_raw("c.value = 7").unwrap();
        ctx.eval_raw("c.reset()").unwrap();
        let v: Number = ctx.eval(lock, "c.value").unwrap();
        assert!(
            (v.value()).abs() < f64::EPSILON,
            "reset() must set value back to 0"
        );
        Ok(())
    });
}

#[test]
fn prototype_property_coexists_with_static_constant() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Counter>(lock);
        ctx.set_global("Counter", constructor.into());
        let r = jsg::Rc::new(Counter::new(1.0, "x"));
        ctx.set_global("c", r.to_js(lock));

        let v: Number = ctx.eval(lock, "c.value").unwrap();
        assert!((v.value() - 1.0).abs() < f64::EPSILON);

        let max: Number = ctx.eval(lock, "Counter.MAX_VALUE").unwrap();
        assert!((max.value() - 1_000_000.0).abs() < f64::EPSILON);
        Ok(())
    });
}

// =============================================================================
// Prototype property — camelCase name derivation
// =============================================================================

#[test]
fn prototype_property_multi_word_camel_case() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(MultiWord::new("Alice", "Smith"));
        ctx.set_global("p", r.to_js(lock));
        let first: String = ctx.eval(lock, "p.firstName").unwrap();
        let last: String = ctx.eval(lock, "p.lastName").unwrap();
        assert_eq!(first, "Alice");
        assert_eq!(last, "Smith");
        Ok(())
    });
}

#[test]
fn prototype_property_multi_word_setter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(MultiWord::new("Alice", "Smith"));
        ctx.set_global("p", r.to_js(lock));
        ctx.eval_raw("p.firstName = 'Bob'").unwrap();
        let first: String = ctx.eval(lock, "p.firstName").unwrap();
        assert_eq!(first, "Bob");
        Ok(())
    });
}

#[test]
fn prototype_property_original_rust_name_not_exposed() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(MultiWord::new("Alice", "Smith"));
        ctx.set_global("p", r.to_js(lock));
        // The raw Rust names (get_first_name, set_first_name) must not be visible.
        let undef: bool = ctx
            .eval(lock, "typeof p.getFirstName === 'undefined'")
            .unwrap();
        assert!(undef, "raw getter name must not be exposed");
        let undef2: bool = ctx
            .eval(lock, "typeof p.setFirstName === 'undefined'")
            .unwrap();
        assert!(undef2, "raw setter name must not be exposed");
        Ok(())
    });
}

// =============================================================================
// Prototype property — explicit name override
// =============================================================================

#[test]
fn explicit_name_override_getter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(ExplicitName::new(42.0));
        ctx.set_global("obj", r.to_js(lock));
        let v: Number = ctx.eval(lock, "obj.myValue").unwrap();
        assert!((v.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn explicit_name_override_setter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(ExplicitName::new(0.0));
        ctx.set_global("obj", r.to_js(lock));
        ctx.eval_raw("obj.myValue = 7").unwrap();
        let v: Number = ctx.eval(lock, "obj.myValue").unwrap();
        assert!((v.value() - 7.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn explicit_name_original_rust_name_not_exposed() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(ExplicitName::new(0.0));
        ctx.set_global("obj", r.to_js(lock));
        let undef: bool = ctx
            .eval(lock, "typeof obj.getSomething === 'undefined'")
            .unwrap();
        assert!(undef);
        Ok(())
    });
}

// =============================================================================
// Prototype property — Option<T> return (null)
// =============================================================================

#[test]
fn prototype_option_property_returns_value_when_some() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(MaybeHolder::with_value(std::f64::consts::PI));
        ctx.set_global("h", r.to_js(lock));
        let v: Number = ctx.eval(lock, "h.value").unwrap();
        assert!((v.value() - std::f64::consts::PI).abs() < 1e-10);
        Ok(())
    });
}

#[test]
fn prototype_option_property_returns_undefined_when_none() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(MaybeHolder::empty());
        ctx.set_global("h", r.to_js(lock));
        // Option<T>::None maps to `undefined` via jsg::ToJS (same as a missing value).
        let is_nullish: bool = ctx.eval(lock, "h.value == null").unwrap();
        assert!(
            is_nullish,
            "None getter return must be nullish (null or undefined) in JS"
        );
        Ok(())
    });
}

#[test]
fn prototype_option_property_setter_makes_it_some() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(MaybeHolder::empty());
        ctx.set_global("h", r.to_js(lock));
        ctx.eval_raw("h.value = 2.5").unwrap();
        let v: Number = ctx.eval(lock, "h.value").unwrap();
        assert!((v.value() - 2.5).abs() < f64::EPSILON);
        Ok(())
    });
}

// =============================================================================
// Instance property — getter
// =============================================================================

#[test]
fn instance_getter_returns_initial_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Token::new("tok-123", "bearer"));
        ctx.set_global("t", r.to_js(lock));
        let id: String = ctx.eval(lock, "t.id").unwrap();
        assert_eq!(id, "tok-123");
        Ok(())
    });
}

#[test]
fn instance_readonly_getter_returns_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Token::new("abc", "jwt"));
        ctx.set_global("t", r.to_js(lock));
        let kind: String = ctx.eval(lock, "t.kind").unwrap();
        assert_eq!(kind, "jwt");
        Ok(())
    });
}

// =============================================================================
// Instance property — setter
// =============================================================================

#[test]
fn instance_setter_updates_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Token::new("old", "bearer"));
        ctx.set_global("t", r.to_js(lock));
        ctx.eval_raw("t.id = 'new-id'").unwrap();
        let id: String = ctx.eval(lock, "t.id").unwrap();
        assert_eq!(id, "new-id");
        Ok(())
    });
}

#[test]
fn instance_setter_visible_from_rust() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Token::new("orig", "bearer"));
        let r2 = r.clone();
        ctx.set_global("t", r.to_js(lock));
        ctx.eval_raw("t.id = 'updated'").unwrap();
        assert_eq!(*r2.id.borrow(), "updated");
        Ok(())
    });
}

// =============================================================================
// Instance property — own-property semantics
// =============================================================================

#[test]
fn instance_property_is_own_property() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Token::new("tok", "bearer"));
        ctx.set_global("t", r.to_js(lock));
        let own: bool = ctx
            .eval(lock, "Object.prototype.hasOwnProperty.call(t, 'id')")
            .unwrap();
        assert!(own, "instance property must be an own property");
        Ok(())
    });
}

#[test]
fn instance_property_has_accessor_descriptor() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Token::new("tok", "bearer"));
        ctx.set_global("t", r.to_js(lock));
        // An accessor descriptor has `get`/`set` keys, not `value`/`writable`.
        let has_get: bool = ctx
            .eval(
                lock,
                "typeof Object.getOwnPropertyDescriptor(t, 'id').get === 'function'",
            )
            .unwrap();
        let has_set: bool = ctx
            .eval(
                lock,
                "typeof Object.getOwnPropertyDescriptor(t, 'id').set === 'function'",
            )
            .unwrap();
        assert!(
            has_get,
            "instance property must have a getter in its descriptor"
        );
        assert!(
            has_set,
            "read-write instance property must have a setter in its descriptor"
        );
        Ok(())
    });
}

#[test]
fn instance_readonly_property_has_no_setter_in_descriptor() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Token::new("tok", "bearer"));
        ctx.set_global("t", r.to_js(lock));
        let set_undef: bool = ctx
            .eval(
                lock,
                "typeof Object.getOwnPropertyDescriptor(t, 'kind').set === 'undefined'",
            )
            .unwrap();
        assert!(set_undef, "read-only instance property must have no setter");
        Ok(())
    });
}

#[test]
fn instance_readonly_property_throws_in_strict_mode() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Token::new("tok", "bearer"));
        ctx.set_global("t", r.to_js(lock));
        let result = ctx.eval_raw("'use strict'; t.kind = 'other'");
        assert!(
            result.is_err(),
            "expected error assigning to read-only instance property"
        );
        Ok(())
    });
}

#[test]
fn instance_property_independent_across_instances() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let a = jsg::Rc::new(Token::new("id-a", "bearer"));
        let b = jsg::Rc::new(Token::new("id-b", "bearer"));
        ctx.set_global("a", a.to_js(lock));
        ctx.set_global("b", b.to_js(lock));
        ctx.eval_raw("a.id = 'changed-a'").unwrap();
        let id_a: String = ctx.eval(lock, "a.id").unwrap();
        let id_b: String = ctx.eval(lock, "b.id").unwrap();
        assert_eq!(id_a, "changed-a");
        assert_eq!(id_b, "id-b", "mutating a.id must not affect b.id");
        Ok(())
    });
}

// =============================================================================
// Inspect property
// =============================================================================

#[test]
fn inspect_property_not_accessible_by_string_key() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(3.0, "dbg"));
        ctx.set_global("c", r.to_js(lock));
        let undef: bool = ctx
            .eval(lock, "typeof c.debugInfo === 'undefined'")
            .unwrap();
        assert!(
            undef,
            "inspect property must not be accessible via string key"
        );
        Ok(())
    });
}

#[test]
fn inspect_property_not_in_object_keys() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(3.0, "dbg"));
        ctx.set_global("c", r.to_js(lock));
        let keys: String = ctx.eval(lock, "Object.keys(c).join(',')").unwrap();
        assert!(
            !keys.contains("debugInfo"),
            "inspect property must not appear in Object.keys()"
        );
        Ok(())
    });
}

#[test]
fn inspect_property_not_in_own_string_names() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(3.0, "dbg"));
        ctx.set_global("c", r.to_js(lock));
        let names: String = ctx
            .eval(lock, "Object.getOwnPropertyNames(c).join(',')")
            .unwrap();
        assert!(
            !names.contains("debugInfo"),
            "inspect property must not appear in getOwnPropertyNames(), got: {names}"
        );
        Ok(())
    });
}

#[test]
fn inspect_property_registered_under_a_symbol() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(3.0, "dbg"));
        ctx.set_global("c", r.to_js(lock));
        // The property is registered under a v8::Symbol; at least one symbol must exist on
        // the prototype chain that has an accessor descriptor.
        let has_symbol_accessor: bool = ctx
            .eval(
                lock,
                r"
                (function() {
                    let proto = Object.getPrototypeOf(c);
                    let syms = Object.getOwnPropertySymbols(proto);
                    for (let sym of syms) {
                        let d = Object.getOwnPropertyDescriptor(proto, sym);
                        if (d && typeof d.get === 'function') return true;
                    }
                    return false;
                })()
                ",
            )
            .unwrap();
        assert!(
            has_symbol_accessor,
            "inspect property must be accessible via its symbol"
        );
        Ok(())
    });
}

#[test]
fn inspect_property_getter_returns_correct_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(7.0, "test"));
        ctx.set_global("c", r.to_js(lock));
        // Retrieve the symbol-keyed getter from the prototype and call it.
        let result: String = ctx
            .eval(
                lock,
                r"
                (function() {
                    let proto = Object.getPrototypeOf(c);
                    let syms = Object.getOwnPropertySymbols(proto);
                    for (let sym of syms) {
                        let d = Object.getOwnPropertyDescriptor(proto, sym);
                        if (d && typeof d.get === 'function') {
                            return d.get.call(c);
                        }
                    }
                    return 'NOT FOUND';
                })()
                ",
            )
            .unwrap();
        assert!(
            result.contains("Counter"),
            "inspect getter must return debug string, got: {result}"
        );
        assert!(
            result.contains('7'),
            "debug string must contain current value"
        );
        Ok(())
    });
}

#[test]
fn inspect_explicit_name_override() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(ExplicitName::new(5.0));
        ctx.set_global("obj", r.to_js(lock));
        // The inspect property must not be accessible as a string key "debugX".
        let undef: bool = ctx.eval(lock, "typeof obj.debugX === 'undefined'").unwrap();
        assert!(
            undef,
            "explicitly-named inspect property must still be hidden under a symbol"
        );
        Ok(())
    });
}

// =============================================================================
// inspectProperties dictionary — kResourceTypeInspect integration
//
// node:util's inspect() relies on a `kResourceTypeInspect` API symbol being set
// on the prototype, whose value is an object (the inspectProperties template)
// mapping string names to unique symbols for each inspect property.
//
// From JavaScript the kResourceTypeInspect symbol is not accessible via
// Symbol.for() (it lives in the ForApi registry, not the global symbol table).
// However it IS returned by Object.getOwnPropertySymbols(), and we can identify
// it because its descriptor is a data property whose value is a plain object
// (the dictionary), distinguishing it from the per-property symbols whose
// descriptors are accessor properties (get/set).
// =============================================================================

/// Finds the inspectProperties dictionary installed on the prototype under
/// the kResourceTypeInspect API symbol.  Returns the dictionary object or
/// `null` if not found.
const FIND_INSPECT_DICT: &str = r"
(function(obj) {
    let proto = Object.getPrototypeOf(obj);
    for (let sym of Object.getOwnPropertySymbols(proto)) {
        let desc = Object.getOwnPropertyDescriptor(proto, sym);
        if (desc && typeof desc.value === 'object' && desc.value !== null) {
            return desc.value;
        }
    }
    return null;
})
";

#[test]
fn inspect_properties_dict_exists_on_prototype() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(3.0, "dbg"));
        ctx.set_global("c", r.to_js(lock));
        ctx.set_global("findInspectDict", ctx.eval_raw(FIND_INSPECT_DICT).unwrap());
        let found: bool = ctx.eval(lock, "findInspectDict(c) !== null").unwrap();
        assert!(
            found,
            "kResourceTypeInspect dictionary must be present on the prototype"
        );
        Ok(())
    });
}

#[test]
fn inspect_properties_dict_contains_property_name() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(3.0, "dbg"));
        ctx.set_global("c", r.to_js(lock));
        ctx.set_global("findInspectDict", ctx.eval_raw(FIND_INSPECT_DICT).unwrap());
        let has_key: bool = ctx.eval(lock, "'debugInfo' in findInspectDict(c)").unwrap();
        assert!(
            has_key,
            "inspectProperties dictionary must contain the camelCase property name"
        );
        Ok(())
    });
}

#[test]
fn inspect_properties_dict_value_is_the_getter_symbol() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(3.0, "dbg"));
        ctx.set_global("c", r.to_js(lock));
        ctx.set_global("findInspectDict", ctx.eval_raw(FIND_INSPECT_DICT).unwrap());
        // The value in the dictionary must be a symbol, and that same symbol must be the key
        // under which the getter accessor is registered on the prototype.
        let valid: bool = ctx
            .eval(
                lock,
                r"
                (function() {
                    let dict = findInspectDict(c);
                    let sym = dict['debugInfo'];
                    if (typeof sym !== 'symbol') return false;
                    let proto = Object.getPrototypeOf(c);
                    let d = Object.getOwnPropertyDescriptor(proto, sym);
                    return d !== undefined && typeof d.get === 'function';
                })()
                ",
            )
            .unwrap();
        assert!(
            valid,
            "inspectProperties dictionary value must be the symbol under which the getter lives"
        );
        Ok(())
    });
}

#[test]
fn inspect_properties_dict_getter_callable_via_symbol() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(7.0, "test"));
        ctx.set_global("c", r.to_js(lock));
        ctx.set_global("findInspectDict", ctx.eval_raw(FIND_INSPECT_DICT).unwrap());
        // Invoking the getter via the symbol from the dictionary should return the value.
        let result: String = ctx
            .eval(
                lock,
                r"
                (function() {
                    let sym = findInspectDict(c)['debugInfo'];
                    let proto = Object.getPrototypeOf(c);
                    return Object.getOwnPropertyDescriptor(proto, sym).get.call(c);
                })()
                ",
            )
            .unwrap();
        assert!(
            result.contains("Counter"),
            "getter invoked via dictionary symbol must return debug string, got: {result}"
        );
        assert!(
            result.contains('7'),
            "debug string must contain current value"
        );
        Ok(())
    });
}

#[test]
fn inspect_properties_dict_present_even_without_inspect_props() {
    // The kResourceTypeInspect symbol must be on the prototype of every JSG resource type
    // (even those with no #[jsg_inspect_property] fields) so node:util can correctly
    // identify the object as a JSG resource and walk its prototype chain.
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Token has only instance properties, no inspect properties.
        let r = jsg::Rc::new(Token::new("abc", "bearer"));
        ctx.set_global("t", r.to_js(lock));
        ctx.set_global("findInspectDict", ctx.eval_raw(FIND_INSPECT_DICT).unwrap());
        let found: bool = ctx
            .eval(lock, "findInspectDict(t) !== null")
            .unwrap();
        assert!(
            found,
            "kResourceTypeInspect dictionary must be present even on types with no inspect properties"
        );
        let empty: bool = ctx
            .eval(lock, "Object.keys(findInspectDict(t)).length === 0")
            .unwrap();
        assert!(empty, "dictionary must be empty when there are no inspect properties");
        Ok(())
    });
}

#[test]
fn inspect_properties_dict_explicit_name_override() {
    // When #[jsg_inspect_property(name = "...")] is used, the dictionary key must be
    // the overridden JS name, not the Rust method name.
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(ExplicitName::new(5.0));
        ctx.set_global("obj", r.to_js(lock));
        ctx.set_global("findInspectDict", ctx.eval_raw(FIND_INSPECT_DICT).unwrap());
        // Must contain "debugX" (the overridden name), not "internalDebug" (Rust method).
        let has_override: bool = ctx.eval(lock, "'debugX' in findInspectDict(obj)").unwrap();
        let has_raw: bool = ctx
            .eval(lock, "'internalDebug' in findInspectDict(obj)")
            .unwrap();
        assert!(
            has_override,
            "inspectProperties must use the overridden name 'debugX'"
        );
        assert!(
            !has_raw,
            "inspectProperties must not use the raw Rust method name 'internalDebug'"
        );
        Ok(())
    });
}

// =============================================================================
// Receiver guard — prototype properties must enforce correct `this`
// =============================================================================

#[test]
fn prototype_property_getter_throws_on_wrong_receiver() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        // Extract the getter from the descriptor and call it with a plain object receiver.
        let result = ctx.eval_raw(
            r"
            'use strict';
            let proto = Object.getPrototypeOf(c);
            let desc = Object.getOwnPropertyDescriptor(proto, 'value');
            desc.get.call({});
            ",
        );
        assert!(result.is_err(), "getter with wrong receiver must throw");
        Ok(())
    });
}

#[test]
fn prototype_property_setter_throws_on_wrong_receiver() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Counter::new(1.0, "x"));
        ctx.set_global("c", r.to_js(lock));
        let result = ctx.eval_raw(
            r"
            'use strict';
            let proto = Object.getPrototypeOf(c);
            let desc = Object.getOwnPropertyDescriptor(proto, 'value');
            desc.set.call({}, 99);
            ",
        );
        assert!(result.is_err(), "setter with wrong receiver must throw");
        Ok(())
    });
}

// =============================================================================
// Interplay: prototype vs instance properties on the same resource
// =============================================================================

/// A resource that mixes both kinds of properties.
#[jsg_resource]
struct Mixed {
    proto_val: Cell<f64>,
    instance_val: RefCell<String>,
}

#[jsg_resource]
impl Mixed {
    #[jsg_property(prototype)]
    pub fn get_proto_val(&self) -> Number {
        Number::new(self.proto_val.get())
    }

    #[jsg_property(prototype)]
    pub fn set_proto_val(&self, v: Number) {
        self.proto_val.set(v.value());
    }

    #[jsg_property(instance)]
    pub fn get_instance_val(&self) -> String {
        self.instance_val.borrow().clone()
    }

    #[jsg_property(instance)]
    pub fn set_instance_val(&self, v: String) {
        *self.instance_val.borrow_mut() = v;
    }
}

impl Mixed {
    fn new(p: f64, i: impl Into<String>) -> Self {
        Self {
            proto_val: Cell::new(p),
            instance_val: RefCell::new(i.into()),
        }
    }
}

#[test]
fn mixed_prototype_not_own_instance_is_own() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Mixed::new(1.0, "hello"));
        ctx.set_global("m", r.to_js(lock));

        let proto_own: bool = ctx
            .eval(lock, "Object.prototype.hasOwnProperty.call(m, 'protoVal')")
            .unwrap();
        let inst_own: bool = ctx
            .eval(
                lock,
                "Object.prototype.hasOwnProperty.call(m, 'instanceVal')",
            )
            .unwrap();

        assert!(!proto_own, "protoVal must NOT be an own property");
        assert!(inst_own, "instanceVal MUST be an own property");
        Ok(())
    });
}

#[test]
fn mixed_both_properties_readable() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Mixed::new(3.0, "world"));
        ctx.set_global("m", r.to_js(lock));
        let pv: Number = ctx.eval(lock, "m.protoVal").unwrap();
        let iv: String = ctx.eval(lock, "m.instanceVal").unwrap();
        assert!((pv.value() - 3.0).abs() < f64::EPSILON);
        assert_eq!(iv, "world");
        Ok(())
    });
}

#[test]
fn mixed_both_properties_writable() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(Mixed::new(0.0, "old"));
        ctx.set_global("m", r.to_js(lock));
        ctx.eval_raw("m.protoVal = 9; m.instanceVal = 'new'")
            .unwrap();
        let pv: Number = ctx.eval(lock, "m.protoVal").unwrap();
        let iv: String = ctx.eval(lock, "m.instanceVal").unwrap();
        assert!((pv.value() - 9.0).abs() < f64::EPSILON);
        assert_eq!(iv, "new");
        Ok(())
    });
}

// =============================================================================
// Combination matrix: every jsg_property flag permutation
//
//  placement : prototype | instance
//  name      : omitted  | name = "..."
//  readonly  : omitted  | readonly
//  rw        : getter-only | getter + setter
//
//  That gives 2 × 2 × 2 × 2 = 16 logical combinations, but readonly + rw
//  is always a compile error (tested separately in compile-fail tests), so
//  we cover the 12 valid runtime variants below.
// =============================================================================

/// Fixture covering all valid `#[jsg_property]` combinations in one resource.
#[jsg_resource]
struct AllCombinations {
    // backing values for each property
    a: Cell<f64>,       // prototype, no-name, rw
    b: Cell<f64>,       // prototype, no-name, readonly
    c: Cell<f64>,       // prototype, name,    rw
    d: Cell<f64>,       // prototype, name,    readonly
    e: RefCell<String>, // instance,  no-name, rw
    f: RefCell<String>, // instance,  no-name, readonly
    g: RefCell<String>, // instance,  name,    rw
    h: RefCell<String>, // instance,  name,    readonly
    // extra: name before readonly (attribute order independence)
    i: Cell<f64>, // prototype, name + readonly (name first)
    j: Cell<f64>, // instance,  readonly + name (readonly first)
}

#[jsg_resource]
impl AllCombinations {
    // ---- prototype, no name, rw ------------------------------------------------
    #[jsg_property(prototype)]
    pub fn get_a(&self) -> Number {
        Number::new(self.a.get())
    }
    #[jsg_property(prototype)]
    pub fn set_a(&self, v: Number) {
        self.a.set(v.value());
    }

    // ---- prototype, no name, readonly ------------------------------------------
    #[jsg_property(prototype, readonly)]
    pub fn get_b(&self) -> Number {
        Number::new(self.b.get())
    }

    // ---- prototype, with name, rw ----------------------------------------------
    #[jsg_property(prototype, name = "namedC")]
    pub fn get_c(&self) -> Number {
        Number::new(self.c.get())
    }
    #[jsg_property(prototype, name = "namedC")]
    pub fn set_c(&self, v: Number) {
        self.c.set(v.value());
    }

    // ---- prototype, with name, readonly ----------------------------------------
    #[jsg_property(prototype, name = "namedD", readonly)]
    pub fn get_d(&self) -> Number {
        Number::new(self.d.get())
    }

    // ---- instance, no name, rw -------------------------------------------------
    #[jsg_property(instance)]
    pub fn get_e(&self) -> String {
        self.e.borrow().clone()
    }
    #[jsg_property(instance)]
    pub fn set_e(&self, v: String) {
        *self.e.borrow_mut() = v;
    }

    // ---- instance, no name, readonly -------------------------------------------
    #[jsg_property(instance, readonly)]
    pub fn get_f(&self) -> String {
        self.f.borrow().clone()
    }

    // ---- instance, with name, rw -----------------------------------------------
    #[jsg_property(instance, name = "namedG")]
    pub fn get_g(&self) -> String {
        self.g.borrow().clone()
    }
    #[jsg_property(instance, name = "namedG")]
    pub fn set_g(&self, v: String) {
        *self.g.borrow_mut() = v;
    }

    // ---- instance, with name, readonly -----------------------------------------
    #[jsg_property(instance, name = "namedH", readonly)]
    pub fn get_h(&self) -> String {
        self.h.borrow().clone()
    }

    // ---- attribute order: name before readonly (prototype) ---------------------
    #[jsg_property(prototype, name = "namedI", readonly)]
    pub fn get_i(&self) -> Number {
        Number::new(self.i.get())
    }

    // ---- attribute order: readonly before name (instance) ----------------------
    #[jsg_property(instance, readonly, name = "namedJ")]
    pub fn get_j(&self) -> Number {
        Number::new(self.j.get())
    }
}

impl AllCombinations {
    fn new() -> Self {
        Self {
            a: Cell::new(1.0),
            b: Cell::new(2.0),
            c: Cell::new(3.0),
            d: Cell::new(4.0),
            e: RefCell::new("e".into()),
            f: RefCell::new("f".into()),
            g: RefCell::new("g".into()),
            h: RefCell::new("h".into()),
            i: Cell::new(9.0),
            j: Cell::new(10.0),
        }
    }
}

// ---- prototype, no-name, rw ---------------------------------------------------

#[test]
fn combo_prototype_noname_rw_get() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let v: Number = ctx.eval(lock, "o.a").unwrap();
        assert!((v.value() - 1.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn combo_prototype_noname_rw_set() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        ctx.eval_raw("o.a = 99").unwrap();
        let v: Number = ctx.eval(lock, "o.a").unwrap();
        assert!((v.value() - 99.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn combo_prototype_noname_rw_not_own_property() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let own: bool = ctx
            .eval(lock, "Object.prototype.hasOwnProperty.call(o, 'a')")
            .unwrap();
        assert!(!own, "prototype property must not be an own property");
        Ok(())
    });
}

// ---- prototype, no-name, readonly ---------------------------------------------

#[test]
fn combo_prototype_noname_readonly_get() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let v: Number = ctx.eval(lock, "o.b").unwrap();
        assert!((v.value() - 2.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn combo_prototype_noname_readonly_throws_in_strict() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let result = ctx.eval_raw("'use strict'; o.b = 5");
        assert!(
            result.is_err(),
            "readonly prototype property must throw in strict mode"
        );
        Ok(())
    });
}

#[test]
fn combo_prototype_noname_readonly_no_setter_in_descriptor() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let proto = ctx.eval_raw("Object.getPrototypeOf(o)").unwrap();
        let _ = proto;
        let set_undef: bool = ctx
            .eval(
                lock,
                "typeof Object.getOwnPropertyDescriptor(Object.getPrototypeOf(o), 'b').set === 'undefined'",
            )
            .unwrap();
        assert!(set_undef, "readonly prototype property must have no setter");
        Ok(())
    });
}

// ---- prototype, with name, rw -------------------------------------------------

#[test]
fn combo_prototype_named_rw_get() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let v: Number = ctx.eval(lock, "o.namedC").unwrap();
        assert!((v.value() - 3.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn combo_prototype_named_rw_set() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        ctx.eval_raw("o.namedC = 33").unwrap();
        let v: Number = ctx.eval(lock, "o.namedC").unwrap();
        assert!((v.value() - 33.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn combo_prototype_named_rw_raw_rust_name_hidden() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        // The Rust method name "getC" / "setC" must not be visible.
        let hidden: bool = ctx
            .eval(
                lock,
                "typeof o.getC === 'undefined' && typeof o.c === 'undefined'",
            )
            .unwrap();
        assert!(
            hidden,
            "raw getter name and camelCase default must be hidden when name is overridden"
        );
        Ok(())
    });
}

// ---- prototype, with name, readonly -------------------------------------------

#[test]
fn combo_prototype_named_readonly_get() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let v: Number = ctx.eval(lock, "o.namedD").unwrap();
        assert!((v.value() - 4.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn combo_prototype_named_readonly_throws_in_strict() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let result = ctx.eval_raw("'use strict'; o.namedD = 0");
        assert!(
            result.is_err(),
            "named readonly prototype property must throw in strict mode"
        );
        Ok(())
    });
}

// ---- instance, no-name, rw ----------------------------------------------------

#[test]
fn combo_instance_noname_rw_get() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let v: String = ctx.eval(lock, "o.e").unwrap();
        assert_eq!(v, "e");
        Ok(())
    });
}

#[test]
fn combo_instance_noname_rw_set() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        ctx.eval_raw("o.e = 'updated'").unwrap();
        let v: String = ctx.eval(lock, "o.e").unwrap();
        assert_eq!(v, "updated");
        Ok(())
    });
}

#[test]
fn combo_instance_noname_rw_is_own_property() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let own: bool = ctx
            .eval(lock, "Object.prototype.hasOwnProperty.call(o, 'e')")
            .unwrap();
        assert!(own, "instance property must be an own property");
        Ok(())
    });
}

#[test]
fn combo_instance_noname_rw_in_object_keys() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let keys: String = ctx.eval(lock, "Object.keys(o).sort().join(',')").unwrap();
        assert!(
            keys.contains('e'),
            "instance property must appear in Object.keys(), got: {keys}"
        );
        Ok(())
    });
}

// ---- instance, no-name, readonly ----------------------------------------------

#[test]
fn combo_instance_noname_readonly_get() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let v: String = ctx.eval(lock, "o.f").unwrap();
        assert_eq!(v, "f");
        Ok(())
    });
}

#[test]
fn combo_instance_noname_readonly_throws_in_strict() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let result = ctx.eval_raw("'use strict'; o.f = 'x'");
        assert!(
            result.is_err(),
            "readonly instance property must throw in strict mode"
        );
        Ok(())
    });
}

#[test]
fn combo_instance_noname_readonly_no_setter_in_descriptor() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let set_undef: bool = ctx
            .eval(
                lock,
                "typeof Object.getOwnPropertyDescriptor(o, 'f').set === 'undefined'",
            )
            .unwrap();
        assert!(
            set_undef,
            "readonly instance property must have no setter in descriptor"
        );
        Ok(())
    });
}

// ---- instance, with name, rw --------------------------------------------------

#[test]
fn combo_instance_named_rw_get() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let v: String = ctx.eval(lock, "o.namedG").unwrap();
        assert_eq!(v, "g");
        Ok(())
    });
}

#[test]
fn combo_instance_named_rw_set() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        ctx.eval_raw("o.namedG = 'ggg'").unwrap();
        let v: String = ctx.eval(lock, "o.namedG").unwrap();
        assert_eq!(v, "ggg");
        Ok(())
    });
}

#[test]
fn combo_instance_named_rw_is_own_property() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let own: bool = ctx
            .eval(lock, "Object.prototype.hasOwnProperty.call(o, 'namedG')")
            .unwrap();
        assert!(own, "named instance property must be an own property");
        Ok(())
    });
}

#[test]
fn combo_instance_named_rw_raw_rust_name_hidden() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let hidden: bool = ctx
            .eval(
                lock,
                "typeof o.getG === 'undefined' && typeof o.g === 'undefined'",
            )
            .unwrap();
        assert!(
            hidden,
            "camelCase default name must be hidden when name is overridden"
        );
        Ok(())
    });
}

// ---- instance, with name, readonly --------------------------------------------

#[test]
fn combo_instance_named_readonly_get() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let v: String = ctx.eval(lock, "o.namedH").unwrap();
        assert_eq!(v, "h");
        Ok(())
    });
}

#[test]
fn combo_instance_named_readonly_throws_in_strict() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let result = ctx.eval_raw("'use strict'; o.namedH = 'x'");
        assert!(
            result.is_err(),
            "named readonly instance property must throw in strict mode"
        );
        Ok(())
    });
}

#[test]
fn combo_instance_named_readonly_is_own_property() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let own: bool = ctx
            .eval(lock, "Object.prototype.hasOwnProperty.call(o, 'namedH')")
            .unwrap();
        assert!(
            own,
            "named readonly instance property must still be an own property"
        );
        Ok(())
    });
}

// ---- attribute ordering: name then readonly (prototype) -----------------------

#[test]
fn combo_attr_order_name_then_readonly_prototype_get() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        // #[jsg_property(prototype, name = "namedI", readonly)]
        let v: Number = ctx.eval(lock, "o.namedI").unwrap();
        assert!((v.value() - 9.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn combo_attr_order_name_then_readonly_prototype_is_readonly() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let result = ctx.eval_raw("'use strict'; o.namedI = 0");
        assert!(
            result.is_err(),
            "name-before-readonly prototype property must be read-only"
        );
        Ok(())
    });
}

// ---- attribute ordering: readonly then name (instance) ------------------------

#[test]
fn combo_attr_order_readonly_then_name_instance_get() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        // #[jsg_property(instance, readonly, name = "namedJ")]
        let v: Number = ctx.eval(lock, "o.namedJ").unwrap();
        assert!((v.value() - 10.0).abs() < f64::EPSILON);
        Ok(())
    });
}

#[test]
fn combo_attr_order_readonly_then_name_instance_is_own_and_readonly() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));
        let own: bool = ctx
            .eval(lock, "Object.prototype.hasOwnProperty.call(o, 'namedJ')")
            .unwrap();
        assert!(
            own,
            "readonly-before-name instance property must be an own property"
        );
        let result = ctx.eval_raw("'use strict'; o.namedJ = 0");
        assert!(
            result.is_err(),
            "readonly-before-name instance property must be read-only"
        );
        Ok(())
    });
}

// ---- instance props enumerable, prototype props not ---------------------------

#[test]
fn combo_object_keys_contains_only_instance_props() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("o", r.to_js(lock));

        // Instance props that MUST appear in Object.keys().
        for key in &["e", "f", "namedG", "namedH", "namedJ"] {
            let found: bool = ctx
                .eval(lock, &format!("Object.keys(o).includes({key:?})"))
                .unwrap();
            assert!(found, "instance key '{key}' must appear in Object.keys()");
        }

        // Prototype props that must NOT appear in Object.keys().
        for key in &["a", "b", "namedC", "namedD", "namedI"] {
            let found: bool = ctx
                .eval(lock, &format!("Object.keys(o).includes({key:?})"))
                .unwrap();
            assert!(
                !found,
                "prototype key '{key}' must NOT appear in Object.keys()"
            );
        }
        Ok(())
    });
}

// ---- multiple instances are independent (instance properties) -----------------

#[test]
fn combo_instance_props_independent_across_instances() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let a = jsg::Rc::new(AllCombinations::new());
        let b = jsg::Rc::new(AllCombinations::new());
        ctx.set_global("a", a.to_js(lock));
        ctx.set_global("b", b.to_js(lock));
        ctx.eval_raw("a.e = 'alpha'").unwrap();
        let ea: String = ctx.eval(lock, "a.e").unwrap();
        let eb: String = ctx.eval(lock, "b.e").unwrap();
        assert_eq!(ea, "alpha");
        assert_eq!(eb, "e", "mutating a.e must not affect b.e");
        Ok(())
    });
}

// =============================================================================
// get_/set_ prefix detection for getter/setter pairing
//
// These tests explicitly verify that:
//  - Methods named `get_<stem>` are registered as getters.
//  - Methods named `set_<stem>` are registered as setters.
//  - A getter and setter with the same `<stem>` are paired into a single
//    read-write property whose JS name is the camelCase form of `<stem>`.
//  - The raw Rust method names (`getX` / `setX`) are never exposed to JS.
//  - A `get_`-only method produces a read-only property.
// =============================================================================

/// Resource with a rw property (`get_count`/`set_count`) and a getter-only
/// property (`get_readonly_val`) to exercise prefix-based detection directly.
#[jsg_resource]
struct PrefixDetection {
    count: Cell<f64>,
    readonly_val: Cell<f64>,
}

#[jsg_resource]
impl PrefixDetection {
    /// `get_count` → getter for JS property "count"
    #[jsg_property(prototype)]
    pub fn get_count(&self) -> Number {
        Number::new(self.count.get())
    }

    /// `set_count` → setter for JS property "count" (same stem as `get_count`)
    #[jsg_property(prototype)]
    pub fn set_count(&self, v: Number) {
        self.count.set(v.value());
    }

    /// `get_readonly_val` → getter-only for JS property "readonlyVal"
    #[jsg_property(prototype, readonly)]
    pub fn get_readonly_val(&self) -> Number {
        Number::new(self.readonly_val.get())
    }
}

impl PrefixDetection {
    fn new(count: f64, readonly_val: f64) -> Self {
        Self {
            count: Cell::new(count),
            readonly_val: Cell::new(readonly_val),
        }
    }
}

/// The `get_` prefix identifies `get_count` as the getter; the value is
/// accessible as the JS property `"count"` (prefix stripped, camelCased).
#[test]
fn get_prefix_identifies_getter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(PrefixDetection::new(42.0, 0.0));
        ctx.set_global("obj", r.to_js(lock));
        let v: Number = ctx.eval(lock, "obj.count").unwrap();
        assert!(
            (v.value() - 42.0).abs() < f64::EPSILON,
            "get_count must be accessible as JS property 'count'"
        );
        Ok(())
    });
}

/// The `set_` prefix identifies `set_count` as the setter for the same
/// JS property `"count"` (matching stem with `get_count`).
#[test]
fn set_prefix_identifies_setter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(PrefixDetection::new(0.0, 0.0));
        ctx.set_global("obj", r.to_js(lock));
        ctx.eval_raw("obj.count = 99").unwrap();
        let v: Number = ctx.eval(lock, "obj.count").unwrap();
        assert!(
            (v.value() - 99.0).abs() < f64::EPSILON,
            "set_count must write to JS property 'count'"
        );
        Ok(())
    });
}

/// `get_count` and `set_count` share the same JS name `"count"` because both
/// have the same stem (`count`) after prefix stripping — they form a single
/// read-write property.
#[test]
fn getter_setter_paired_by_matching_stem() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(PrefixDetection::new(5.0, 0.0));
        ctx.set_global("obj", r.to_js(lock));
        // Both getter and setter must share the single JS name.
        let descriptor: bool = ctx
            .eval(
                lock,
                "(function() {
                    var d = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(obj), 'count');
                    return typeof d.get === 'function' && typeof d.set === 'function';
                })()",
            )
            .unwrap();
        assert!(
            descriptor,
            "get_count and set_count must be paired as getter+setter on JS property 'count'"
        );
        Ok(())
    });
}

/// The raw Rust getter name (`getCount`) must NOT be exposed as a JS property.
#[test]
fn getter_raw_rust_name_not_exposed() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(PrefixDetection::new(1.0, 0.0));
        ctx.set_global("obj", r.to_js(lock));
        let undef: bool = ctx
            .eval(lock, "typeof obj.getCount === 'undefined'")
            .unwrap();
        assert!(
            undef,
            "raw getter name 'getCount' must not be exposed to JS"
        );
        Ok(())
    });
}

/// The raw Rust setter name (`setCount`) must NOT be exposed as a JS property.
#[test]
fn setter_raw_rust_name_not_exposed() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(PrefixDetection::new(1.0, 0.0));
        ctx.set_global("obj", r.to_js(lock));
        let undef: bool = ctx
            .eval(lock, "typeof obj.setCount === 'undefined'")
            .unwrap();
        assert!(
            undef,
            "raw setter name 'setCount' must not be exposed to JS"
        );
        Ok(())
    });
}

/// A method annotated with only `get_` (no matching `set_`) produces a
/// getter-only (read-only) property; the JS property descriptor has a getter
/// but no setter.
#[test]
fn getter_only_method_produces_readonly_property() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(PrefixDetection::new(0.0, 7.0));
        ctx.set_global("obj", r.to_js(lock));
        let v: Number = ctx.eval(lock, "obj.readonlyVal").unwrap();
        assert!(
            (v.value() - 7.0).abs() < f64::EPSILON,
            "get_readonly_val must be readable as 'readonlyVal'"
        );
        // No setter in the descriptor.
        let no_setter: bool = ctx
            .eval(
                lock,
                "(function() {
                    var d = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(obj), 'readonlyVal');
                    return typeof d.set === 'undefined';
                })()",
            )
            .unwrap();
        assert!(no_setter, "getter-only property must have no setter in descriptor");
        Ok(())
    });
}

/// In strict mode, assigning to a getter-only property throws a `TypeError`.
#[test]
fn getter_only_throws_on_write_in_strict_mode() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(PrefixDetection::new(0.0, 3.0));
        ctx.set_global("obj", r.to_js(lock));
        let result = ctx.eval_raw("'use strict'; obj.readonlyVal = 99");
        assert!(
            result.is_err(),
            "assigning to getter-only property must throw in strict mode"
        );
        Ok(())
    });
}

// =============================================================================
// Default placement (`#[jsg_property]` without explicit placement argument)
//
// When no placement is specified, the macro defaults to `prototype` placement.
// These tests verify this default matches the behaviour of explicit
// `#[jsg_property(prototype)]`.
// =============================================================================

/// Resource using `#[jsg_property]` (no placement arg) to exercise the default.
#[jsg_resource]
struct DefaultPlacement {
    name: RefCell<String>,
    version: Cell<f64>,
}

#[jsg_resource]
impl DefaultPlacement {
    /// No placement arg → defaults to `prototype`.
    #[jsg_property]
    pub fn get_name(&self) -> String {
        self.name.borrow().clone()
    }

    #[jsg_property]
    pub fn set_name(&self, v: String) {
        *self.name.borrow_mut() = v;
    }

    /// Getter-only with default placement.
    #[jsg_property(readonly)]
    pub fn get_version(&self) -> Number {
        Number::new(self.version.get())
    }
}

impl DefaultPlacement {
    fn new(name: impl Into<String>, version: f64) -> Self {
        Self {
            name: RefCell::new(name.into()),
            version: Cell::new(version),
        }
    }
}

/// Default placement (`#[jsg_property]` with no args) places the property on
/// the prototype, not the instance — `Object.keys()` is empty.
#[test]
fn default_placement_is_prototype_not_instance() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(DefaultPlacement::new("hello", 1.0));
        ctx.set_global("obj", r.to_js(lock));
        // Prototype property → not an own property.
        let own: bool = ctx
            .eval(lock, "Object.prototype.hasOwnProperty.call(obj, 'name')")
            .unwrap();
        assert!(
            !own,
            "default-placement property must NOT be an own property"
        );
        // Prototype property → not in Object.keys().
        let keys: String = ctx.eval(lock, "Object.keys(obj).join(',')").unwrap();
        assert_eq!(
            keys, "",
            "default-placement property must not appear in Object.keys()"
        );
        Ok(())
    });
}

/// The getter works with default placement.
#[test]
fn default_placement_getter_returns_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(DefaultPlacement::new("world", 2.0));
        ctx.set_global("obj", r.to_js(lock));
        let v: String = ctx.eval(lock, "obj.name").unwrap();
        assert_eq!(v, "world");
        Ok(())
    });
}

/// The setter works with default placement.
#[test]
fn default_placement_setter_updates_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(DefaultPlacement::new("before", 0.0));
        ctx.set_global("obj", r.to_js(lock));
        ctx.eval_raw("obj.name = 'after'").unwrap();
        let v: String = ctx.eval(lock, "obj.name").unwrap();
        assert_eq!(v, "after");
        Ok(())
    });
}

/// A readonly property with default placement is accessible but rejects writes
/// in strict mode.
#[test]
fn default_placement_readonly_throws_on_write() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(DefaultPlacement::new("x", 42.0));
        ctx.set_global("obj", r.to_js(lock));
        let v: Number = ctx.eval(lock, "obj.version").unwrap();
        assert!((v.value() - 42.0).abs() < f64::EPSILON);
        let result = ctx.eval_raw("'use strict'; obj.version = 0");
        assert!(
            result.is_err(),
            "readonly default-placement property must throw on write"
        );
        Ok(())
    });
}

/// Default placement is on the prototype chain — `"name" in obj` is true.
#[test]
fn default_placement_found_via_in_operator() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let r = jsg::Rc::new(DefaultPlacement::new("hi", 1.0));
        ctx.set_global("obj", r.to_js(lock));
        let found: bool = ctx.eval(lock, "'name' in obj").unwrap();
        assert!(found, "prototype property must be found by 'in' operator");
        Ok(())
    });
}
