// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Tests for `jsg::Arguments<T>`, a rest parameter that takes the remaining arguments of a
//! method or constructor, like C++ JSG's `jsg::Arguments<T>`.

use jsg::v8;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

use crate::Harness;

#[jsg_resource]
struct Rest {
    prefix: String,
    parts: Vec<String>,
}

#[jsg_resource]
impl Rest {
    #[jsg_constructor]
    fn constructor(prefix: String, parts: jsg::Arguments<String>) -> Self {
        Self {
            prefix,
            parts: parts.into_inner(),
        }
    }

    #[jsg_property(readonly)]
    fn get_joined(&self) -> String {
        format!("{}{}", self.prefix, self.parts.join("+"))
    }

    #[jsg_method]
    fn sum(&self, first: u32, rest: jsg::Arguments<u32>) -> u32 {
        first + rest.iter().sum::<u32>()
    }

    #[jsg_method]
    fn count(rest: jsg::Arguments<v8::Local<'_, v8::Value>>) -> u32 {
        u32::try_from(rest.len()).unwrap()
    }

    /// The values themselves, in order.
    #[jsg_method]
    fn echo<'s>(
        this: jsg::Rc<Self>,
        _lock: &mut jsg::Lock<'s>,
        values: jsg::Arguments<v8::Local<'s, v8::Value>>,
    ) -> Vec<v8::Local<'s, v8::Value>> {
        let _ = this;
        values.into_inner()
    }

    /// Whether every argument is this resource.
    #[jsg_method]
    fn all_same(this: jsg::Rc<Self>, others: jsg::Arguments<jsg::Rc<Self>>) -> bool {
        others.iter().all(|other| *other == this)
    }

    /// An optional parameter before the rest parameter.
    #[jsg_method]
    fn scaled(&self, factor: Option<u32>, rest: jsg::Arguments<u32>) -> Vec<u32> {
        let factor = factor.unwrap_or(1);
        rest.into_iter().map(|value| value * factor).collect()
    }
}

fn eval(harness: &Harness, code: &str) -> String {
    let mut result = String::new();
    harness.run_in_context(|lock, ctx| {
        let rest = jsg::Rc::new(
            lock,
            Rest {
                prefix: String::new(),
                parts: Vec::new(),
            },
        );
        ctx.set_global(lock, "rest", rest);
        let constructor = jsg::resource::function_template_of::<Rest>(lock).unwrap();
        ctx.set_global(lock, "Rest", constructor);
        result = ctx.eval(lock, code).unwrap();
        Ok(())
    });
    result
}

#[test]
fn rest_parameter_takes_the_remaining_arguments() {
    let result = eval(
        &Harness::new(),
        "[rest.sum(1), rest.sum(1, 2, 3), Rest.count(), Rest.count(1, 'a', {}),
          JSON.stringify(rest.echo('a', 1, null, undefined)),
          rest.echo().length,
          JSON.stringify(rest.scaled()), JSON.stringify(rest.scaled(2, 1, 2)),
          JSON.stringify(rest.scaled(undefined, 3)),
          new Rest('p:').joined, new Rest('p:', 'a', 'b').joined].join(' ')",
    );
    assert_eq!(
        result,
        "1 6 0 3 [\"a\",1,null,null] 0 [] [2,4] [3] p: p:a+b"
    );
}

#[test]
fn rest_parameter_keeps_values_as_they_are() {
    let result = eval(
        &Harness::new(),
        "const object = {};
         const values = rest.echo(object, undefined, 1n);
         [values[0] === object, values[1] === undefined, typeof values[2], values.length].join()",
    );
    assert_eq!(result, "true,true,bigint,3");
}

#[test]
fn rest_argument_errors_name_the_rest_parameter() {
    let result = eval(
        &Harness::new(),
        "const errors = [rest.allSame(), rest.allSame(rest, rest)];
         for (const f of [() => rest.allSame(rest, {}), () => rest.allSame({}, rest),
                          () => rest.sum(1, 2, {})]) {
           try { f(); errors.push('no error'); } catch (e) { errors.push(`${e.name}: ${e.message}`); }
         }
         errors.join('\\n')",
    );
    let not_of_type = "TypeError: Failed to execute 'allSame' on 'Rest': parameter 1 is not of \
                       type 'Rest'.";
    assert_eq!(
        result.lines().collect::<Vec<_>>(),
        [
            "true",
            "true",
            not_of_type,
            not_of_type,
            // An error from deeper in the conversion is kept, as for any parameter.
            "TypeError: The value cannot be converted because it is not an integer.",
        ]
    );
}

#[test]
fn rest_parameter_does_not_count_towards_length() {
    let harness = Harness::with_flags(|mut f| f.set_spec_compliant_property_attributes(true));
    let result = eval(
        &harness,
        "[rest.sum.length, Rest.count.length, rest.echo.length, rest.scaled.length,
          Rest.length].join()",
    );
    assert_eq!(result, "1,0,0,0,1");
}
