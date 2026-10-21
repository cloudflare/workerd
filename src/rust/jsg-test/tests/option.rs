// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Option<T>` conversion from JS matches C++ `jsg::Optional<T>`: only
//! `undefined` is `None`, and `null` goes to `T`.

use jsg::FromJS;

fn convert<T: FromJS>(
    lock: &mut jsg::Lock,
    ctx: &crate::EvalContext<'_>,
    expr: &str,
) -> Result<T::ResultType, jsg::Error> {
    let value = ctx.eval_raw(expr).expect("expression should evaluate");
    T::from_js(lock, value)
}

#[test]
fn option_reads_only_undefined_as_none() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(convert::<Option<String>>(lock, ctx, "undefined")?, None);
        assert_eq!(
            convert::<Option<String>>(lock, ctx, "'x'")?,
            Some("x".to_owned())
        );
        Ok(())
    });
}

#[test]
fn option_passes_null_to_inner_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        assert_eq!(
            convert::<Option<String>>(lock, ctx, "null")?,
            Some("null".to_owned())
        );
        assert_eq!(convert::<Option<u32>>(lock, ctx, "null")?, Some(0));
        assert_eq!(convert::<Option<bool>>(lock, ctx, "null")?, Some(false));
        Ok(())
    });
}

#[test]
fn option_try_from_js_passes_null_to_inner_type() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let null = ctx.eval_raw("null").expect("expression should evaluate");
        let result = <Option<String> as FromJS>::try_from_js(lock, null)?;
        assert_eq!(result, Some(Some("null".to_owned())));
        // A top-level mismatch in T stays a mismatch, not an error.
        let number = ctx.eval_raw("1").expect("expression should evaluate");
        let result = <Option<jsg::NonCoercible<String>> as FromJS>::try_from_js(lock, number)?;
        assert!(result.is_none());
        let undefined = ctx
            .eval_raw("undefined")
            .expect("expression should evaluate");
        assert_eq!(
            <Option<String> as FromJS>::try_from_js(lock, undefined)?,
            Some(None)
        );
        Ok(())
    });
}
