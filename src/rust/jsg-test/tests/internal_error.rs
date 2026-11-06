// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An internal error reaches JavaScript without its message.

use jsg::FromJS;

#[test]
fn internal_error_is_redacted_when_thrown() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let thrown =
            lock.try_catch(|_lock| Err::<(), _>(jsg::Error::new_internal_error("secret detail")))?;
        let Err(exception) = thrown else {
            panic!("the internal error was not thrown");
        };
        let err = jsg::Error::from_js(lock, exception)?;
        assert_eq!(err.name, jsg::ExceptionType::Error);
        assert!(
            err.message.starts_with("internal error; reference = "),
            "{}",
            err.message
        );
        assert!(!err.message.contains("secret detail"), "{}", err.message);
        Ok(())
    });
}
