//! Tests for resource method callbacks.
//!
//! These tests ensure that resource methods can be called from JavaScript and that
//! the resource pointer is correctly unwrapped. This specifically validates that
//! V8's internal field embedder data type tags are used correctly when getting
//! aligned pointers from internal fields.

use std::cell::Cell;
use std::rc::Rc;

use jsg::Lock;
use jsg::ResourceState;
use jsg::ResourceTemplate;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::EvalResult;

#[jsg_resource]
struct EchoResource {
    _state: ResourceState,
    prefix: String,
}

#[jsg_resource]
#[expect(clippy::unnecessary_wraps)]
impl EchoResource {
    #[jsg_method]
    pub fn echo(&self, message: String) -> Result<String, String> {
        Ok(format!("{}{}", self.prefix, message))
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
    harness.run_in_context(|isolate, ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);
        let resource = jsg::Ref::new(EchoResource {
            _state: ResourceState::default(),
            prefix: "Hello, ".to_owned(),
        });
        let mut template = EchoResourceTemplate::new(&mut lock);
        let wrapped = jsg::wrap_resource(&mut lock, resource, &mut template);
        ctx.set_global_safe("echoResource", wrapped.into_ffi());

        // Call the method from JavaScript
        assert_eq!(
            ctx.eval_safe("echoResource.echo('World!')"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "Hello, World!".to_owned(),
            }
        );
    });
}

/// Validates that multiple method calls work correctly on the same resource.
#[test]
fn resource_method_can_be_called_multiple_times() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);
        let resource = jsg::Ref::new(EchoResource {
            _state: ResourceState::default(),
            prefix: ">> ".to_owned(),
        });
        let mut template = EchoResourceTemplate::new(&mut lock);
        let wrapped = jsg::wrap_resource(&mut lock, resource, &mut template);
        ctx.set_global_safe("echo", wrapped.into_ffi());

        // First call
        assert_eq!(
            ctx.eval_safe("echo.echo('first')"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: ">> first".to_owned(),
            }
        );

        // Second call
        assert_eq!(
            ctx.eval_safe("echo.echo('second')"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: ">> second".to_owned(),
            }
        );
    });
}

/// Validates that methods can return values directly without Result wrapper.
#[test]
fn resource_method_returns_non_result_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);
        let counter = Rc::new(Cell::new(42));
        let resource = jsg::Ref::new(DirectReturnResource {
            _state: ResourceState::default(),
            name: "TestResource".to_owned(),
            counter: counter.clone(),
        });
        let mut template = DirectReturnResourceTemplate::new(&mut lock);
        let wrapped = jsg::wrap_resource(&mut lock, resource, &mut template);
        ctx.set_global_safe("resource", wrapped.into_ffi());

        assert_eq!(
            ctx.eval_safe("resource.getName()"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "TestResource".to_owned(),
            }
        );

        assert_eq!(
            ctx.eval_safe("resource.isValid()"),
            EvalResult {
                success: true,
                result_type: "boolean".to_owned(),
                result_value: "true".to_owned(),
            }
        );

        assert_eq!(
            ctx.eval_safe("resource.getCounter()"),
            EvalResult {
                success: true,
                result_type: "number".to_owned(),
                result_value: "42".to_owned(),
            }
        );

        assert_eq!(
            ctx.eval_safe("resource.incrementCounter()"),
            EvalResult {
                success: true,
                result_type: "undefined".to_owned(),
                result_value: "undefined".to_owned(),
            }
        );
        assert_eq!(counter.get(), 43);

        assert_eq!(
            ctx.eval_safe("resource.maybeName()"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "TestResource".to_owned(),
            }
        );
    });
}

/// Validates that Option<T> returns null for None.
#[test]
fn resource_method_returns_null_for_none() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);
        let resource = jsg::Ref::new(DirectReturnResource {
            _state: ResourceState::default(),
            name: String::new(),
            counter: Rc::new(Cell::new(0)),
        });
        let mut template = DirectReturnResourceTemplate::new(&mut lock);
        let wrapped = jsg::wrap_resource(&mut lock, resource, &mut template);
        ctx.set_global_safe("resource", wrapped.into_ffi());

        assert_eq!(
            ctx.eval_safe("resource.maybeName()"),
            EvalResult {
                success: true,
                result_type: "object".to_owned(),
                result_value: "null".to_owned(),
            }
        );
    });
}
