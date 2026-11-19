// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `v8::Local<'s, v8::WasmModuleObject>`, a `WebAssembly.Module`, which converts as C++ JSG's
//! `v8::Local<v8::WasmModuleObject>` and `jsg::V8Ref<v8::WasmModuleObject>` do, and the
//! `v8::CompiledWasmModule` it holds.

use jsg::ExceptionType;
use jsg::FromJS;
use jsg::ToJS;
use jsg::ToJsTypedArray;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;

use crate::Harness;

/// `(module (func (export "answer") (result i32) i32.const 42))`.
const ANSWER_MODULE: &[u8] = &[
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic and version
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f, // type section: () -> i32
    0x03, 0x02, 0x01, 0x00, // function section: one function of type 0
    0x07, 0x0a, 0x01, 0x06, b'a', b'n', b's', b'w', b'e', b'r', 0x00, 0x00, // export "answer"
    0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b, // code section: i32.const 42
];

/// `kj::OneOf<jsg::V8Ref<v8::WasmModuleObject>, v8::Local<v8::ArrayBuffer>, kj::String>`.
#[jsg_oneof]
enum ModuleOrBufferOrText<'s> {
    Module(v8::Local<'s, v8::WasmModuleObject>),
    Buffer(v8::Local<'s, v8::ArrayBuffer>),
    Text(String),
}

/// A struct with a module field, as Worker Loader's `WorkerCode` has.
#[jsg_struct]
struct WasmCode {
    pub wasm: Option<v8::TracedGlobal<v8::WasmModuleObject>>,
}

#[jsg_resource]
struct WasmModules {}

#[jsg_resource]
impl WasmModules {
    #[jsg_method]
    pub fn echo<'s>(
        &self,
        module: v8::Local<'s, v8::WasmModuleObject>,
    ) -> v8::Local<'s, v8::WasmModuleObject> {
        module
    }
}

/// Defines the global `bytes`, a `Uint8Array` of `ANSWER_MODULE`, and allows compiling it.
fn define_bytes(lock: &mut jsg::Lock<'_>, ctx: &crate::EvalContext<'_>) {
    Harness::set_allow_eval(lock, true);
    let bytes = ANSWER_MODULE.to_js_typed_array(lock);
    ctx.set_global(lock, "bytes", bytes);
}

/// Compiles `ANSWER_MODULE` in JavaScript.
fn compile<'s>(lock: &mut jsg::Lock<'s>, ctx: &crate::EvalContext<'_>) -> v8::Local<'s, v8::Value> {
    define_bytes(lock, ctx);
    ctx.eval_raw(lock, "new WebAssembly.Module(bytes)").unwrap()
}

#[test]
fn module_compiled_in_javascript_converts() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = compile(lock, ctx);
        assert!(value.is_wasm_module_object());
        assert!(value.is_object());
        let module = v8::Local::<v8::WasmModuleObject>::from_js(lock, value)?;
        let object: v8::Local<v8::Object> = module.into();
        assert_eq!(v8::Local::<v8::Value>::from(object), value);
        assert!(v8::Local::<v8::WasmModuleObject>::try_from(object).is_ok());
        Ok(())
    });
}

#[test]
fn other_values_are_rejected() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        define_bytes(lock, ctx);
        for (code, type_of) in [
            ("bytes", "object"),
            ("bytes.buffer", "object"),
            ("({})", "object"),
            ("'module'", "string"),
            ("undefined", "undefined"),
            (
                "new WebAssembly.Instance(new WebAssembly.Module(bytes))",
                "object",
            ),
        ] {
            let value = ctx.eval_raw(lock, code).unwrap();
            assert!(!value.is_wasm_module_object(), "{code}");
            assert!(v8::Local::<v8::WasmModuleObject>::try_from(value).is_err());
            let err = v8::Local::<v8::WasmModuleObject>::from_js(lock, value).unwrap_err();
            assert_eq!(err.name, ExceptionType::TypeError, "{code}");
            assert_eq!(
                err.message,
                format!("Expected WasmModuleObject but got {type_of}")
            );
        }
        Ok(())
    });
}

#[test]
fn method_parameter_rejects_a_buffer() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        define_bytes(lock, ctx);
        let modules = jsg::Rc::new(lock, WasmModules {}).to_js(lock)?;
        ctx.set_global(lock, "modules", modules);
        let message: String = ctx
            .eval(
                lock,
                "try { modules.echo(bytes.buffer); 'ok' } catch (e) { `${e.name}: ${e.message}` }",
            )
            .unwrap();
        assert_eq!(
            message,
            "TypeError: Failed to execute 'echo' on 'WasmModules': parameter 1 is not of type \
             'WasmModuleObject'."
        );
        Ok(())
    });
}

/// Returning a module returns the object that was passed in, as C++ JSG does.
#[test]
fn to_js_preserves_identity() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        define_bytes(lock, ctx);
        let modules = jsg::Rc::new(lock, WasmModules {}).to_js(lock)?;
        ctx.set_global(lock, "modules", modules);
        let same: bool = ctx
            .eval(
                lock,
                "const m = new WebAssembly.Module(bytes); modules.echo(m) === m",
            )
            .unwrap();
        assert!(same);

        let value = compile(lock, ctx);
        let module = v8::Local::<v8::WasmModuleObject>::from_js(lock, value)?;
        assert_eq!(module.to_js(lock)?, value);
        let global = module.to_global(lock);
        assert_eq!(v8::Local::<v8::Value>::from(global.as_local(lock)), value);
        let traced = module.to_traced_global(lock);
        assert_eq!(traced.to_js(lock)?, value);
        Ok(())
    });
}

#[test]
fn union_tells_a_module_from_a_buffer() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        define_bytes(lock, ctx);
        let module = ctx.eval_raw(lock, "new WebAssembly.Module(bytes)").unwrap();
        match ModuleOrBufferOrText::from_js(lock, module)? {
            ModuleOrBufferOrText::Module(m) => assert_eq!(m.to_js(lock)?, module),
            _ => panic!("a module should convert to the module variant"),
        }
        let buffer = ctx.eval_raw(lock, "bytes.buffer").unwrap();
        assert!(matches!(
            ModuleOrBufferOrText::from_js(lock, buffer)?,
            ModuleOrBufferOrText::Buffer(_)
        ));
        let text = ctx.eval_raw(lock, "'export default 1'").unwrap();
        assert!(matches!(
            ModuleOrBufferOrText::from_js(lock, text)?,
            ModuleOrBufferOrText::Text(t) if t == "export default 1"
        ));
        Ok(())
    });
}

#[test]
fn struct_field_holds_a_module() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        define_bytes(lock, ctx);
        let value = ctx
            .eval_raw(lock, "({ wasm: new WebAssembly.Module(bytes) })")
            .unwrap();
        let code = WasmCode::from_js(lock, value)?;
        let module = code.wasm.expect("the field should be set").as_local(lock);
        assert_eq!(module.compiled_module().wire_bytes(), ANSWER_MODULE);

        let value = ctx.eval_raw(lock, "({ wasm: bytes })").unwrap();
        let Err(err) = WasmCode::from_js(lock, value) else {
            panic!("a Uint8Array is not a module");
        };
        assert_eq!(err.name, ExceptionType::TypeError);
        assert_eq!(
            err.message,
            "Incorrect type for the 'wasm' field on 'WasmCode': the provided value is not \
             of type 'WasmModuleObject'."
        );
        Ok(())
    });
}

#[test]
fn wire_bytes_are_the_compiled_bytes() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = compile(lock, ctx);
        let module = v8::Local::<v8::WasmModuleObject>::try_from(value).unwrap();
        let compiled = module.compiled_module();
        assert_eq!(compiled.wire_bytes(), ANSWER_MODULE);
        Ok(())
    });
}

/// The compiled module holds the code and the wire bytes by itself: it outlives the isolate that
/// compiled it, makes a module object in another isolate on another thread, and is freed by the
/// last drop there.
#[test]
fn compiled_module_outlives_its_isolate() {
    let mut compiled = None;
    {
        let harness = Harness::new();
        harness.run_in_context(|lock, ctx| {
            let value = compile(lock, ctx);
            let module = v8::Local::<v8::WasmModuleObject>::try_from(value).unwrap();
            compiled = Some((module.compiled_module(), module.compiled_module()));
            Ok(())
        });
    }
    // The isolate is gone, so these hold the only references to the compiled code.
    let (to_instantiate, to_drop) = compiled.unwrap();
    std::thread::spawn(move || {
        let harness = Harness::new();
        harness.run_in_context(|lock, ctx| {
            let module = Harness::wasm_module_object_from_compiled_module(lock, to_instantiate);
            ctx.set_global(lock, "module", module);
            let answer: u32 = ctx
                .eval(lock, "new WebAssembly.Instance(module).exports.answer()")
                .unwrap();
            assert_eq!(answer, 42);
            Ok(())
        });
        drop(harness);
        assert_eq!(to_drop.wire_bytes(), ANSWER_MODULE);
        drop(to_drop);
    })
    .join()
    .unwrap();
}

/// C++ makes a new module object from the compiled module, which runs the same code.
#[test]
fn compiled_module_is_shared_with_cpp() {
    let harness = Harness::new();
    harness.run_in_context(|lock, ctx| {
        let value = compile(lock, ctx);
        let module = v8::Local::<v8::WasmModuleObject>::try_from(value).unwrap();
        let shared =
            Harness::wasm_module_object_from_compiled_module(lock, module.compiled_module());
        assert_ne!(v8::Local::<v8::Value>::from(shared), value);
        assert_eq!(shared.compiled_module().wire_bytes(), ANSWER_MODULE);
        ctx.set_global(lock, "shared", shared);
        let answer: u32 = ctx
            .eval(lock, "new WebAssembly.Instance(shared).exports.answer()")
            .unwrap();
        assert_eq!(answer, 42);
        Ok(())
    });
}
