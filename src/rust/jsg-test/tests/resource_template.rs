// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for resource template shape that depends on compatibility flags.

use jsg_macros::jsg_constructor;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_static_constant;

use crate::Harness;

#[jsg_resource]
struct Sample {
    label: String,
}

#[jsg_resource]
impl Sample {
    #[jsg_static_constant]
    const VALUE: f64 = 1.0;

    #[jsg_constructor]
    fn constructor(label: String) -> Self {
        Self { label }
    }
}

fn eval_string(harness: &Harness, code: &str) -> String {
    let mut out = String::new();
    harness.run_in_context(|lock, ctx| {
        let constructor = jsg::resource::function_template_of::<Sample>(lock);
        ctx.set_global(lock, "Sample", constructor);
        out = ctx.eval(lock, code).unwrap();
        Ok(())
    });
    out
}

#[test]
fn to_string_tag_follows_set_to_string_tag() {
    let code = "Object.prototype.toString.call(new Sample('x'))";
    assert_eq!(eval_string(&Harness::new(), code), "[object Object]");

    let harness = Harness::with_flags(|mut flags| flags.set_set_to_string_tag(true));
    assert_eq!(eval_string(&harness, code), "[object Sample]");
}

#[test]
fn constant_descriptor_follows_spec_compliant_property_attributes() {
    let code = "JSON.stringify(Object.getOwnPropertyDescriptor(Sample.prototype, 'VALUE'))";
    assert_eq!(
        eval_string(&Harness::new(), code),
        r#"{"value":1,"writable":false,"enumerable":true,"configurable":true}"#
    );

    let harness =
        Harness::with_flags(|mut flags| flags.set_spec_compliant_property_attributes(true));
    assert_eq!(
        eval_string(&harness, code),
        r#"{"value":1,"writable":false,"enumerable":true,"configurable":false}"#
    );
}
