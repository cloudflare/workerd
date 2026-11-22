// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::Harness;

use super::*;

/// Outside a request, `current()` fails with the error that C++ `IoContext::current()` throws
/// there, and `try_current()` returns `None`.
#[test]
fn outside_a_request_there_is_no_io_context() {
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        assert!(IoContext::try_current(lock).is_none());
        let Err(err) = IoContext::current(lock) else {
            panic!("there should be no IoContext outside a request");
        };
        assert_eq!(err.name, jsg::ExceptionType::Error);
        assert_eq!(err.message, bridge::io_context_no_request_message());
        Ok(())
    });
}

/// `then()`, `catch()` and `then_or_catch()` fail unless their `IoContext` is the current one, as
/// C++ `IoContext::addFunctor()` does.
#[test]
fn registering_reactions_requires_the_io_context_to_be_current() {
    #[expect(
        clippy::trivially_copy_pass_by_ref,
        clippy::unnecessary_wraps,
        reason = "the signature of a reaction"
    )]
    fn reaction<'a>(
        lock: &mut jsg::Lock<'a>,
        _context: &IoContext<'a>,
        _data: &(),
        _value: v8::Local<'a, v8::Value>,
    ) -> jsg::Result<v8::Local<'a, v8::Value>> {
        Ok(lock.undefined())
    }

    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        // No request is in progress, so no IoContext is current.
        let context = IoContext {
            id: 1,
            _lock: PhantomData,
        };
        let undefined = lock.undefined();
        let promise = v8::Promise::resolved(lock, undefined)?;
        assert!(context.then(lock, promise, (), reaction).is_err());
        assert!(context.catch(lock, promise, (), reaction).is_err());
        assert!(
            context
                .then_or_catch(lock, promise, (), reaction, reaction)
                .is_err()
        );
        Ok(())
    });
}
