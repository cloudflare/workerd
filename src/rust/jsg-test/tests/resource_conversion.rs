// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for resource `ToJS` and `FromJS` macro-generated implementations.
//!
//! These tests verify the automatically derived `ToJS` and `FromJS` implementations
//! that the `#[jsg_resource]` macro generates for resource types:
//!
//! - `ToJS`: allocates a `Ref<R>` and wraps it as a JS object (alloc + wrap)
//! - `FromJS`: unwraps a JS object back to a `Ref<R>`, returning a `TypeError`
//!   for non-matching values

use jsg::FromJS;
use jsg::Number;
use jsg::ToJS;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Greeter {
    greeting: String,
}

#[jsg_resource]
impl Greeter {
    #[jsg_method]
    pub fn greet(&self, name: &str) -> String {
        format!("{}, {}!", self.greeting, name)
    }
}

#[jsg_resource]
struct Counter {
    value: f64,
}

#[jsg_resource]
impl Counter {
    #[jsg_method]
    pub fn get_value(&self) -> Number {
        Number::new(self.value)
    }
}

// =============================================================================
// ToJS tests
// =============================================================================

/// `ToJS` creates a JS object that has the resource's methods.
#[test]
fn to_js_creates_object_with_methods() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let greeter = Greeter {
            greeting: "Hello".to_owned(),
        };
        let js_val = greeter.to_js(lock);
        ctx.set_global("greeter", js_val);

        let result: String = ctx.eval(lock, "greeter.greet('World')").unwrap();
        assert_eq!(result, "Hello, World!");
        Ok(())
    });
}

/// `ToJS` produces a valid object for different resource types.
#[test]
fn to_js_works_for_different_resource_types() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let counter = Counter { value: 42.0 };
        let js_val = counter.to_js(lock);
        ctx.set_global("counter", js_val);

        let result: Number = ctx.eval(lock, "counter.getValue()").unwrap();
        assert!((result.value() - 42.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// `ToJS` called twice on different values creates distinct JS objects.
#[test]
fn to_js_creates_distinct_objects() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let g1 = Greeter {
            greeting: "Hi".to_owned(),
        };
        let g2 = Greeter {
            greeting: "Hey".to_owned(),
        };
        ctx.set_global("g1", g1.to_js(lock));
        ctx.set_global("g2", g2.to_js(lock));

        let same: bool = ctx.eval(lock, "g1 === g2").unwrap();
        assert!(
            !same,
            "distinct resources should produce distinct JS objects"
        );

        let r1: String = ctx.eval(lock, "g1.greet('A')").unwrap();
        let r2: String = ctx.eval(lock, "g2.greet('A')").unwrap();
        assert_eq!(r1, "Hi, A!");
        assert_eq!(r2, "Hey, A!");
        Ok(())
    });
}

// =============================================================================
// FromJS tests
// =============================================================================

/// `FromJS` unwraps a JS-wrapped resource back to a `Ref<R>`.
#[test]
fn from_js_unwraps_resource() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let greeter = Greeter {
            greeting: "Howdy".to_owned(),
        };
        ctx.set_global("greeter", greeter.to_js(lock));

        // Get the JS value back and use FromJS to unwrap it.
        let js_val = ctx.eval_raw("greeter").unwrap();
        let r = Greeter::from_js(lock, js_val).expect("FromJS should succeed");
        assert_eq!(r.greeting, "Howdy");
        Ok(())
    });
}

/// `FromJS` returns a `Ref<R>` that keeps the resource alive.
#[test]
fn from_js_ref_keeps_resource_alive() {
    use std::sync::atomic::AtomicUsize;
    use std::sync::atomic::Ordering;

    static DROPS: AtomicUsize = AtomicUsize::new(0);

    #[jsg_resource]
    struct Tracked {
        tag: String,
    }

    impl Drop for Tracked {
        fn drop(&mut self) {
            DROPS.fetch_add(1, Ordering::SeqCst);
        }
    }

    #[jsg_resource]
    impl Tracked {}

    DROPS.store(0, Ordering::SeqCst);

    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let tracked = Tracked {
            tag: "alive".to_owned(),
        };
        ctx.set_global("obj", tracked.to_js(lock));

        let js_val = ctx.eval_raw("obj").unwrap();
        let r = Tracked::from_js(lock, js_val).expect("FromJS should succeed");
        assert_eq!(r.tag, "alive");

        // The Ref from FromJS keeps the resource alive even after GC.
        crate::Harness::request_gc(lock);
        assert_eq!(DROPS.load(Ordering::SeqCst), 0);
        assert_eq!(r.tag, "alive");

        std::mem::drop(r);
        Ok(())
    });
}

/// `FromJS` returns a `TypeError` for a plain JS object.
#[test]
fn from_js_rejects_plain_object() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let js_val = ctx.eval_raw("({})").unwrap();
        let err = Greeter::from_js(lock, js_val).unwrap_err();
        assert_eq!(err.name, jsg::ExceptionType::TypeError);
        assert_eq!(err.message, "expected Greeter, got object");
        Ok(())
    });
}

/// `FromJS` returns a `TypeError` for a string.
#[test]
fn from_js_rejects_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let js_val = ctx.eval_raw("'not a resource'").unwrap();
        let err = Greeter::from_js(lock, js_val).unwrap_err();
        assert_eq!(err.name, jsg::ExceptionType::TypeError);
        assert_eq!(err.message, "expected Greeter, got string");
        Ok(())
    });
}

/// `FromJS` returns a `TypeError` for a number.
#[test]
fn from_js_rejects_number() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let js_val = ctx.eval_raw("42").unwrap();
        let err = Greeter::from_js(lock, js_val).unwrap_err();
        assert_eq!(err.name, jsg::ExceptionType::TypeError);
        assert_eq!(err.message, "expected Greeter, got number");
        Ok(())
    });
}

/// `FromJS` returns a `TypeError` for null.
#[test]
fn from_js_rejects_null() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let js_val = ctx.eval_raw("null").unwrap();
        let err = Greeter::from_js(lock, js_val).unwrap_err();
        assert_eq!(err.name, jsg::ExceptionType::TypeError);
        // typeof null === "object" in JavaScript
        assert_eq!(err.message, "expected Greeter, got object");
        Ok(())
    });
}

/// `FromJS` returns a `TypeError` for undefined.
#[test]
fn from_js_rejects_undefined() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let js_val = ctx.eval_raw("undefined").unwrap();
        let err = Greeter::from_js(lock, js_val).unwrap_err();
        assert_eq!(err.name, jsg::ExceptionType::TypeError);
        assert_eq!(err.message, "expected Greeter, got undefined");
        Ok(())
    });
}

// =============================================================================
// Round-trip tests
// =============================================================================

/// `ToJS` followed by `FromJS` preserves the resource data.
#[test]
fn round_trip_preserves_data() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let original = Greeter {
            greeting: "Bonjour".to_owned(),
        };
        ctx.set_global("obj", original.to_js(lock));

        let js_val = ctx.eval_raw("obj").unwrap();
        let unwrapped = Greeter::from_js(lock, js_val).expect("round-trip should succeed");
        assert_eq!(unwrapped.greeting, "Bonjour");
        Ok(())
    });
}

/// Round-trip through JavaScript — resource passes through JS code and comes back.
#[test]
fn round_trip_through_js_identity_function() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let greeter = Greeter {
            greeting: "Hola".to_owned(),
        };
        ctx.set_global("greeter", greeter.to_js(lock));

        // Pass through a JS identity function.
        let js_val = ctx.eval_raw("(x => x)(greeter)").unwrap();
        let unwrapped = Greeter::from_js(lock, js_val).expect("identity round-trip should work");
        assert_eq!(unwrapped.greeting, "Hola");
        Ok(())
    });
}

/// Round-trip preserves object identity — same wrapper object.
#[test]
fn round_trip_preserves_js_identity() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let greeter = Greeter {
            greeting: "Ciao".to_owned(),
        };
        ctx.set_global("g", greeter.to_js(lock));

        let same: bool = ctx.eval(lock, "g === (x => x)(g)").unwrap();
        assert!(
            same,
            "passing a resource through JS should preserve identity"
        );
        Ok(())
    });
}

// =============================================================================
// FromJS via eval_raw (integration with test harness)
// =============================================================================

/// Evaluating a global that holds a resource and calling `FromJS` returns a `Ref<R>`.
#[test]
fn eval_raw_returns_ref_via_from_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let greeter = Greeter {
            greeting: "Salut".to_owned(),
        };
        ctx.set_global("g", greeter.to_js(lock));

        let js_val = ctx.eval_raw("g").unwrap();
        let r = Greeter::from_js(lock, js_val).expect("FromJS should succeed");
        assert_eq!(r.greeting, "Salut");
        Ok(())
    });
}

/// `FromJS` on a non-resource value returns a `TypeError`.
#[test]
fn eval_raw_with_wrong_type_gives_type_error() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let js_val = ctx.eval_raw("42").unwrap();
        let err = Greeter::from_js(lock, js_val).unwrap_err();
        assert_eq!(err.name, jsg::ExceptionType::TypeError);
        assert_eq!(err.message, "expected Greeter, got number");
        Ok(())
    });
}

/// Resources stored in a JS array can be retrieved individually via `FromJS`.
#[test]
fn resource_stored_in_js_array_and_retrieved() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let g1 = Greeter {
            greeting: "One".to_owned(),
        };
        let g2 = Greeter {
            greeting: "Two".to_owned(),
        };
        ctx.set_global("g1", g1.to_js(lock));
        ctx.set_global("g2", g2.to_js(lock));

        let js_val = ctx.eval_raw("([g1, g2])[0]").unwrap();
        let r = Greeter::from_js(lock, js_val).expect("FromJS should succeed for index 0");
        assert_eq!(r.greeting, "One");

        let js_val = ctx.eval_raw("([g1, g2])[1]").unwrap();
        let r = Greeter::from_js(lock, js_val).expect("FromJS should succeed for index 1");
        assert_eq!(r.greeting, "Two");
        Ok(())
    });
}
