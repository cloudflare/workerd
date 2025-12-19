use jsg::Lock;
use jsg::NonCoercible;
use jsg::ResourceState;
use jsg::ResourceTemplate;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

use crate::EvalResult;

#[jsg_resource]
struct MyResource {
    _state: ResourceState,
}

#[jsg_resource]
#[expect(clippy::unnecessary_wraps)]
impl MyResource {
    #[jsg_method]
    pub fn string(&self, nc: NonCoercible<String>) -> Result<String, String> {
        Ok(nc.value().clone())
    }

    #[jsg_method]
    pub fn boolean(&self, nc: NonCoercible<bool>) -> Result<bool, String> {
        Ok(*nc.value())
    }

    #[jsg_method]
    pub fn number(&self, nc: NonCoercible<f64>) -> Result<f64, String> {
        Ok(*nc.value())
    }
}

#[test]
fn non_coercible_string_new_and_value() {
    let nc: NonCoercible<String> = NonCoercible::new("hello".to_owned());
    assert_eq!(nc.value(), "hello");
}

#[test]
fn non_coercible_bool_new_and_value() {
    let nc_true: NonCoercible<bool> = NonCoercible::new(true);
    let nc_false: NonCoercible<bool> = NonCoercible::new(false);
    assert!(*nc_true.value());
    assert!(!*nc_false.value());
}

#[test]
fn non_coercible_f64_new_and_value() {
    let nc: NonCoercible<f64> = NonCoercible::new(2.5);
    assert!((*nc.value() - 2.5).abs() < f64::EPSILON);
}

#[test]
fn non_coercible_from_trait() {
    let nc_string: NonCoercible<String> = "test".to_owned().into();
    assert_eq!(nc_string.value(), "test");

    let nc_bool: NonCoercible<bool> = true.into();
    assert!(*nc_bool.value());

    let nc_f64: NonCoercible<f64> = 42.0.into();
    assert!((*nc_f64.value() - 42.0).abs() < f64::EPSILON);
}

#[test]
fn non_coercible_as_ref_trait() {
    let nc: NonCoercible<String> = NonCoercible::new("as_ref test".to_owned());
    let s: &String = nc.as_ref();
    assert_eq!(s, "as_ref test");

    let nc_bool: NonCoercible<bool> = NonCoercible::new(true);
    let b: &bool = nc_bool.as_ref();
    assert!(*b);
}

#[test]
fn non_coercible_deref_trait() {
    let nc: NonCoercible<String> = NonCoercible::new("deref test".to_owned());
    // Deref allows us to call String methods directly
    assert_eq!(nc.len(), 10);
    assert!(nc.starts_with("deref"));

    let nc_bool: NonCoercible<bool> = NonCoercible::new(true);
    // Deref to bool
    assert!(*nc_bool);

    let nc_f64: NonCoercible<f64> = NonCoercible::new(2.5);
    // Deref to f64
    assert!((*nc_f64) > 2.0);
}

#[test]
fn non_coercible_clone() {
    let nc1: NonCoercible<String> = NonCoercible::new("clone test".to_owned());
    let nc2 = nc1.clone();
    assert_eq!(nc1.value(), nc2.value());
    assert_eq!(*nc1, *nc2);
}

#[test]
fn non_coercible_partial_eq() {
    let nc1: NonCoercible<String> = NonCoercible::new("equal".to_owned());
    let nc2: NonCoercible<String> = NonCoercible::new("equal".to_owned());
    let nc3: NonCoercible<String> = NonCoercible::new("different".to_owned());

    assert_eq!(nc1, nc2);
    assert_ne!(nc1, nc3);

    let nc_bool1: NonCoercible<bool> = NonCoercible::new(true);
    let nc_bool2: NonCoercible<bool> = NonCoercible::new(true);
    let nc_bool3: NonCoercible<bool> = NonCoercible::new(false);

    assert_eq!(nc_bool1, nc_bool2);
    assert_ne!(nc_bool1, nc_bool3);
}

#[test]
fn non_coercible_debug() {
    let nc: NonCoercible<String> = NonCoercible::new("debug test".to_owned());
    let debug_str = format!("{nc:?}");
    assert!(debug_str.contains("NonCoercible"));
    assert!(debug_str.contains("debug test"));
}

#[test]
fn non_coercible_methods_accept_correct_types_and_reject_incorrect_types() {
    let harness = crate::Harness::new();
    harness.run_in_context(|isolate, ctx| unsafe {
        let mut lock = Lock::from_isolate_ptr(isolate);
        let resource = jsg::Ref::new(MyResource {
            _state: ResourceState::default(),
        });
        let mut template = MyResourceTemplate::new(&mut lock);
        let wrapped = jsg::wrap_resource(&mut lock, resource, &mut template);
        ctx.set_global_safe("resource", wrapped.into_ffi());

        // String method accepts string
        assert_eq!(
            ctx.eval_safe("resource.string('hello')"),
            EvalResult {
                success: true,
                result_type: "string".to_owned(),
                result_value: "hello".to_owned(),
            }
        );

        // String method rejects number
        let result = ctx.eval_safe("resource.string(123)");
        assert!(!result.success);
        assert!(result.result_value.contains("string"));

        // Boolean method accepts boolean
        assert_eq!(
            ctx.eval_safe("resource.boolean(true)"),
            EvalResult {
                success: true,
                result_type: "boolean".to_owned(),
                result_value: "true".to_owned(),
            }
        );

        // Boolean method rejects string
        let result = ctx.eval_safe("resource.boolean('true')");
        assert!(!result.success);
        assert!(result.result_value.contains("boolean"));

        // Number method accepts number
        assert_eq!(
            ctx.eval_safe("resource.number(42.5)"),
            EvalResult {
                success: true,
                result_type: "number".to_owned(),
                result_value: "42.5".to_owned(),
            }
        );

        // Number method rejects string
        let result = ctx.eval_safe("resource.number('42')");
        assert!(!result.success);
        assert!(result.result_value.contains("number"));
    });
}
