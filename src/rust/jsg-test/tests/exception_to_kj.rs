// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `Lock::exception_to_kj`, checked against C++ JSG's `jsg::Lock::exceptionToKj()`: the type,
//! description and details are the same, and so is what JavaScript sees it read. Each records
//! where it creates the exception, so the locations differ.

use jsg::v8;

use crate::EvalContext;
use crate::Harness;

/// Defines the global `DOMException`, which the test context lacks, as the constructor of a
/// `DOMException` that Rust JSG creates.
fn define_dom_exception(lock: &mut jsg::Lock<'_>, ctx: &EvalContext<'_>) {
    let exception = lock.dom_exception("Error", "").unwrap();
    ctx.set_global(lock, "DOMException", exception);
    eval(
        lock,
        ctx,
        "globalThis.DOMException = DOMException.constructor",
    );
}

fn eval<'s>(
    lock: &mut jsg::Lock<'s>,
    ctx: &EvalContext<'_>,
    expr: &str,
) -> v8::Local<'s, v8::Value> {
    ctx.eval_raw(lock, expr)
        .unwrap_or_else(|_| panic!("{expr} should evaluate"))
}

/// Converts the value of `expr` with Rust JSG and C++ JSG, checks that the exceptions are the
/// same, and returns Rust JSG's.
fn assert_same(lock: &mut jsg::Lock<'_>, ctx: &EvalContext<'_>, expr: &str) -> cxx::KjError {
    define_dom_exception(lock, ctx);
    let value = eval(lock, ctx, expr);
    assert_same_value(lock, value, expr)
}

/// Converts `value`, which `expr` describes, with Rust JSG and C++ JSG, checks that the
/// exceptions are the same, and returns Rust JSG's.
pub(super) fn assert_same_value<'s>(
    lock: &mut jsg::Lock<'s>,
    value: v8::Local<'s, v8::Value>,
    expr: &str,
) -> cxx::KjError {
    let rust = lock.exception_to_kj(value).unwrap();
    let cpp = Harness::cpp_exception_to_kj(lock, value).unwrap();
    assert_eq!(rust.exception_type(), cpp.exception_type(), "{expr}");
    // Rust reads the description of C++'s exception as a C string, up to its first NUL.
    assert_eq!(
        rust.description().split('\0').next(),
        Some(cpp.description()),
        "{expr}"
    );
    assert_eq!(
        rust.details().cloned().unwrap_or_default(),
        cpp.details().cloned().unwrap_or_default(),
        "{expr}"
    );
    rust
}

const VALUES: &[&str] = &[
    "new Error('boom')",
    "new TypeError('boom')",
    "new SyntaxError('boom')",
    "new AggregateError([new Error('a')], 'boom')",
    "new DOMException('boom', 'AbortError')",
    "new DOMException('boom')",
    "new (class extends DOMException {})('boom', 'DataError')",
    "Object.assign(new DOMException('boom', 'DataError'), { overloaded: true })",
    "Object.assign(new Error('boom'), { name: 'CustomError' })",
    "Object.assign(new Error('boom'), { name: 42 })",
    "new (class Custom extends Error {})('boom')",
    "Object.assign(new Error('boom'), { overloaded: true })",
    "Object.assign(new Error('boom'), { retryable: 1 })",
    "Object.assign(new Error('boom'), { overloaded: 0, retryable: 'yes' })",
    "Object.assign(new Error('boom'), { code: 7, cause: new TypeError('cause') })",
    "Object.assign(new Error('boom'), { stack: 'x'.repeat(5000) })",
    "Object.assign(new Error('boom'), { name: '\u{e9}'.repeat(3000) })",
    "Object.assign(new Error('boom'), { name: 'a' + '\u{e9}'.repeat(3000) })",
    "Object.assign(new Error('boom'), { stack: 'a' + '\u{20ac}'.repeat(2000) })",
    "new Error('a' + '\u{1f600}'.repeat(2000))",
    "Object.assign(new Error('boom'), { message: 42 })",
    "new Error('a\\0b')",
    "Object.assign(new Error('boom'), { name: 'A\\0B' })",
    "new DOMException('a\\0b', 'Data\\0Error')",
    "Object.defineProperty(new DOMException('boom', 'DataError'), 'name', { value: 'Other' })",
    "Object.defineProperty(new DOMException('boom', 'DataError'), 'message', { value: 'other' })",
    "undefined",
    "null",
    "1",
    "'TypeError: a string'",
    "'a\\0b'",
    "Symbol('s')",
    "10n",
    "({})",
    "({ message: 'not an error' })",
    "({ name: 'TypeError', message: 'not an error' })",
    "[1, 2]",
    "() => {}",
    "new Proxy(new Error('boom'), {})",
];

#[test]
fn matches_cpp_jsg() {
    Harness::new().run_in_context(|lock, ctx| {
        for expr in VALUES {
            assert_same(lock, ctx, expr);
        }
        Ok(())
    });
}

#[test]
fn matches_cpp_jsg_with_enhanced_error_serialization() {
    let harness = Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(true));
    harness.run_in_context(|lock, ctx| {
        for expr in VALUES {
            assert_same(lock, ctx, expr);
        }
        Ok(())
    });
}

#[test]
fn descriptions_and_types() {
    Harness::new().run_in_context(|lock, ctx| {
        for (expr, exception_type, description) in [
            (
                "new DOMException('boom', 'AbortError')",
                cxx::KjExceptionType::Failed,
                "jsg.DOMException(AbortError): boom",
            ),
            (
                "Object.assign(new DOMException('boom', 'DataError'), { overloaded: true })",
                cxx::KjExceptionType::Failed,
                "jsg.DOMException(DataError): boom",
            ),
            (
                "new SyntaxError('boom')",
                cxx::KjExceptionType::Failed,
                "jsg.SyntaxError: boom",
            ),
            (
                "Object.assign(new Error('boom'), { name: 'CustomError' })",
                cxx::KjExceptionType::Failed,
                "jsg.Error: CustomError: boom",
            ),
            (
                "Object.assign(new Error('boom'), { overloaded: true })",
                cxx::KjExceptionType::Overloaded,
                "jsg.Error: boom",
            ),
            (
                "Object.assign(new TypeError('boom'), { retryable: true })",
                cxx::KjExceptionType::Disconnected,
                "jsg.TypeError: boom",
            ),
            (
                "undefined",
                cxx::KjExceptionType::Failed,
                "jsg.Error: undefined",
            ),
            ("1", cxx::KjExceptionType::Failed, "jsg.Error: 1"),
            (
                "'TypeError: a string'",
                cxx::KjExceptionType::Failed,
                "jsg.Error: TypeError: a string",
            ),
            ("'a\\0b'", cxx::KjExceptionType::Failed, "jsg.Error: a"),
            ("({})", cxx::KjExceptionType::Failed, "jsg.Error: #<Object>"),
        ] {
            let exception = assert_same(lock, ctx, expr);
            assert_eq!(exception.exception_type(), exception_type, "{expr}");
            assert_eq!(exception.description(), description, "{expr}");
        }
        Ok(())
    });
}

#[test]
fn details() {
    let harness = Harness::with_flags(|mut flags| flags.set_enhanced_error_serialization(true));
    harness.run_in_context(|lock, ctx| {
        let ids = |exception: &cxx::KjError| -> Vec<u64> {
            exception
                .details()
                .map(|details| details.iter().map(|(id, _)| *id).collect())
                .unwrap_or_default()
        };
        let error = assert_same(lock, ctx, "new Error('boom')");
        assert_eq!(ids(&error).len(), 3);
        // A primitive has no metadata, and a function is not serializable.
        let primitive = assert_same(lock, ctx, "1");
        assert_eq!(ids(&primitive).len(), 2);
        let function = assert_same(lock, ctx, "() => {}");
        assert_eq!(ids(&function).len(), 1);
        let symbol = assert_same(lock, ctx, "Symbol('s')");
        assert!(ids(&symbol).is_empty());
        Ok(())
    });
}

#[test]
fn reads_properties_as_cpp_jsg_does() {
    Harness::new().run_in_context(|lock, ctx| {
        let expr = "globalThis.reads = []; \
                    new Proxy(new Error('boom'), { \
                      get(target, key, receiver) { \
                        reads.push(String(key)); \
                        return Reflect.get(target, key, receiver); \
                      } \
                    })";
        let value = eval(lock, ctx, expr);
        lock.exception_to_kj(value).unwrap();
        let rust_reads: String = ctx.eval(lock, "reads.join()").unwrap();
        eval(lock, ctx, "reads.length = 0");
        Harness::cpp_exception_to_kj(lock, value).unwrap();
        let cpp_reads: String = ctx.eval(lock, "reads.join()").unwrap();
        assert_eq!(rust_reads, cpp_reads);
        assert_eq!(rust_reads, "overloaded,retryable,name,stack,message");
        Ok(())
    });
}

/// A `DOMException`'s name and message are read without running their getters, as C++ reads them.
#[test]
fn reads_a_dom_exception_as_cpp_jsg_does() {
    Harness::new().run_in_context(|lock, ctx| {
        define_dom_exception(lock, ctx);
        let expr = "globalThis.reads = []; \
                    ['name', 'stack', 'message'].reduce((e, key) => \
                      Object.defineProperty(e, key, { \
                        get() { reads.push(key); return key; }, \
                      }), \
                      new DOMException('boom', 'DataError'))";
        let value = eval(lock, ctx, expr);
        lock.exception_to_kj(value).unwrap();
        let rust_reads: String = ctx.eval(lock, "reads.join()").unwrap();
        eval(lock, ctx, "reads.length = 0");
        Harness::cpp_exception_to_kj(lock, value).unwrap();
        let cpp_reads: String = ctx.eval(lock, "reads.join()").unwrap();
        assert_eq!(rust_reads, cpp_reads);
        assert!(
            !rust_reads.contains("name") && !rust_reads.contains("message"),
            "{rust_reads}"
        );
        Ok(())
    });
}

/// An error implemented in the per-isolate bootstrap, whose `name` and `message` are accessors, is
/// described by the name and message it holds, as C++ describes it.
#[test]
fn describes_an_error_by_the_name_and_message_it_holds_as_cpp_jsg_does() {
    Harness::new().run_in_context(|lock, ctx| {
        let cases = [
            ("TypeError", "boom", "jsg.TypeError: boom"),
            ("TypeError", "", "jsg.TypeError"),
            ("AbortError", "boom", "jsg.Error: AbortError: boom"),
        ];
        for (name, message, expected) in cases {
            let value = eval(lock, ctx, "new Error('other')");
            Harness::set_error_slots(lock, value, name, message);
            let exception = assert_same_value(lock, value, name);
            assert_eq!(exception.description(), expected);
        }
        Ok(())
    });
}

/// Converts the value of `expr`, which must throw, with Rust JSG and C++ JSG, and returns what
/// each threw, as `name: message`.
fn thrown(lock: &mut jsg::Lock<'_>, ctx: &EvalContext<'_>, expr: &str) -> [String; 2] {
    let value = eval(lock, ctx, expr);
    let rust = lock
        .try_catch(|lock| {
            let result = lock.exception_to_kj(value);
            assert!(result.as_ref().is_err_and(jsg::Error::is_pending), "{expr}");
            result
        })
        .unwrap()
        .expect_err(expr);
    let cpp = lock
        .try_catch(|lock| {
            let result = Harness::cpp_exception_to_kj(lock, value);
            assert!(result.as_ref().is_err_and(jsg::Error::is_pending), "{expr}");
            result
        })
        .unwrap()
        .expect_err(expr);
    [rust, cpp].map(|exception| {
        ctx.set_global(lock, "e", exception);
        ctx.eval::<String>(lock, "`${e.name}: ${e.message}`")
            .unwrap()
    })
}

#[test]
fn what_reading_a_property_throws_stays_pending() {
    Harness::new().run_in_context(|lock, ctx| {
        for (expr, expected) in [
            (
                "({ get overloaded() { throw new TypeError('overloaded') } })",
                "TypeError: overloaded",
            ),
            (
                "({ get retryable() { throw new TypeError('retryable') } })",
                "TypeError: retryable",
            ),
            (
                "Object.defineProperty(new Error('boom'), 'name', \
                   { get() { throw new RangeError('name') } })",
                "RangeError: name",
            ),
            (
                "Object.defineProperty(new Error('boom'), 'stack', \
                   { get() { throw new RangeError('stack') } })",
                "RangeError: stack",
            ),
            (
                "Object.defineProperty(new Error('boom'), 'message', \
                   { get() { throw new RangeError('message') } })",
                "RangeError: message",
            ),
        ] {
            assert_eq!(thrown(lock, ctx, expr), [expected, expected], "{expr}");
        }
        Ok(())
    });
}

#[test]
fn what_serializing_throws_is_caught() {
    Harness::new().run_in_context(|lock, ctx| {
        // Serializing reads `a`, whose getter throws, so only the metadata is left.
        let exception = assert_same(lock, ctx, "({ get a() { throw new Error('a') } })");
        assert_eq!(exception.description(), "jsg.Error: #<Object>");
        assert_eq!(exception.details().map(Vec::len), Some(1));
        Ok(())
    });
}
