// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::FromJS;
use jsg::NullOrUndefined;
use jsg::ToJS;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_resource]
struct Args {}

#[jsg_resource]
impl Args {
    #[jsg_method]
    fn echo(&self, value: NullOrUndefined<String>) -> NullOrUndefined<String> {
        value
    }

    #[jsg_method]
    fn with_prefix(
        &self,
        _prefix: String,
        value: NullOrUndefined<String>,
    ) -> NullOrUndefined<String> {
        value
    }

    #[jsg_method]
    fn strict(&self, value: NullOrUndefined<jsg::NonCoercible<String>>) -> bool {
        value.is_some()
    }
}

#[jsg_struct]
#[derive(Debug, PartialEq)]
struct Fields {
    pub value: NullOrUndefined<String>,
}

#[jsg_resource]
struct Child {}

#[jsg_resource]
impl Child {}

#[jsg_resource]
struct Parent {
    child: NullOrUndefined<jsg::Rc<Child>>,
}

#[jsg_resource]
impl Parent {}

#[test]
fn gc_traces_the_contained_resource() {
    let harness = crate::Harness::new();
    let mut weak_child = None;
    harness.run_in_context(|lock, ctx| {
        let child = jsg::Rc::new(Child {});
        weak_child = Some(child.downgrade());
        let parent = jsg::Rc::new(Parent {
            child: NullOrUndefined::Some(child),
        });
        let wrapped = parent.to_js(lock)?;
        ctx.set_global(lock, "parent", wrapped);
        crate::Harness::request_gc(lock);
        assert!(weak_child.as_ref().unwrap().is_alive());
        Ok(())
    });
    // The parent global is scoped to its context; it and its child can now be collected.
    harness.run_in_context(|lock, _ctx| {
        crate::Harness::request_gc(lock);
        assert!(!weak_child.as_ref().unwrap().is_alive());
        for child in [NullOrUndefined::Null, NullOrUndefined::Undefined] {
            let parent = jsg::Rc::new(Parent { child });
            let _wrapped = parent.to_js(lock)?;
            crate::Harness::request_gc(lock);
        }
        Ok(())
    });
}

#[test]
fn conversion_preserves_all_states() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        for (expr, expected) in [
            ("'x'", NullOrUndefined::Some("x".to_owned())),
            ("null", NullOrUndefined::Null),
            ("undefined", NullOrUndefined::Undefined),
        ] {
            let input = ctx.eval_raw(lock, expr).unwrap();
            let value = NullOrUndefined::<String>::from_js(lock, input)?;
            assert_eq!(value, expected);
            let output = value.to_js(lock)?;
            ctx.set_global(lock, "output", output);
            assert!(
                ctx.eval::<bool>(lock, &format!("output === {expr}"))
                    .unwrap()
            );
            assert_eq!(
                NullOrUndefined::<String>::try_from_js(lock, input)?,
                Some(expected)
            );
        }
        // The inner conversion still coerces non-nullish values normally.
        let input = ctx.eval_raw(lock, "123").unwrap();
        assert_eq!(
            NullOrUndefined::<String>::from_js(lock, input)?,
            NullOrUndefined::Some("123".to_owned())
        );
        Ok(())
    });
}

#[test]
fn methods_accept_omission_and_report_inner_type_errors() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let args = jsg::Rc::new(Args {}).to_js(lock)?;
        ctx.set_global(lock, "args", args);
        assert!(ctx.eval::<bool>(lock, "args.echo() === undefined && args.echo(undefined) === undefined && args.echo(null) === null && args.echo('x') === 'x'").unwrap());
        assert_eq!(ctx.eval::<String>(lock, "try { args.strict(123) } catch(e) { e.message }").unwrap(),
            "Failed to execute 'strict' on 'Args': parameter 1 is not of type 'string'.");
        Ok(())
    });
}

#[test]
fn optional_arguments_do_not_count_toward_web_idl_length() {
    for enabled in [false, true] {
        let harness = crate::Harness::with_flags(|mut flags| {
            flags.set_spec_compliant_property_attributes(enabled);
        });
        harness.run_in_context(|lock, ctx| {
            let args = jsg::Rc::new(Args {}).to_js(lock)?;
            ctx.set_global(lock, "args", args);
            assert_eq!(
                ctx.eval::<u32>(lock, "args.withPrefix.length").unwrap(),
                u32::from(enabled)
            );
            assert_eq!(ctx.eval::<u32>(lock, "args.echo.length").unwrap(), 0);
            assert!(
                ctx.eval::<bool>(lock, "args.withPrefix('p') === undefined")
                    .unwrap()
            );
            Ok(())
        });
    }
}

#[test]
fn struct_fields_preserve_states_with_both_wrapping_modes() {
    for fast in [false, true] {
        let harness = crate::Harness::with_flags(|mut flags| flags.set_fast_jsg_struct(fast));
        harness.run_in_context(|lock, ctx| {
            for (expr, expected) in [
                ("({value: 'x'})", NullOrUndefined::Some("x".to_owned())),
                ("({value: null})", NullOrUndefined::Null),
                ("({value: undefined})", NullOrUndefined::Undefined),
                ("({})", NullOrUndefined::Undefined),
                ("undefined", NullOrUndefined::Undefined),
                ("null", NullOrUndefined::Undefined),
            ] {
                let input = ctx.eval_raw(lock, expr).unwrap();
                assert_eq!(
                    Fields::from_js(lock, input)?,
                    Fields {
                        value: expected.clone()
                    }
                );
                let output = Fields {
                    value: expected.clone(),
                }
                .to_js(lock)?;
                let obj: jsg::v8::Local<'_, jsg::v8::Object> = output.try_into().unwrap();
                assert_eq!(obj.has(lock, "value")?, fast || !expected.is_undefined());
                let property = obj.get(lock, "value")?;
                assert_eq!(
                    NullOrUndefined::<String>::from_js(lock, property)?,
                    expected
                );
            }
            Ok(())
        });
    }
}

#[test]
fn lenient_conversion_only_swallows_shallow_mismatches() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        type Strict = NullOrUndefined<jsg::NonCoercible<String>>;
        let input = ctx.eval_raw(lock, "123").unwrap();
        assert!(Strict::from_js(lock, input).is_err());
        assert!(Strict::try_from_js(lock, input)?.is_none());

        let lenient = jsg::Lenient::<Strict>::from_js(lock, input)?;
        let (value, original) = lenient.to_parts();
        assert!(value.is_undefined());
        assert!(original.is_some());
        let lenient = jsg::Lenient::<Strict>::from_js(lock, input)?;
        assert!(NullOrUndefined::from(lenient).is_undefined());

        let malformed = ctx.eval_raw(lock, "[123]").unwrap();
        assert!(jsg::Lenient::<NullOrUndefined<Vec<Fields>>>::from_js(lock, malformed).is_err());
        let throwing = ctx
            .eval_raw(lock, "({ toString() { throw new Error('coercion'); } })")
            .unwrap();
        let error = lock.try_catch(|lock| NullOrUndefined::<String>::from_js(lock, throwing));
        assert!(error?.is_err());
        Ok(())
    });
}
