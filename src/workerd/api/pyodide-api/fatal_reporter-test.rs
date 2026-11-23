// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::Harness;

use super::*;

#[test]
fn reports_outside_a_request_do_nothing() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        let reporter = jsg::Rc::new(lock, WorkerFatalReporter);
        ctx.set_global(lock, "reporter", reporter);
        let returned = ctx
            .eval::<bool>(
                lock,
                "reporter.reportFatal('RuntimeError: unreachable') === undefined && \
                 reporter.reportFatal({ toString() { return 'converted'; } }) === undefined && \
                 reporter.reportFatal(undefined) === undefined && \
                 reporter.reportPythonWorkersInternalError() === undefined",
            )
            .unwrap();
        assert!(returned);
        Ok(())
    });
}

#[test]
fn report_fatal_converts_its_argument() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        let reporter = jsg::Rc::new(lock, WorkerFatalReporter);
        ctx.set_global(lock, "reporter", reporter);
        // As for a C++ `kj::String` parameter, converting the error runs its `toString()`, and
        // what that throws propagates.
        let message = ctx
            .eval::<String>(
                lock,
                "try { reporter.reportFatal({ toString() { throw new Error('thrown'); } }); \
                 'no error' } catch (e) { e.message }",
            )
            .unwrap();
        assert_eq!(message, "thrown");
        let name = ctx
            .eval::<String>(
                lock,
                "try { reporter.reportFatal(Symbol()); 'no error' } catch (e) { e.name }",
            )
            .unwrap();
        assert_eq!(name, "TypeError");
        // As in C++, the error can't be omitted.
        let message = ctx
            .eval::<String>(
                lock,
                "try { reporter.reportFatal(); 'no error' } catch (e) { `${e.name}: ${e.message}` }",
            )
            .unwrap();
        assert_eq!(
            message,
            "TypeError: Failed to execute 'reportFatal' on 'WorkerFatalReporter': parameter 1 is \
             not of type 'string'."
        );
        Ok(())
    });
}
