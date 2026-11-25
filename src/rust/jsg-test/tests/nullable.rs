// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Nullable<T>` is `T | null`, with Web IDL's `undefined`-to-`null` coercion.
//! `NullOrUndefined<T>` is `T | null | undefined`, and keeps the three apart.

use jsg::FromJS;
use jsg::Nullable;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

fn convert<'s, T: FromJS<'s>>(
    lock: &mut jsg::Lock<'s>,
    ctx: &crate::EvalContext<'_>,
    expr: &str,
) -> Result<T::ResultType, jsg::Error> {
    let value = ctx
        .eval_raw(lock, expr)
        .expect("expression should evaluate");
    T::from_js(lock, value)
}

/// Names which of the three states `value` is in.
fn describe(value: Option<Nullable<String>>) -> String {
    match value {
        Some(Nullable::Some(s)) => format!("some {s}"),
        Some(Nullable::Null) => "null".to_owned(),
        None => "undefined".to_owned(),
    }
}

/// Evaluates `expr` and returns its string, or `"Name: message"` for what it throws.
fn call(lock: &mut jsg::Lock, ctx: &crate::EvalContext<'_>, expr: &str) -> String {
    let code = format!("try {{ {expr} }} catch (e) {{ `${{e.name}}: ${{e.message}}` }}");
    ctx.eval(lock, &code).unwrap()
}

#[jsg_resource]
struct Args {}

#[jsg_resource]
impl Args {
    #[jsg_method]
    pub fn take_nullable(&self, value: Nullable<String>) -> String {
        describe(Some(value))
    }

    #[jsg_method]
    pub fn take_optional_nullable(&self, value: Option<Nullable<String>>) -> String {
        describe(value)
    }

    #[jsg_method]
    pub fn give_null(&self) -> Nullable<String> {
        Nullable::Null
    }

    #[jsg_method]
    pub fn give_undefined(&self) -> Option<Nullable<String>> {
        None
    }
}

#[jsg_struct]
#[derive(Debug, PartialEq)]
struct Fields {
    pub required: Nullable<String>,
    pub optional: Option<Nullable<String>>,
}

#[test]
fn nullable_reads_null_and_values() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(
            convert::<Nullable<String>>(lock, ctx, "null")?,
            Nullable::Null
        );
        assert_eq!(
            convert::<Nullable<String>>(lock, ctx, "'x'")?,
            Nullable::Some("x".to_owned())
        );
        Ok(())
    });
}

#[test]
fn nullable_coerces_undefined_to_null() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = convert::<Nullable<String>>(lock, ctx, "undefined")?;
        assert_eq!(value, Nullable::Null);
        assert!(value.to_js(lock)?.is_null());

        let undefined = ctx
            .eval_raw(lock, "undefined")
            .expect("expression should evaluate");
        assert_eq!(
            <Nullable<String> as FromJS>::try_from_js(lock, undefined)?,
            Some(Nullable::Null)
        );
        Ok(())
    });
}

#[test]
fn optional_nullable_keeps_null_and_undefined_apart() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(
            convert::<Option<Nullable<String>>>(lock, ctx, "undefined")?,
            None
        );
        assert_eq!(
            convert::<Option<Nullable<String>>>(lock, ctx, "null")?,
            Some(Nullable::Null)
        );
        assert_eq!(
            convert::<Option<Nullable<String>>>(lock, ctx, "'x'")?,
            Some(Nullable::Some("x".to_owned()))
        );
        Ok(())
    });
}

#[test]
fn nullable_to_js() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        assert!(Nullable::<String>::Null.to_js(lock)?.is_null());
        assert!(Nullable::Some("x".to_owned()).to_js(lock)?.is_string());
        assert!(None::<Nullable<String>>.to_js(lock)?.is_undefined());
        assert!(Some(Nullable::<String>::Null).to_js(lock)?.is_null());
        Ok(())
    });
}

#[test]
fn nullable_argument_is_required_but_accepts_undefined() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let args = jsg::Rc::new(lock, Args {}).to_js(lock)?;
        ctx.set_global(lock, "args", args);
        let not_of_type = "TypeError: Failed to execute 'takeNullable' on 'Args': parameter 1 \
                           is not of type 'string'.";
        assert_eq!(call(lock, ctx, "args.takeNullable()"), not_of_type);
        assert_eq!(call(lock, ctx, "args.takeNullable(undefined)"), "null");
        assert_eq!(call(lock, ctx, "args.takeNullable(null)"), "null");
        assert_eq!(call(lock, ctx, "args.takeNullable('x')"), "some x");

        assert_eq!(call(lock, ctx, "args.takeOptionalNullable()"), "undefined");
        assert_eq!(
            call(lock, ctx, "args.takeOptionalNullable(undefined)"),
            "undefined"
        );
        assert_eq!(call(lock, ctx, "args.takeOptionalNullable(null)"), "null");
        assert_eq!(call(lock, ctx, "args.takeOptionalNullable('x')"), "some x");

        assert!(
            ctx.eval::<bool>(
                lock,
                "args.giveNull() === null && args.giveUndefined() === undefined"
            )
            .unwrap()
        );
        Ok(())
    });
}

#[test]
fn nullable_struct_fields() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(
            convert::<Fields>(lock, ctx, "({ required: null })")?,
            Fields {
                required: Nullable::Null,
                optional: None,
            }
        );
        assert_eq!(
            convert::<Fields>(lock, ctx, "({ required: 'x', optional: null })")?,
            Fields {
                required: Nullable::Some("x".to_owned()),
                optional: Some(Nullable::Null),
            }
        );
        for expr in ["({})", "({ required: undefined, optional: undefined })"] {
            assert_eq!(
                convert::<Fields>(lock, ctx, expr)?,
                Fields {
                    required: Nullable::Null,
                    optional: None,
                }
            );
        }

        let value = Fields {
            required: Nullable::Null,
            optional: None,
        }
        .to_js(lock)?;
        let obj: v8::Local<'_, v8::Object> = value.try_into().unwrap();
        assert!(obj.get(lock, "required")?.is_null());
        assert!(!obj.has(lock, "optional")?);

        let value = Fields {
            required: Nullable::Null,
            optional: Some(Nullable::Null),
        }
        .to_js(lock)?;
        let obj: v8::Local<'_, v8::Object> = value.try_into().unwrap();
        assert!(obj.get(lock, "optional")?.is_null());
        Ok(())
    });
}

/// As in "maybes - don't substitute null" (value-test.c++): a value that `T` does not take is
/// null for a worker without `dont_substitute_null_on_type_error`, and a type error with it.
#[test]
fn nullable_type_mismatch_follows_dont_substitute_null_on_type_error() {
    type Strict = Nullable<jsg::NonCoercible<String>>;
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(convert::<Strict>(lock, ctx, "123")?, Nullable::Null);
        // A value that `T` takes, and null, convert as before.
        assert_eq!(
            convert::<Strict>(lock, ctx, "'x'")?,
            Nullable::Some(jsg::NonCoercible::new("x".to_owned()))
        );
        assert_eq!(convert::<Strict>(lock, ctx, "null")?, Nullable::Null);
        // Only a top-level mismatch is null: an element of the wrong type is still an error.
        let err =
            convert::<Nullable<Vec<jsg::NonCoercible<String>>>>(lock, ctx, "[1]").unwrap_err();
        assert_eq!(
            err.message,
            "Incorrect type for array element 0: the provided value is not of type 'string'."
        );
        // Each element of a sequence of nullables is converted the same way.
        assert_eq!(
            convert::<Vec<Strict>>(lock, ctx, "['a', 1]")?,
            vec![
                Nullable::Some(jsg::NonCoercible::new("a".to_owned())),
                Nullable::Null
            ]
        );
        Ok(())
    });

    let harness = crate::Harness::with_flags(|mut flags| flags.set_no_substitute_null(true));
    harness.run_in_context(|lock, ctx| {
        let err = convert::<Strict>(lock, ctx, "123").unwrap_err();
        assert_eq!(err.name, jsg::ExceptionType::TypeError);
        assert_eq!(err.message, "Expected a string value but got number");
        Ok(())
    });
}
