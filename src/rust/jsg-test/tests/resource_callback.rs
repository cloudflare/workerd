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
use jsg::ResourceState;
use jsg::ResourceTemplate;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct EchoResource {
    _state: ResourceState,
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
    _state: ResourceState,
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
        let resource = jsg::Ref::new(EchoResource {
            _state: ResourceState::default(),
            prefix: "Hello, ".to_owned(),
        });
        let mut template = EchoResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
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
        let resource = jsg::Ref::new(EchoResource {
            _state: ResourceState::default(),
            prefix: ">> ".to_owned(),
        });
        let mut template = EchoResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
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
        let resource = jsg::Ref::new(EchoResource {
            _state: ResourceState::default(),
            prefix: "Hello, ".to_owned(),
        });
        let mut template = EchoResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
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
        let resource = jsg::Ref::new(DirectReturnResource {
            _state: ResourceState::default(),
            name: "TestResource".to_owned(),
            counter: counter.clone(),
        });
        let mut template = DirectReturnResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
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
struct MathResource {
    _state: ResourceState,
}

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
        let template = MathResourceTemplate::new(lock);
        let constructor = template.get_constructor().as_local_function(lock);
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
        let resource = jsg::Ref::new(MathResource {
            _state: ResourceState::default(),
        });
        let mut template = MathResourceTemplate::new(lock);

        // Expose the class constructor as a global
        let constructor = template.get_constructor().as_local_function(lock);
        ctx.set_global("MathResource", constructor.into());

        // Expose an instance as a global
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
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
        let template = MathResourceTemplate::new(lock);
        let constructor = template.get_constructor().as_local_function(lock);
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
        let template = MathResourceTemplate::new(lock);
        let constructor = template.get_constructor().as_local_function(lock);
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
        let resource = jsg::Ref::new(DirectReturnResource {
            _state: ResourceState::default(),
            name: String::new(),
            counter: Rc::new(Cell::new(0)),
        });
        let mut template = DirectReturnResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("resource", wrapped);

        let result: Option<String> = ctx.eval(lock, "resource.maybeName()").unwrap();
        assert!(result.is_none());
        Ok(())
    });
}
