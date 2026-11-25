// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `v8::ScriptCompiler::compile_function`, C++ `v8::ScriptCompiler::CompileFunction()`.

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;

fn compile<'s>(
    lock: &mut jsg::Lock<'s>,
    source: &str,
    params: &[&str],
    compile_cache: &[u8],
) -> jsg::Result<v8::Local<'s, v8::Function>> {
    let source = v8::String::new_from_str(lock, source).unwrap();
    v8::ScriptCompiler::compile_function(lock, source, "test:compiled", params, compile_cache)
}

#[test]
fn compiled_function_takes_its_parameters() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let function = compile(lock, "return a * 10 + b;", &["a", "b"], &[])?;
        let args = [4.to_js(lock)?, 2.to_js(lock)?];
        let result = function.call(lock, None, &args)?;
        assert_eq!(i32::from_js(lock, result)?, 42);
        Ok(())
    });
}

#[test]
fn compiled_function_reports_its_origin() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let function = compile(lock, "return new Error('here').stack;", &[], &[])?;
        let stack = function.call(lock, None, &[])?;
        let stack = String::from_js(lock, stack)?;
        assert!(stack.contains("test:compiled"), "{stack}");
        Ok(())
    });
}

#[test]
fn compile_error_is_left_pending() {
    crate::Harness::new().run_in_context(|lock, ctx| {
        let caught = lock.try_catch(|lock| compile(lock, "return (;", &[], &[]))?;
        let exception = caught.expect_err("the source does not compile");
        let error = jsg::Error::from_js(lock, exception)?;
        assert_eq!(error.name, ExceptionType::SyntaxError);
        // The TryCatch took the exception, so JavaScript runs again.
        assert!(ctx.eval::<bool>(lock, "true").unwrap());
        Ok(())
    });
}

#[test]
fn rejected_compile_cache_costs_only_the_compile() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        // Not a code cache, which V8's compatibility check refuses.
        let garbage = [0x5a_u8; 64];
        let function = compile(lock, "return x + 1;", &["x"], &garbage)?;
        let args = [1.to_js(lock)?];
        let result = function.call(lock, None, &args)?;
        assert_eq!(i32::from_js(lock, result)?, 2);
        let counts = crate::Harness::compile_cache_counts(lock);
        assert_eq!(
            counts,
            crate::CompileCacheCounts {
                found: 0,
                rejected: 1
            }
        );
        Ok(())
    });
}

#[test]
fn matching_compile_cache_is_used() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let source = "return x * 2;";
        let first = compile(lock, source, &["x"], &[])?;
        let cache = crate::Harness::create_code_cache(lock, first);
        let function = compile(lock, source, &["x"], &cache)?;
        let args = [21.to_js(lock)?];
        let result = function.call(lock, None, &args)?;
        assert_eq!(i32::from_js(lock, result)?, 42);
        let counts = crate::Harness::compile_cache_counts(lock);
        assert_eq!(
            counts,
            crate::CompileCacheCounts {
                found: 1,
                rejected: 0
            }
        );
        Ok(())
    });
}

#[test]
fn compile_cache_of_other_source_is_rejected_while_compiling() {
    crate::Harness::new().run_in_context(|lock, _ctx| {
        let other = compile(lock, "return x * 2;", &["x"], &[])?;
        let cache = crate::Harness::create_code_cache(lock, other);
        // V8 tells sources apart only by their length, so this one is longer.
        let function = compile(lock, "return x * 3 + 0;", &["x"], &cache)?;
        let args = [2.to_js(lock)?];
        let result = function.call(lock, None, &args)?;
        assert_eq!(i32::from_js(lock, result)?, 6);
        let counts = crate::Harness::compile_cache_counts(lock);
        assert_eq!(
            counts,
            crate::CompileCacheCounts {
                found: 0,
                rejected: 1
            }
        );
        Ok(())
    });
}
