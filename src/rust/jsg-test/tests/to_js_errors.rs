// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! A conversion to JavaScript that fails passes on its error: through the containers that hold
//! the value, and from a method's return value to the method's JavaScript caller, which catches
//! what JavaScript threw during the conversion.

use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

/// Converts to what calling `callback` returns, so its conversion runs JavaScript.
struct CallsWhenConverted {
    callback: v8::Global<v8::Value>,
}

impl<'s> ToJS<'s> for CallsWhenConverted {
    fn to_js(self, lock: &mut jsg::Lock<'s>) -> jsg::Result<v8::Local<'s, v8::Value>> {
        let callback = v8::Local::<v8::Function>::try_from(self.callback.as_local(lock))
            .map_err(|_| jsg::Error::new_type_error("expected a function"))?;
        callback.call::<v8::Local<v8::Value>>(lock, None::<v8::Local<v8::Value>>, &[])
    }
}

#[jsg_resource]
struct Converter;

impl Converter {
    fn later(lock: &mut jsg::Lock<'_>, callback: v8::Local<'_, v8::Value>) -> CallsWhenConverted {
        CallsWhenConverted {
            callback: v8::Global::new(lock, callback),
        }
    }
}

#[jsg_resource]
impl Converter {
    #[jsg_method]
    fn one(&self, lock: &mut jsg::Lock, callback: v8::Local<v8::Value>) -> CallsWhenConverted {
        Self::later(lock, callback)
    }

    #[jsg_method]
    fn fallible(
        &self,
        lock: &mut jsg::Lock,
        callback: v8::Local<v8::Value>,
    ) -> jsg::Result<CallsWhenConverted> {
        if !callback.is_function() {
            return Err(jsg::Error::new_type_error("expected a function"));
        }
        Ok(Self::later(lock, callback))
    }

    #[jsg_method]
    fn optional(
        &self,
        lock: &mut jsg::Lock,
        callback: v8::Local<v8::Value>,
    ) -> Option<CallsWhenConverted> {
        (!callback.is_undefined()).then(|| Self::later(lock, callback))
    }

    /// Converts to an array of what each callback returns.
    #[jsg_method]
    fn many(
        &self,
        lock: &mut jsg::Lock,
        first: v8::Local<v8::Value>,
        second: v8::Local<v8::Value>,
        third: v8::Local<v8::Value>,
    ) -> Vec<CallsWhenConverted> {
        vec![
            Self::later(lock, first),
            Self::later(lock, second),
            Self::later(lock, third),
        ]
    }
}

fn install(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    ctx.set_global(lock, "converter", jsg::Rc::new(Converter));
}

#[test]
fn converting_a_return_value_runs_javascript() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        assert_eq!(
            ctx.eval::<String>(lock, "converter.one(() => 'one')")
                .unwrap(),
            "one"
        );
        assert_eq!(
            ctx.eval::<String>(lock, "converter.fallible(() => 'fallible')")
                .unwrap(),
            "fallible"
        );
        assert_eq!(
            ctx.eval::<String>(lock, "converter.optional(() => 'optional')")
                .unwrap(),
            "optional"
        );
        assert_eq!(
            ctx.eval::<String>(
                lock,
                "converter.many(() => 'a', () => 'b', () => 'c').join()"
            )
            .unwrap(),
            "a,b,c"
        );
        Ok(())
    });
}

/// What JavaScript throws while a method's return value is converted reaches the method's
/// caller unchanged, whichever container holds the value.
#[test]
fn what_a_return_value_conversion_throws_reaches_the_caller() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        for call in [
            "converter.one(thrower)",
            "converter.fallible(thrower)",
            "converter.optional(thrower)",
            "converter.many(() => 'a', thrower, () => 'c')",
        ] {
            let script = format!(
                "const sentinel = {{ sentinel: true }};
                 const thrower = () => {{ throw sentinel; }};
                 let caught;
                 try {{ {call}; }} catch (e) {{ caught = e; }}
                 caught === sentinel"
            );
            assert!(
                ctx.eval::<bool>(lock, &format!("{{ {script} }}")).unwrap(),
                "{call}: the caller did not catch what the conversion threw"
            );
        }
        Ok(())
    });
}

/// Converting a `Vec` stops at the first element whose conversion fails.
#[test]
fn converting_a_vec_stops_at_the_first_error() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        install(lock, ctx);
        let script = "
            const calls = [];
            try {
              converter.many(
                  () => { calls.push('a'); return 'a'; },
                  () => { calls.push('b'); throw new Error('b'); },
                  () => { calls.push('c'); return 'c'; });
            } catch (e) {
              calls.push(e.message);
            }
            calls.join()";
        assert_eq!(ctx.eval::<String>(lock, script).unwrap(), "a,b,b");
        Ok(())
    });
}
