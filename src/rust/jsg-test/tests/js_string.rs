// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::FromJS;
use jsg::JsString;
use jsg::ToJS;
use jsg::Type;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Strings;

#[jsg_resource]
impl Strings {
    #[jsg_method]
    pub fn echo<'s>(&self, value: JsString<'s>) -> JsString<'s> {
        value
    }

    #[jsg_method]
    pub fn optional<'s>(&self, value: Option<JsString<'s>>) -> Option<JsString<'s>> {
        value
    }
}

#[test]
fn coerces_method_arguments() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.set_global(lock, "strings", jsg::Rc::new(Strings));
        for (expr, expected) in [
            ("''", ""),
            ("'hello'", "hello"),
            ("42", "42"),
            ("-0", "0"),
            ("true", "true"),
            ("1n", "1"),
            ("null", "null"),
            ("undefined", "undefined"),
            ("[1, 2]", "1,2"),
            ("new String('boxed')", "boxed"),
            ("({ toString() { return 'object'; } })", "object"),
            ("({ [Symbol.toPrimitive]() { return 'primitive'; } })", "primitive"),
        ] {
            let result = ctx.eval::<String>(lock, &format!("strings.echo({expr})")).unwrap();
            assert_eq!(result, expected, "{expr}");
        }
        assert!(ctx.eval::<bool>(lock, "strings.optional() === undefined").unwrap());
        assert!(ctx.eval::<bool>(lock, "strings.optional(null) === 'null'").unwrap());
        assert!(ctx.eval::<bool>(lock, "strings.optional(42) === '42'").unwrap());
        assert!(ctx.eval::<bool>(lock, "try { strings.echo(); false } catch (e) { e instanceof TypeError && e.message.includes('string') }").unwrap());
        Ok(())
    });
}

#[test]
fn preserves_utf16_and_exposes_string_operations() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let units = [0x61, 0, 0xd800, 0xdc00, 0xdfff];
        let handle = v8::String::new_from_two_byte(lock, &units).unwrap();
        let value: v8::Local<'_, v8::Value> = handle.into();
        let string = JsString::from_js(lock, value)?;
        assert_eq!(string.length(), 5);
        let mut written = [0; 5];
        string.write(lock, 0, 5, &mut written, v8::WriteFlags::None);
        assert_eq!(written, units);
        assert_eq!(string.to_js(lock)?, value);
        assert_eq!(v8::Local::<v8::String>::from(string), handle);
        assert_eq!(JsString::from(handle), string);
        assert_eq!(JsString::class_name(), "string");
        assert_eq!(JsString::type_name(), "string");
        assert!(JsString::is_exact(value));
        let number = 42_u32.to_js(lock)?;
        assert!(!JsString::is_exact(number));

        ctx.set_global(lock, "strings", jsg::Rc::new(Strings));
        assert!(
            ctx.eval::<bool>(
                lock,
                "strings.echo('a\\0\\ud800\\udc00\\udfff') === 'a\\0\\ud800\\udc00\\udfff'"
            )
            .unwrap()
        );
        Ok(())
    });
}

#[test]
fn propagates_coercion_exceptions() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        ctx.set_global(lock, "strings", jsg::Rc::new(Strings));
        assert!(
            ctx.eval::<bool>(
                lock,
                "try { strings.echo(Symbol()); false } catch (e) { e instanceof TypeError }"
            )
            .unwrap()
        );
        assert!(
            ctx.eval::<bool>(
                lock,
                "(() => {
            for (const thrown of [42, null, {}, new RangeError('boom')]) {
                let caught = false;
                try { strings.echo({ toString() { throw thrown; } }); }
                catch (e) { caught = e === thrown; }
                if (!caught) return false;
            }
            return true;
        })()"
            )
            .unwrap()
        );
        Ok(())
    });
}

#[jsg_oneof]
enum StringOrNumber<'s> {
    String(JsString<'s>),
    Number(f64),
}

#[test]
fn converts_in_the_string_union_pass() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        for (expr, expected) in [("true", "true"), ("null", "null"), ("'hello'", "hello")] {
            let value = ctx.eval_raw(lock, expr).unwrap();
            let StringOrNumber::String(string) = StringOrNumber::from_js(lock, value)? else {
                panic!("{expr} should convert to a string");
            };
            assert_eq!(string.to_string(lock), expected);
        }
        let value = ctx.eval_raw(lock, "42").unwrap();
        assert!(matches!(
            StringOrNumber::from_js(lock, value)?,
            StringOrNumber::Number(42.0)
        ));
        Ok(())
    });
}
