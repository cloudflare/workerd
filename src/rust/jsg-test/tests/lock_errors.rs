// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The errors a `Lock` creates and throws reach JavaScript as errors of their type, with their
//! message.

use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Thrower;

#[jsg_resource]
impl Thrower {
    #[jsg_method]
    fn throw_error(&self, lock: &mut jsg::Lock, message: String) {
        lock.throw_error(&message);
    }

    #[jsg_method]
    fn throw_invalid_string_length(&self, lock: &mut jsg::Lock) {
        lock.throw_exception(&jsg::Error::invalid_string_length());
    }

    #[jsg_method]
    fn range_error<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        message: String,
    ) -> jsg::v8::Local<'s, jsg::v8::Object> {
        lock.range_error(&message)
    }
}

fn caught(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>, call: &str, check: &str) -> bool {
    ctx.eval::<bool>(
        lock,
        &format!(
            "{{ let caught;
               try {{ {call}; }} catch (e) {{ caught = e; }}
               {check} }}"
        ),
    )
    .unwrap()
}

#[test]
fn throw_error_throws_an_error_with_the_message() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        ctx.set_global(lock, "thrower", jsg::Rc::new(Thrower));
        assert!(caught(
            lock,
            ctx,
            "thrower.throwError('boom')",
            "caught.constructor === Error && caught.message === 'boom'"
        ));
        // A message with a multi-byte character reaches JavaScript whole.
        assert!(caught(
            lock,
            ctx,
            "thrower.throwError('héllo')",
            "caught.message === 'héllo'"
        ));
        Ok(())
    });
}

#[test]
fn invalid_string_length_is_v8s_range_error() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        ctx.set_global(lock, "thrower", jsg::Rc::new(Thrower));
        assert!(caught(
            lock,
            ctx,
            "thrower.throwInvalidStringLength()",
            "caught instanceof RangeError && caught.message === 'Invalid string length'"
        ));
        // The same error V8 throws for a string it cannot create.
        assert!(caught(
            lock,
            ctx,
            "'x'.repeat(2 ** 30)",
            "caught instanceof RangeError && caught.message === 'Invalid string length'"
        ));
        Ok(())
    });
}

#[test]
fn range_error_is_a_range_error_object() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        ctx.set_global(lock, "thrower", jsg::Rc::new(Thrower));
        assert!(
            ctx.eval::<bool>(
                lock,
                "const e = thrower.rangeError('out of range');
                 e instanceof RangeError && e.message === 'out of range'"
            )
            .unwrap()
        );
        Ok(())
    });
}
