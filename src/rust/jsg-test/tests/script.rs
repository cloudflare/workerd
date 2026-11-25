// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Compiling JavaScript and WebAssembly from Rust: `v8::Script`, `v8::Function::compile` and
//! `v8::WasmModuleObject::compile`, and the isolate's eval setting.

use jsg::FromJS;
use jsg::v8;

use crate::Harness;

/// `(module (func (export "answer") (result i32) i32.const 42))`.
const ANSWER_MODULE: &[u8] = &[
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic and version
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f, // type section: () -> i32
    0x03, 0x02, 0x01, 0x00, // function section: one function of type 0
    0x07, 0x0a, 0x01, 0x06, b'a', b'n', b's', b'w', b'e', b'r', 0x00, 0x00, // export "answer"
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b, // code section: i32.const 42
];

fn string<'s>(lock: &mut jsg::Lock<'s>, value: &str) -> v8::Local<'s, v8::String> {
    v8::String::new_from_utf8(lock, value.as_bytes()).unwrap()
}

/// The message of the error `value`, as `String(value)` makes it.
fn message<'s>(lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) -> String {
    String::from_js(lock, value).unwrap()
}

#[test]
fn a_script_runs_in_the_global_scope_and_returns_its_completion_value() {
    Harness::new().run_in_context(|lock, ctx| {
        ctx.eval_raw(lock, "globalThis.base = 40;").unwrap();
        let source = string(lock, "const local = 2; base + local");
        let name = string(lock, "answer.js");
        let script = v8::Script::compile(lock, source, name)?;
        let result = script.run(lock)?;
        assert_eq!(u32::from_js(lock, result)?, 42);
        Ok(())
    });
}

#[test]
fn a_script_that_does_not_compile_leaves_a_syntax_error_pending() {
    Harness::new().run_in_context(|lock, _ctx| {
        let caught = lock.try_catch(|lock| {
            let source = string(lock, "(");
            let name = string(lock, "broken.js");
            v8::Script::compile(lock, source, name).map(|_| ())
        })?;
        let exception = caught.expect_err("the script must not compile");
        assert!(message(lock, exception).starts_with("SyntaxError"));
        Ok(())
    });
}

#[test]
fn what_a_script_throws_stays_pending() {
    Harness::new().run_in_context(|lock, _ctx| {
        let caught = lock.try_catch(|lock| {
            let source = string(lock, "throw new RangeError('thrown')");
            let name = string(lock, "throws.js");
            let script = v8::Script::compile(lock, source, name)?;
            script.run(lock).map(|_| ())
        })?;
        let exception = caught.expect_err("the script must throw");
        assert_eq!(message(lock, exception), "RangeError: thrown");
        Ok(())
    });
}

#[test]
fn a_compiled_function_takes_its_parameters_and_the_name_it_is_given() {
    Harness::new().run_in_context(|lock, ctx| {
        let source = string(lock, "return a * b;");
        let name = string(lock, "multiply");
        let params = [string(lock, "a"), string(lock, "b")];
        let function = v8::Function::compile(lock, source, name, &params)?;
        function.set_name(name);
        assert!(!v8::Local::<v8::Value>::from(function).is_async_function());
        ctx.set_global(lock, "multiply", v8::Local::<v8::Value>::from(function));
        assert_eq!(ctx.eval::<u32>(lock, "multiply(6, 7)").unwrap(), 42);
        assert_eq!(
            ctx.eval::<String>(lock, "multiply.name").unwrap(),
            "multiply"
        );
        assert_eq!(ctx.eval::<u32>(lock, "multiply.length").unwrap(), 2);
        Ok(())
    });
}

#[test]
fn a_function_body_that_does_not_compile_leaves_a_syntax_error_pending() {
    Harness::new().run_in_context(|lock, _ctx| {
        let caught = lock.try_catch(|lock| {
            let source = string(lock, "return (;");
            let name = string(lock, "broken");
            v8::Function::compile(lock, source, name, &[]).map(|_| ())
        })?;
        let exception = caught.expect_err("the function must not compile");
        assert!(message(lock, exception).starts_with("SyntaxError"));
        Ok(())
    });
}

#[test]
fn is_async_function_tells_async_functions_apart() {
    Harness::new().run_in_context(|lock, ctx| {
        let async_function = ctx.eval_raw(lock, "(async function () {})").unwrap();
        let function = ctx.eval_raw(lock, "(function () {})").unwrap();
        let object = ctx.eval_raw(lock, "({})").unwrap();
        assert!(async_function.is_async_function());
        assert!(!function.is_async_function());
        assert!(!object.is_async_function());
        Ok(())
    });
}

#[test]
fn a_wasm_module_compiles_from_its_bytes_while_eval_is_disallowed() {
    Harness::new().run_in_context(|lock, ctx| {
        assert!(!lock.is_eval_allowed());
        let module = v8::WasmModuleObject::compile(lock, ANSWER_MODULE)?;
        ctx.set_global(lock, "answerModule", v8::Local::<v8::Value>::from(module));
        assert_eq!(
            ctx.eval::<u32>(
                lock,
                "new WebAssembly.Instance(answerModule).exports.answer()"
            )
            .unwrap(),
            42
        );
        Ok(())
    });
}

#[test]
fn bytes_that_are_not_a_wasm_module_leave_a_compile_error_pending() {
    Harness::new().run_in_context(|lock, _ctx| {
        let caught =
            lock.try_catch(|lock| v8::WasmModuleObject::compile(lock, b"not wasm").map(|_| ()))?;
        let exception = caught.expect_err("the bytes must not compile");
        assert!(message(lock, exception).starts_with("CompileError"));
        Ok(())
    });
}

#[test]
fn set_allow_eval_turns_eval_on_and_off() {
    Harness::new().run_in_context(|lock, ctx| {
        assert!(!lock.is_eval_allowed());
        assert!(ctx.eval_raw(lock, "eval('1')").is_err());
        lock.set_allow_eval(true);
        assert!(lock.is_eval_allowed());
        assert_eq!(ctx.eval::<u32>(lock, "eval('1 + 1')").unwrap(), 2);
        lock.set_allow_eval(false);
        assert!(!lock.is_eval_allowed());
        assert!(ctx.eval_raw(lock, "eval('1')").is_err());
        Ok(())
    });
}

#[test]
fn is_starting_from_snapshot_is_true_only_in_an_isolate_started_from_one() {
    Harness::new().run_in_context(|lock, _ctx| {
        assert!(!lock.is_starting_from_snapshot());
        Ok(())
    });
    crate::snapshot_round_trip(
        |_lock, _ctx| Ok(()),
        |lock, _ctx| {
            assert!(lock.is_starting_from_snapshot());
            Ok(())
        },
    )
    .unwrap();
}
