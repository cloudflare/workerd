// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_test::Harness;

use super::*;

/// Outside a request, `current()` fails with the error JavaScript sees from C++ APIs that need
/// a request, holding the value C++ threw, and `try_current()` returns `None`.
#[test]
fn outside_a_request_there_is_no_io_context() {
    let harness = Harness::new();
    harness.run_in_context(|lock, _ctx| {
        assert!(IoContext::try_current(lock).is_none());
        let Err(err) = IoContext::current(lock) else {
            panic!("there should be no IoContext outside a request");
        };
        assert_eq!(err.name, jsg::ExceptionType::Error);
        assert!(
            err.message
                .starts_with("Disallowed operation called within global scope."),
            "{}",
            err.message
        );
        assert!(err.exception(lock).is_some());
        Ok(())
    });
}
