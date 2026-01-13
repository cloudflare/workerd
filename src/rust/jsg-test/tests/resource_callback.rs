//! Tests for resource method callbacks.
//!
//! These tests ensure that resource methods can be called from JavaScript and that
//! the resource pointer is correctly unwrapped. This specifically validates that
//! V8's internal field embedder data type tags are used correctly when getting
//! aligned pointers from internal fields.

use std::cell::Cell;
use std::rc::Rc;

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
    pub fn get_counter(&self) -> f64 {
        f64::from(self.counter.get())
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
        let result: f64 = ctx.eval(lock, "resource.getCounter()").unwrap();
        assert!((result - 42.0).abs() < f64::EPSILON);

        // Test incrementCounter returns undefined (we just check it doesn't error)
        let _: Option<bool> = ctx.eval(lock, "resource.incrementCounter()").unwrap();
        assert_eq!(counter.get(), 43);

        // Test maybeName returns string for Some
        let result: String = ctx.eval(lock, "resource.maybeName()").unwrap();
        assert_eq!(result, "TestResource");
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
