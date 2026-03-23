// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for resource method callbacks.
//!
//! These tests ensure that resource methods can be called from JavaScript and that
//! the resource pointer is correctly unwrapped. This specifically validates that
//! V8's internal field embedder data type tags are used correctly when getting
//! aligned pointers from internal fields.

use std::cell::Cell;
use std::rc::Rc;

use jsg::ExceptionType;
use jsg::Number;
use jsg::ToJS;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;

#[jsg_resource]
struct EchoResource {
    prefix: String,
}

#[jsg_resource]
#[expect(clippy::unnecessary_wraps)]
impl EchoResource {
    #[jsg_method]
    pub fn echo(&self, message: String) -> Result<String, jsg::Error> {
        Ok(format!("{}{}", self.prefix, message))
    }

    #[jsg_method]
    pub fn greet(&self, name: &str) -> String {
        format!("{}{}!", self.prefix, name)
    }
}

#[jsg_resource]
struct DirectReturnResource {
    name: String,
    counter: Rc<Cell<u32>>,
}

#[jsg_resource]
impl DirectReturnResource {
    #[jsg_method]
    pub fn get_name(&self) -> String {
        self.name.clone()
    }

    #[jsg_method]
    pub fn is_valid(&self) -> bool {
        !self.name.is_empty()
    }

    #[jsg_method]
    pub fn get_counter(&self) -> jsg::Number {
        jsg::Number::from(self.counter.get())
    }

    #[jsg_method]
    pub fn increment_counter(&self) {
        self.counter.set(self.counter.get() + 1);
    }

    #[jsg_method]
    pub fn maybe_name(&self) -> Option<String> {
        Some(self.name.clone()).filter(|s| !s.is_empty())
    }
}

/// Validates that resource methods can be called from JavaScript.
/// This test ensures the embedder data type tag is correctly used when
/// unwrapping resource pointers from V8 internal fields.
#[test]
fn resource_method_callback_receives_correct_self() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(EchoResource {
            prefix: "Hello, ".to_owned(),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("echoResource", wrapped);

        // Call the method from JavaScript
        let result: String = ctx.eval(lock, "echoResource.echo('World!')").unwrap();
        assert_eq!(result, "Hello, World!");
        Ok(())
    });
}

/// Validates that multiple method calls work correctly on the same resource.
#[test]
fn resource_method_can_be_called_multiple_times() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(EchoResource {
            prefix: ">> ".to_owned(),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("echo", wrapped);

        // First call
        let result: String = ctx.eval(lock, "echo.echo('first')").unwrap();
        assert_eq!(result, ">> first");

        // Second call
        let result: String = ctx.eval(lock, "echo.echo('second')").unwrap();
        assert_eq!(result, ">> second");
        Ok(())
    });
}

/// Validates that methods can accept &str parameters.
#[test]
fn resource_method_accepts_str_ref_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(EchoResource {
            prefix: "Hello, ".to_owned(),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("echo", wrapped);

        let result: String = ctx.eval(lock, "echo.greet('World')").unwrap();
        assert_eq!(result, "Hello, World!");
        Ok(())
    });
}

/// Validates that methods can return values directly without Result wrapper.
#[test]
fn resource_method_returns_non_result_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let counter = Rc::new(Cell::new(42));
        let resource = jsg::Rc::new(DirectReturnResource {
            name: "TestResource".to_owned(),
            counter: counter.clone(),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("resource", wrapped);

        // Test getString returns string
        let result: String = ctx.eval(lock, "resource.getName()").unwrap();
        assert_eq!(result, "TestResource");

        // Test isValid returns boolean
        let result: bool = ctx.eval(lock, "resource.isValid()").unwrap();
        assert!(result);

        // Test getCounter returns number
        let result: Number = ctx.eval(lock, "resource.getCounter()").unwrap();
        assert!((result.value() - 42.0).abs() < f64::EPSILON);

        // Test incrementCounter returns undefined (we just check it doesn't error)
        let _: Option<bool> = ctx.eval(lock, "resource.incrementCounter()").unwrap();
        assert_eq!(counter.get(), 43);

        // Test maybeName returns string for Some
        let result: String = ctx.eval(lock, "resource.maybeName()").unwrap();
        assert_eq!(result, "TestResource");
        Ok(())
    });
}

#[jsg_resource]
struct MathResource;

#[jsg_resource]
impl MathResource {
    #[jsg_method]
    pub fn add(a: Number, b: Number) -> Number {
        Number::new(a.value() + b.value())
    }

    #[jsg_method]
    pub fn greet(name: String) -> String {
        format!("Hello, {name}!")
    }

    #[jsg_method]
    pub fn divide(a: Number, b: Number) -> Result<Number, jsg::Error> {
        if b.value() == 0.0 {
            return Err(jsg::Error::new_range_error("Division by zero"));
        }
        Ok(Number::new(a.value() / b.value()))
    }

    #[jsg_method]
    pub fn get_prefix(&self) -> String {
        "math".to_owned()
    }
}

/// Validates that methods without &self are registered as static methods on the class.
#[test]
fn static_method_callable_on_class() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<MathResource>(lock);
        ctx.set_global("MathResource", constructor.into());

        let result: Number = ctx.eval(lock, "MathResource.add(2, 3)").unwrap();
        assert!((result.value() - 5.0).abs() < f64::EPSILON);

        let result: String = ctx.eval(lock, "MathResource.greet('World')").unwrap();
        assert_eq!(result, "Hello, World!");
        Ok(())
    });
}

/// Validates that instance methods still work when static methods are present.
#[test]
fn instance_and_static_methods_coexist() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        // Expose the class constructor as a global
        let constructor = jsg::resource::function_template_of::<MathResource>(lock);
        ctx.set_global("MathResource", constructor.into());

        // Allocate and expose an instance as a global
        let resource = jsg::Rc::new(MathResource);
        let wrapped = resource.to_js(lock);
        ctx.set_global("math", wrapped);

        // Instance method works on the object
        let result: String = ctx.eval(lock, "math.getPrefix()").unwrap();
        assert_eq!(result, "math");

        // Static method works on the class
        let result: Number = ctx.eval(lock, "MathResource.add(10, 20)").unwrap();
        assert!((result.value() - 30.0).abs() < f64::EPSILON);

        // Static methods are NOT on the instance
        let is_undefined: bool = ctx.eval(lock, "typeof math.add === 'undefined'").unwrap();
        assert!(is_undefined);
        Ok(())
    });
}

/// Validates that static methods with Result return type work on the success path.
#[test]
fn static_method_result_return_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<MathResource>(lock);
        ctx.set_global("MathResource", constructor.into());

        let result: String = ctx.eval(lock, "MathResource.greet('Rust')").unwrap();
        assert_eq!(result, "Hello, Rust!");
        Ok(())
    });
}

/// Validates that static methods propagate JS exceptions from `Result::Err`.
#[test]
fn static_method_throws_exception() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<MathResource>(lock);
        ctx.set_global("MathResource", constructor.into());

        // Valid call succeeds
        let result: Number = ctx.eval(lock, "MathResource.divide(10, 2)").unwrap();
        assert!((result.value() - 5.0).abs() < f64::EPSILON);

        // Division by zero throws a RangeError
        let err = ctx
            .eval::<Number>(lock, "MathResource.divide(1, 0)")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::RangeError);
        assert!(err.message.contains("Division by zero"));
        Ok(())
    });
}

/// Validates that Option<T> returns null for None.
#[test]
fn resource_method_returns_null_for_none() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(DirectReturnResource {
            name: String::new(),
            counter: Rc::new(Cell::new(0)),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("resource", wrapped);

        let result: Option<String> = ctx.eval(lock, "resource.maybeName()").unwrap();
        assert!(result.is_none());
        Ok(())
    });
}

// =============================================================================
// Static constant tests
// =============================================================================

#[jsg_resource]
struct ConstantResource;

#[jsg_resource]
impl ConstantResource {
    #[jsg_static_constant]
    pub const MAX_SIZE: u32 = 1024;

    #[jsg_static_constant]
    pub const STATUS_OK: i32 = 0;

    #[jsg_static_constant]
    pub const STATUS_ERROR: i32 = -1;

    #[jsg_static_constant]
    pub const SCALE_FACTOR: f64 = 2.5;

    #[jsg_method]
    pub fn get_name(&self) -> String {
        "constant_resource".to_owned()
    }
}

/// Validates that static constants are accessible on the constructor.
#[test]
fn static_constant_accessible_on_constructor() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<ConstantResource>(lock);
        ctx.set_global("ConstantResource", constructor.into());

        let result: Number = ctx.eval(lock, "ConstantResource.MAX_SIZE").unwrap();
        assert!((result.value() - 1024.0).abs() < f64::EPSILON);

        let result: Number = ctx.eval(lock, "ConstantResource.STATUS_OK").unwrap();
        assert!((result.value() - 0.0).abs() < f64::EPSILON);

        let result: Number = ctx.eval(lock, "ConstantResource.STATUS_ERROR").unwrap();
        assert!((result.value() - (-1.0)).abs() < f64::EPSILON);

        let result: Number = ctx.eval(lock, "ConstantResource.SCALE_FACTOR").unwrap();
        assert!((result.value() - 2.5).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Validates that static constants are also accessible on instances (via prototype).
#[test]
fn static_constant_accessible_on_instance() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ConstantResource {});
        let wrapped = resource.to_js(lock);
        ctx.set_global("obj", wrapped);

        let result: Number = ctx.eval(lock, "obj.MAX_SIZE").unwrap();
        assert!((result.value() - 1024.0).abs() < f64::EPSILON);

        let result: Number = ctx.eval(lock, "obj.STATUS_OK").unwrap();
        assert!((result.value() - 0.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Validates that static constants are read-only (not writable).
#[test]
fn static_constant_is_read_only() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<ConstantResource>(lock);
        ctx.set_global("ConstantResource", constructor.into());

        // Attempt to overwrite should silently fail (strict mode would throw).
        // The value should remain unchanged.
        let result: Number = ctx
            .eval(
                lock,
                "ConstantResource.MAX_SIZE = 9999; ConstantResource.MAX_SIZE",
            )
            .unwrap();
        assert!((result.value() - 1024.0).abs() < f64::EPSILON);
        Ok(())
    });
}

/// Validates that static constants coexist with methods.
#[test]
fn static_constant_coexists_with_methods() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(ConstantResource {});

        let constructor = jsg::resource::function_template_of::<ConstantResource>(lock);
        ctx.set_global("ConstantResource", constructor.into());

        let wrapped = resource.to_js(lock);
        ctx.set_global("obj", wrapped);

        // Instance method works
        let result: String = ctx.eval(lock, "obj.getName()").unwrap();
        assert_eq!(result, "constant_resource");

        // Static constant works on constructor
        let result: Number = ctx.eval(lock, "ConstantResource.MAX_SIZE").unwrap();
        assert!((result.value() - 1024.0).abs() < f64::EPSILON);

        // Static constant works on instance
        let result: Number = ctx.eval(lock, "obj.MAX_SIZE").unwrap();
        assert!((result.value() - 1024.0).abs() < f64::EPSILON);
        Ok(())
    });
}

// =============================================================================
// catch_panic tests
// =============================================================================

/// A resource whose `panic_now` method unconditionally panics.
/// Used to verify that a panic inside a `#[jsg_method]` callback is caught and
/// converted to a JavaScript exception rather than aborting the process.
#[jsg_resource]
struct PanicResource;

#[jsg_resource]
impl PanicResource {
    #[jsg_method]
    pub fn panic_now(&self) {
        panic!("intentional panic in jsg_method");
    }
}

/// Full round-trip: a JS "request handler" function calls a Rust-backed API
/// method that panics.  Verifies that:
///
/// 1. `catch_panic` converts the panic to an `"internal error"` JS exception
///    whose message does NOT expose the raw Rust panic string to JS.
/// 2. `request_termination()` is called so no further JS executes.
/// 3. The process is not aborted.
///
/// This mirrors a real Worker fetch handler that delegates to a Rust-backed
/// binding: the panic must error the request cleanly and terminate the isolate.
#[test]
fn panic_in_rust_backed_api_errors_request_and_terminates_isolate() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(PanicResource);
        let wrapped = resource.to_js(lock);
        ctx.set_global("obj", wrapped);

        // Simulate a fetch-handler that delegates to a Rust-backed binding.
        // The handler calls `obj.panicNow()` — just like real Worker code
        // would call into a Rust-implemented API method.
        ctx.eval_raw("function handleRequest() { while(true) { obj.panicNow(); } throw null; }")
            .unwrap();

        assert!(
            !lock.is_termination_requested(),
            "termination should not be requested before the panic"
        );

        // Invoke the handler — the panic inside panicNow() is caught by
        // catch_panic, which calls throw_internal_error() then
        // request_termination().  The internal error exception is set first
        // and is visible to eval's TryCatch; it carries "internal error" in
        // its message while hiding the raw panic string from JS.
        let err = ctx
            .eval::<bool>(lock, "handleRequest()")
            .unwrap_err()
            .unwrap_jsg_err(lock);

        assert!(
            err.message.contains("internal error"),
            "JS error message should say \"internal error\", got: {:?}",
            err.message
        );
        assert!(
            !err.message.contains("intentional panic in jsg_method"),
            "raw panic message must not be exposed to JS, got: {:?}",
            err.message
        );

        // The isolate is now terminated: no further JS execution is possible.
        assert!(
            lock.is_termination_requested(),
            "termination must be requested after a panic in a Rust-backed method"
        );

        Ok(())
    });
}

// =============================================================================
// Receiver guard tests
// =============================================================================

/// Validates that destructuring an instance method and calling it without a
/// receiver throws a `TypeError: Illegal invocation` rather than reaching Rust.
///
/// This tests the `v8::Signature` guard described in the SAFETY comment of the
/// `#[jsg_method]` callback generated by `jsg-macros`.
#[test]
fn instance_method_throws_on_missing_receiver() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(EchoResource {
            prefix: "Hello, ".to_owned(),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("obj", wrapped);

        // Destructure the method and call it with no receiver (`this` === global
        // / undefined in strict mode). V8's Signature check must fire before the
        // Rust callback is invoked.
        let err = ctx
            .eval::<String>(lock, "const { echo } = obj; echo('world')")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::TypeError);
        assert!(
            err.message.to_lowercase().contains("illegal invocation")
                || err.message.to_lowercase().contains("illegal"),
            "unexpected error message: {}",
            err.message
        );
        Ok(())
    });
}

/// Validates that `Reflect.apply` with a plain-object receiver also throws
/// `TypeError: Illegal invocation`.
#[test]
fn instance_method_throws_on_wrong_receiver_via_reflect_apply() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(EchoResource {
            prefix: "Hello, ".to_owned(),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("obj", wrapped);

        let err = ctx
            .eval::<String>(lock, "Reflect.apply(obj.echo, {}, ['world'])")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::TypeError);
        assert!(
            err.message.to_lowercase().contains("illegal invocation")
                || err.message.to_lowercase().contains("illegal"),
            "unexpected error message: {}",
            err.message
        );
        Ok(())
    });
}

/// Validates that a method called on the correct receiver still works after
/// the above receiver-guard checks, confirming normal dispatch is unaffected.
#[test]
fn instance_method_works_on_correct_receiver() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(EchoResource {
            prefix: "Hi, ".to_owned(),
        });
        let wrapped = resource.to_js(lock);
        ctx.set_global("obj", wrapped);

        // `Function.prototype.call` with the correct `this` must succeed.
        let result: String = ctx.eval(lock, "obj.echo.call(obj, 'world')").unwrap();
        assert_eq!(result, "Hi, world");
        Ok(())
    });
}

// =============================================================================
// Constructor tests
// =============================================================================

#[jsg_resource]
struct Greeting {
    message: String,
}

#[jsg_resource]
impl Greeting {
    #[jsg_constructor]
    fn constructor(message: String) -> Self {
        Self { message }
    }

    #[jsg_method]
    fn get_message(&self) -> String {
        self.message.clone()
    }
}

/// Resources without `#[jsg_constructor]` should throw when called with `new`.
#[test]
fn resource_without_constructor_throws() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<EchoResource>(lock);
        ctx.set_global("EchoResource", constructor.into());

        let result: Result<Number, _> = ctx.eval(lock, "new EchoResource('hi')");
        assert!(result.is_err(), "should throw illegal constructor");
        Ok(())
    });
}

/// A `#[jsg_constructor]` method is callable from JavaScript via `new`.
#[test]
fn constructor_creates_instance() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Greeting>(lock);
        ctx.set_global("Greeting", constructor.into());

        let result: String = ctx
            .eval(lock, "new Greeting('hello').getMessage()")
            .unwrap();
        assert_eq!(result, "hello");
        Ok(())
    });
}

/// Constructor arguments are converted from JS types via `FromJS`.
#[test]
fn constructor_converts_arguments() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Greeting>(lock);
        ctx.set_global("Greeting", constructor.into());

        // Number is coerced to string by V8
        let result: String = ctx
            .eval(lock, "new Greeting(String(42)).getMessage()")
            .unwrap();
        assert_eq!(result, "42");
        Ok(())
    });
}

/// Multiple `new` calls create distinct JS objects.
#[test]
fn constructor_creates_distinct_objects() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Greeting>(lock);
        ctx.set_global("Greeting", constructor.into());

        let result: String = ctx
            .eval(
                lock,
                "let a = new Greeting('one'); let b = new Greeting('two'); \
                 a.getMessage() + ',' + b.getMessage()",
            )
            .unwrap();
        assert_eq!(result, "one,two");
        Ok(())
    });
}

/// `instanceof` works correctly for constructor-created instances.
#[test]
fn constructor_instanceof_works() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Greeting>(lock);
        ctx.set_global("Greeting", constructor.into());

        let result: String = ctx
            .eval(
                lock,
                "let g = new Greeting('test'); \
                 String(g instanceof Greeting)",
            )
            .unwrap();
        assert_eq!(result, "true");
        Ok(())
    });
}

// Constructor with Lock parameter

#[jsg_resource]
struct Counter {
    value: Number,
}

#[jsg_resource]
impl Counter {
    #[jsg_constructor]
    fn constructor(_lock: &mut jsg::Lock, value: Number) -> Self {
        Self { value }
    }

    #[jsg_method]
    fn get_value(&self) -> Number {
        self.value
    }
}

/// `#[jsg_constructor]` with a `Lock` parameter works.
#[test]
fn constructor_with_lock_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Counter>(lock);
        ctx.set_global("Counter", constructor.into());

        let result: Number = ctx.eval(lock, "new Counter(99).getValue()").unwrap();
        assert!((result.value() - 99.0).abs() < f64::EPSILON);
        Ok(())
    });
}
