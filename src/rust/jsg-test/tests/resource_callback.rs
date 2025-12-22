//! Tests for resource method callbacks.
//!
//! These tests ensure that resource methods can be called from JavaScript and that
//! the resource pointer is correctly unwrapped. This specifically validates that
//! V8's internal field embedder data type tags are used correctly when getting
//! aligned pointers from internal fields.

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
    pub fn echo(&self, message: &str) -> Result<String, String> {
        Ok(format!("{}{}", self.prefix, message))
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
