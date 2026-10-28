// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A method's argument errors match C++ JSG's (`TypeWrapper::unwrap()` in
//! `type-wrapper.h`): a missing required argument or a top-level type mismatch is
//! "Failed to execute 'm' on 'T': parameter n is not of type 'X'.", and errors
//! from deeper in the conversion are kept.

use jsg::ToJS;
use jsg_macros::jsg_method;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

#[jsg_struct]
#[derive(Debug)]
struct Point {
    pub x: u32,
}

#[jsg_oneof]
#[derive(Debug)]
enum Key {
    Text(String),
    Point(Point),
}

type Label = Option<String>;

#[jsg_resource]
struct Args {}

#[jsg_resource]
impl Args {
    #[jsg_method]
    pub fn take_count(&self, count: u32) -> f64 {
        f64::from(count)
    }

    #[jsg_method]
    pub fn take_two(&self, label: String, _count: u32) -> String {
        label
    }

    #[jsg_method]
    pub fn take_optional(&self, label: Option<String>) -> bool {
        label.is_some()
    }

    #[jsg_method]
    pub fn take_label(&self, label: Label) -> bool {
        label.is_some()
    }

    #[jsg_method(name = "renamed")]
    pub fn take_renamed(&self, _count: u32) {}

    #[jsg_method]
    pub fn take_point(&self, point: Point) -> f64 {
        f64::from(point.x)
    }

    #[jsg_method]
    pub fn take_key(&self, key: Key) -> String {
        match key {
            Key::Text(text) => text,
            Key::Point(point) => point.x.to_string(),
        }
    }
}

/// Runs `call` on a global `args` and returns `"Name: message"` for what it
/// throws, or `"ok"`.
fn error(lock: &mut jsg::Lock, ctx: &crate::EvalContext<'_>, call: &str) -> String {
    let code = format!("try {{ {call}; 'ok' }} catch (e) {{ `${{e.name}}: ${{e.message}}` }}");
    ctx.eval(lock, &code).unwrap()
}

#[test]
fn missing_required_argument() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let args = jsg::Rc::new(Args {}).to_js(lock)?;
        ctx.set_global(lock, "args", args);
        assert_eq!(
            error(lock, ctx, "args.takeCount()"),
            "TypeError: Failed to execute 'takeCount' on 'Args': parameter 1 is not of type \
             'unsigned integer'."
        );
        assert_eq!(
            error(lock, ctx, "args.takeTwo('a')"),
            "TypeError: Failed to execute 'takeTwo' on 'Args': parameter 2 is not of type \
             'unsigned integer'."
        );
        assert_eq!(
            error(lock, ctx, "args.renamed()"),
            "TypeError: Failed to execute 'renamed' on 'Args': parameter 1 is not of type \
             'unsigned integer'."
        );
        // Like an omitted Optional, an omitted Option is fine, even by alias.
        let omitted = "args.takeOptional() === false && args.takeLabel() === false";
        assert!(ctx.eval::<bool>(lock, omitted).unwrap());
        Ok(())
    });
}

#[test]
fn top_level_type_mismatch() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let args = jsg::Rc::new(Args {}).to_js(lock)?;
        ctx.set_global(lock, "args", args);
        assert_eq!(
            error(lock, ctx, "args.takePoint(5)"),
            "TypeError: Failed to execute 'takePoint' on 'Args': parameter 1 is not of type \
             'Point'."
        );
        assert_eq!(
            error(lock, ctx, "args.takeKey(5)"),
            "TypeError: Failed to execute 'takeKey' on 'Args': parameter 1 is not of type \
             'string or Point'."
        );
        Ok(())
    });
}

#[test]
fn deeper_errors_are_kept() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let args = jsg::Rc::new(Args {}).to_js(lock)?;
        ctx.set_global(lock, "args", args);
        let negative = "TypeError: The value cannot be converted because it is negative and \
                        this API expects a positive number.";
        assert_eq!(error(lock, ctx, "args.takeCount(-1)"), negative);
        assert_eq!(error(lock, ctx, "args.takePoint({ x: -1 })"), negative);
        Ok(())
    });
}
