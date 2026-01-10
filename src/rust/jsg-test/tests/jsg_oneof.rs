use jsg::ExceptionType;
use jsg::ResourceState;
use jsg::ResourceTemplate;
use jsg_macros::jsg_method;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_resource;

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum StringOrNumber {
    String(String),
    Number(f64),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum NumberOrString {
    Number(f64),
    String(String),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum StringOrBool {
    String(String),
    Bool(bool),
}

#[jsg_oneof]
#[derive(Debug, Clone, PartialEq)]
enum ThreeTypes {
    String(String),
    Number(f64),
    Bool(bool),
}

#[jsg_resource]
struct EnumTestResource {
    _state: ResourceState,
}

#[jsg_resource]
#[expect(clippy::unnecessary_wraps)]
impl EnumTestResource {
    #[jsg_method]
    pub fn string_or_number(&self, value: StringOrNumber) -> Result<String, jsg::Error> {
        match value {
            StringOrNumber::String(s) => Ok(format!("string:{s}")),
            StringOrNumber::Number(n) => Ok(format!("number:{n}")),
        }
    }

    #[jsg_method]
    pub fn string_or_number_ref(&self, value: &StringOrNumber) -> Result<String, jsg::Error> {
        match value {
            StringOrNumber::String(s) => Ok(format!("ref_string:{s}")),
            StringOrNumber::Number(n) => Ok(format!("ref_number:{n}")),
        }
    }

    #[jsg_method]
    pub fn string_or_bool(&self, value: StringOrBool) -> Result<String, jsg::Error> {
        match value {
            StringOrBool::String(s) => Ok(format!("string:{s}")),
            StringOrBool::Bool(b) => Ok(format!("bool:{b}")),
        }
    }

    #[jsg_method]
    pub fn three_types(&self, value: ThreeTypes) -> Result<String, jsg::Error> {
        match value {
            ThreeTypes::String(s) => Ok(format!("string:{s}")),
            ThreeTypes::Number(n) => Ok(format!("number:{n}")),
            ThreeTypes::Bool(b) => Ok(format!("bool:{b}")),
        }
    }

    #[jsg_method]
    pub fn number_or_string(&self, value: NumberOrString) -> Result<String, jsg::Error> {
        match value {
            NumberOrString::Number(n) => Ok(format!("number:{n}")),
            NumberOrString::String(s) => Ok(format!("string:{s}")),
        }
    }
}

#[test]
fn jsg_oneof_derives_debug_and_clone() {
    let s = StringOrNumber::String("test".to_owned());
    let cloned = s.clone();
    assert_eq!(s, cloned);

    let debug_str = format!("{s:?}");
    assert!(debug_str.contains("String"));
    assert!(debug_str.contains("test"));
}

#[test]
fn jsg_oneof_string_or_number_accepts_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(EnumTestResource {
            _state: ResourceState::default(),
        });
        let mut template = EnumTestResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("resource", wrapped);

        let result: String = ctx.eval(lock, "resource.stringOrNumber('hello')").unwrap();
        assert_eq!(result, "string:hello");
        Ok(())
    });
}

#[test]
fn jsg_oneof_string_or_number_accepts_number() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(EnumTestResource {
            _state: ResourceState::default(),
        });
        let mut template = EnumTestResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("resource", wrapped);

        let result: String = ctx.eval(lock, "resource.stringOrNumber(42)").unwrap();
        assert_eq!(result, "number:42");
        Ok(())
    });
}

#[test]
fn jsg_oneof_string_or_number_rejects_boolean() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(EnumTestResource {
            _state: ResourceState::default(),
        });
        let mut template = EnumTestResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("resource", wrapped);

        let err = ctx
            .eval::<String>(lock, "resource.stringOrNumber(true)")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::TypeError);
        assert!(err.message.contains("string"));
        assert!(err.message.contains("number"));
        Ok(())
    });
}

#[test]
fn jsg_oneof_string_or_bool_accepts_both() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(EnumTestResource {
            _state: ResourceState::default(),
        });
        let mut template = EnumTestResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("resource", wrapped);

        let result: String = ctx.eval(lock, "resource.stringOrBool('test')").unwrap();
        assert_eq!(result, "string:test");

        let result: String = ctx.eval(lock, "resource.stringOrBool(true)").unwrap();
        assert_eq!(result, "bool:true");
        Ok(())
    });
}

#[test]
fn jsg_oneof_three_types_accepts_all() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(EnumTestResource {
            _state: ResourceState::default(),
        });
        let mut template = EnumTestResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("resource", wrapped);

        let result: String = ctx.eval(lock, "resource.threeTypes('hello')").unwrap();
        assert_eq!(result, "string:hello");

        let result: String = ctx.eval(lock, "resource.threeTypes(123.5)").unwrap();
        assert_eq!(result, "number:123.5");

        let result: String = ctx.eval(lock, "resource.threeTypes(false)").unwrap();
        assert_eq!(result, "bool:false");
        Ok(())
    });
}

#[test]
fn jsg_oneof_three_types_rejects_null_and_undefined() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(EnumTestResource {
            _state: ResourceState::default(),
        });
        let mut template = EnumTestResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("resource", wrapped);

        let err = ctx
            .eval::<String>(lock, "resource.threeTypes(null)")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::TypeError);

        let err = ctx
            .eval::<String>(lock, "resource.threeTypes(undefined)")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::TypeError);
        Ok(())
    });
}

#[test]
fn jsg_oneof_variant_order_matches_declaration() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(EnumTestResource {
            _state: ResourceState::default(),
        });
        let mut template = EnumTestResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("resource", wrapped);

        // StringOrNumber has String first, Number second
        let result: String = ctx.eval(lock, "resource.stringOrNumber('42')").unwrap();
        assert_eq!(result, "string:42");

        let result: String = ctx.eval(lock, "resource.stringOrNumber(42)").unwrap();
        assert_eq!(result, "number:42");

        // NumberOrString has Number first, String second
        let result: String = ctx.eval(lock, "resource.numberOrString(42)").unwrap();
        assert_eq!(result, "number:42");

        let result: String = ctx.eval(lock, "resource.numberOrString('42')").unwrap();
        assert_eq!(result, "string:42");
        Ok(())
    });
}

#[test]
fn jsg_oneof_reference_parameter() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Ref::new(EnumTestResource {
            _state: ResourceState::default(),
        });
        let mut template = EnumTestResourceTemplate::new(lock);
        let wrapped = unsafe { jsg::wrap_resource(lock, resource, &mut template) };
        ctx.set_global("resource", wrapped);

        let result: String = ctx
            .eval(lock, "resource.stringOrNumberRef('hello')")
            .unwrap();
        assert_eq!(result, "ref_string:hello");

        let result: String = ctx.eval(lock, "resource.stringOrNumberRef(42)").unwrap();
        assert_eq!(result, "ref_number:42");
        Ok(())
    });
}
