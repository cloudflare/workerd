// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Boolean` handles, the counterpart of `v8::Boolean`.

use jsg::ExceptionType;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Booleans;

#[jsg_resource]
impl Booleans {
    #[jsg_method]
    pub fn negate(&self, value: v8::Local<'_, v8::Boolean>) -> bool {
        !value.value()
    }
}

#[test]
fn boolean_handles() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let yes = v8::Boolean::new(lock, true);
        assert!(yes.value());
        let no = v8::Boolean::new(lock, false);
        assert!(!no.value());

        let value: v8::Local<v8::Value> = yes.into();
        assert!(value.is_boolean());
        assert!(!value.is_object());
        assert!(!value.is_number());
        assert_eq!(value, ctx.eval_raw(lock, "true").unwrap());

        let from_js = ctx.eval_raw(lock, "false").unwrap();
        assert!(!v8::Local::<v8::Boolean>::try_from(from_js).unwrap().value());

        // A Boolean object is not a boolean, nor is anything that coerces to one.
        for expr in ["new Boolean(false)", "1", "'true'", "null", "undefined"] {
            let not_boolean = ctx.eval_raw(lock, expr).unwrap();
            assert!(
                v8::Local::<v8::Boolean>::try_from(not_boolean).is_err(),
                "{expr}"
            );
        }
        Ok(())
    });
}

/// A `Local<Boolean>` parameter takes a boolean as it is, without `ToBoolean`.
#[test]
fn boolean_handle_parameter_does_not_coerce() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let resource = jsg::Rc::new(Booleans).to_js(lock)?;
        ctx.set_global(lock, "r", resource);

        assert!(!ctx.eval::<bool>(lock, "r.negate(true)").unwrap());
        assert!(ctx.eval::<bool>(lock, "r.negate(false)").unwrap());

        let err = ctx
            .eval::<bool>(lock, "r.negate(1)")
            .unwrap_err()
            .unwrap_jsg_err(lock);
        assert_eq!(err.name, ExceptionType::TypeError);
        assert!(err.message.contains("Boolean"), "{}", err.message);
        Ok(())
    });
}
