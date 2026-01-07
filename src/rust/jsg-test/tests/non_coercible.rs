use jsg::ExceptionType;
use jsg::NonCoercible;
use jsg::ResourceState;
use jsg::ResourceTemplate;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct MyResource {
    _state: ResourceState,
}

#[jsg_resource]
#[expect(clippy::unnecessary_wraps)]
impl MyResource {
    #[jsg_method]
    pub fn string(&self, nc: NonCoercible<String>) -> Result<String, jsg::Error> {
        Ok(nc.as_ref().clone())
    }

    #[jsg_method]
    pub fn boolean(&self, nc: NonCoercible<bool>) -> Result<bool, jsg::Error> {
        Ok(*nc.as_ref())
    }

    #[jsg_method]
    pub fn number(&self, nc: NonCoercible<f64>) -> Result<f64, jsg::Error> {
        Ok(*nc.as_ref())
    }
}

#[test]
fn non_coercible_string_new_and_as_ref() {
    let nc: NonCoercible<String> = NonCoercible::new("hello".to_owned());
    assert_eq!(nc.as_ref(), "hello");
}

#[test]
fn non_coercible_bool_new_and_as_ref() {
    let nc_true: NonCoercible<bool> = NonCoercible::new(true);
    let nc_false: NonCoercible<bool> = NonCoercible::new(false);
    assert!(*nc_true.as_ref());
    assert!(!*nc_false.as_ref());
}

#[test]
fn non_coercible_f64_new_and_as_ref() {
    let nc: NonCoercible<f64> = NonCoercible::new(2.5);
    assert!((*nc.as_ref() - 2.5).abs() < f64::EPSILON);
}

#[test]
fn non_coercible_from_trait() {
    let nc_string: NonCoercible<String> = "test".to_owned().into();
    assert_eq!(nc_string.as_ref(), "test");

    let nc_bool: NonCoercible<bool> = true.into();
    assert!(*nc_bool.as_ref());

    let nc_f64: NonCoercible<f64> = 42.0.into();
    assert!((*nc_f64.as_ref() - 42.0).abs() < f64::EPSILON);
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
    assert_eq!(nc1.as_ref(), nc2.as_ref());
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
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(MyResource {
            _state: ResourceState::default(),
        });
        let mut template = MyResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("resource", wrapped);

        // String method accepts string
        let result: String = ctx.eval(lock, "resource.string('hello')").unwrap();
        assert_eq!(result, "hello");

        // String method rejects number
        let err = ctx
            .eval::<String>(lock, "resource.string(123)")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::TypeError);
        assert!(err.message.contains("string"));

        // Boolean method accepts boolean
        let result: bool = ctx.eval(lock, "resource.boolean(true)").unwrap();
        assert!(result);

        // Boolean method rejects string
        let err = ctx
            .eval::<bool>(lock, "resource.boolean('true')")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::TypeError);
        assert!(err.message.contains("bool"));

        // Number method accepts number
        let result: f64 = ctx.eval(lock, "resource.number(42.5)").unwrap();
        assert!((result - 42.5).abs() < f64::EPSILON);

        // Number method rejects string
        let err = ctx
            .eval::<f64>(lock, "resource.number('42')")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::TypeError);
        assert!(err.message.contains("number"));
        Ok(())
    });
}
