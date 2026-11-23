// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! An internal error reaches JavaScript without its message, which is logged instead, at `info`
//! for one from a KJ exception marked `worker_do_not_log`.

use jsg::FromJS;

/// How the description of the KJ exception that C++ `jsg::Deserializer` throws for an unsupported
/// header begins, `KJ_EXCEPTION(FAILED, "worker_do_not_log", message)`, after logging it itself.
const DO_NOT_LOG_DESCRIPTION: &str = "worker_do_not_log; message = Unable to deserialize cloned \
                                      data due to invalid or unsupported version.";

/// The description of a KJ exception that is logged when it reaches JavaScript.
const LOGGED_DESCRIPTION: &str = "an unexpected failure in C++";

const INTERNAL_ERROR_PREFIX: &str = "internal error; reference = ";

/// The error that a C++ call failing with a KJ exception with `description` gives Rust.
fn error_from_cpp(description: &str) -> jsg::Error {
    crate::fail_with_kj_exception(description)
        .expect_err("the C++ call should fail")
        .into()
}

/// How an internal error reaches JavaScript: thrown, or as a value, as a promise is rejected
/// with it.
#[derive(Debug, Clone, Copy)]
enum Delivery {
    Thrown,
    Value,
}

/// Delivers `err` to JavaScript as `delivery` says, and returns the message JavaScript gets, with
/// what was logged meanwhile.
fn deliver(
    lock: &mut jsg::Lock<'_>,
    err: &jsg::Error,
    delivery: Delivery,
) -> (String, Vec<String>) {
    let (exception, logs) = crate::capture_kj_logs(|| match delivery {
        Delivery::Thrown => lock
            .try_catch(|_lock| Err::<(), _>(err.clone()))
            .expect("nothing should be pending")
            .expect_err("the error should be thrown"),
        Delivery::Value => lock.exception(err).expect("the error should be created"),
    });
    let js = jsg::Error::from_js(lock, exception).expect("the exception should be an error");
    assert_eq!(js.name, jsg::ExceptionType::Error);
    (js.message, logs)
}

/// The reference ID in `message`, the message of an internal error that JavaScript gets.
fn reference_id(message: &str) -> &str {
    let id = message
        .strip_prefix(INTERNAL_ERROR_PREFIX)
        .unwrap_or_else(|| panic!("not an internal error: {message}"));
    assert!(!id.is_empty(), "{message}");
    id
}

#[test]
fn internal_error_from_kj_is_logged_with_its_reference_id() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let err = error_from_cpp(LOGGED_DESCRIPTION);
        for delivery in [Delivery::Thrown, Delivery::Value] {
            let (message, logs) = deliver(lock, &err, delivery);
            let id = reference_id(&message);
            assert!(
                logs.iter().any(|log| log.starts_with("error: ")
                    && log.contains(LOGGED_DESCRIPTION)
                    && log.contains(id)),
                "{delivery:?}: {logs:?}"
            );
        }
        Ok(())
    });
}

#[test]
fn internal_error_from_do_not_log_kj_exception_is_not_logged_as_an_error() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let err = error_from_cpp(DO_NOT_LOG_DESCRIPTION);
        for delivery in [Delivery::Thrown, Delivery::Value] {
            // JavaScript gets what it gets for any other internal error.
            let (message, logs) = deliver(lock, &err, delivery);
            let id = reference_id(&message);
            assert!(
                !logs.iter().any(|log| log.starts_with("error: ")),
                "{delivery:?}: {logs:?}"
            );
            // As C++ JSG logs it: at info, which only --verbose shows, and not under the ID.
            assert!(
                logs.iter()
                    .any(|log| log.starts_with("info: ") && log.contains(DO_NOT_LOG_DESCRIPTION)),
                "{delivery:?}: {logs:?}"
            );
            assert!(
                !logs.iter().any(|log| log.contains(id)),
                "{delivery:?}: {logs:?}"
            );
        }
        Ok(())
    });
}

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
            err.message.starts_with(INTERNAL_ERROR_PREFIX),
            "{}",
            err.message
        );
        assert!(!err.message.contains("secret detail"), "{}", err.message);
        Ok(())
    });
}
