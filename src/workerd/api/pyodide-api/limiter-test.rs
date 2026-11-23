// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::Harness;

use super::*;

// What evaluating `code` throws, as `name: message`, or "no error".
fn thrown_by(lock: &mut jsg::Lock<'_>, ctx: &jsg_test::EvalContext<'_>, code: &str) -> String {
    ctx.eval::<String>(
        lock,
        &format!("try {{ {code}; 'no error' }} catch (e) {{ `${{e.name}}: ${{e.message}}` }}"),
    )
    .unwrap()
}

#[test]
fn disabled_limiter_limits_nothing() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        let limiter = SimplePythonLimiter::disabled(lock);
        ctx.set_global(lock, "limiter", limiter);
        // Neither order nor repetition matters, and any snapshot type is accepted.
        assert_eq!(
            thrown_by(
                lock,
                ctx,
                "limiter.finishStartup(undefined); limiter.beginStartup(); \
                 limiter.beginStartup(); limiter.finishStartup('baseline'); \
                 limiter.finishStartup(null); limiter.finishStartup(7)",
            ),
            "no error"
        );
        Ok(())
    });
}

#[test]
fn finish_startup_converts_its_argument() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        let limiter = SimplePythonLimiter::disabled(lock);
        ctx.set_global(lock, "limiter", limiter);
        // As in C++, where it is a `kj::Maybe<kj::String>`, the snapshot type can't be omitted.
        assert_eq!(
            thrown_by(lock, ctx, "limiter.finishStartup()"),
            "TypeError: Failed to execute 'finishStartup' on 'SimplePythonLimiter': parameter 1 \
             is not of type 'string'."
        );
        // Converting it runs its `toString()`, and what that throws propagates.
        assert_eq!(
            thrown_by(
                lock,
                ctx,
                "limiter.finishStartup({ toString() { throw new RangeError('thrown'); } })",
            ),
            "RangeError: thrown"
        );
        assert_eq!(
            thrown_by(lock, ctx, "limiter.finishStartup(Symbol())"),
            "TypeError: Cannot convert a Symbol value to a string"
        );
        Ok(())
    });
}
