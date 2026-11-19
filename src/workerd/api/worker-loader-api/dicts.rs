// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The JavaScript dictionaries of Worker Loader.

// `#[jsg_struct]` binds each field to a local of the same name in the code it generates.
#![allow(
    non_snake_case,
    reason = "a `#[jsg_struct]` field is named as its JavaScript property is"
)]

use channel_interop::Fetcher;
use jsg::v8;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_struct;

use crate::code::Bytes;
use crate::code::Wasm;

// Each struct is named as its C++ counterpart, which C++ JSG's conversion errors name, and the
// TypeScript name C++ generates is given by `ts_override`.

/// Limits on a stage of a Worker's resources, as C++ `workerd::ResourceLimits`.
#[jsg_struct]
pub struct ResourceLimits {
    pub cpuMs: Option<u32>,
    pub subRequests: Option<u32>,
}

/// The options of `WorkerStub.getEntrypoint()` and `getDurableObjectClass()`.
#[jsg_struct(ts_override = "WorkerStubEntrypointOptions")]
pub struct EntrypointOptions {
    pub props: Option<v8::TracedGlobal<v8::Object>>,
    pub limits: Option<ResourceLimits>,
}

/// A module given as an object, whose one field says what kind of module it is.
#[jsg_struct(
    ts_override = "WorkerLoaderModule { data?: ArrayBuffer; json?: any; wasm?: ArrayBuffer | \
                   ArrayBufferView | WebAssembly.Module; }"
)]
pub struct Module {
    /// An ES module.
    pub js: Option<String>,
    /// A `CommonJS` module.
    pub cjs: Option<String>,
    /// A text blob, which imports as a string.
    pub text: Option<String>,
    /// A byte blob, which imports as an `ArrayBuffer`.
    pub data: Option<Bytes>,
    /// Any JavaScript value, serialized to JSON and parsed again when imported.
    pub json: Option<v8::TracedGlobal<v8::Value>>,
    /// A Python module.
    pub py: Option<String>,
    /// A WASM module, as bytes to compile or an already-compiled `WebAssembly.Module`.
    pub wasm: Option<Wasm>,
}

/// What a module name maps to: a `WebAssembly.Module`, whose compiled code the Worker shares, a
/// module object, or a string whose kind the name gives.
#[jsg_oneof]
pub enum ModuleContent {
    Wasm(v8::TracedGlobal<v8::WasmModuleObject>),
    Module(Module),
    Text(String),
}

/// A dynamic Worker's code.
#[jsg_struct(
    ts_override = "WorkerLoaderWorkerCode { modules: Record<string, string | WebAssembly.Module \
                   | WorkerLoaderModule>; env?: any; globalOutbound?: Fetcher | null; tails?: \
                   Fetcher[]; streamingTails?: Fetcher[]; }"
)]
pub struct WorkerCode {
    pub compatibilityDate: String,
    pub compatibilityFlags: Option<Vec<String>>,
    pub allowExperimental: Option<bool>,
    pub limits: Option<ResourceLimits>,
    pub mainModule: String,
    pub modules: jsg::Dict<ModuleContent>,
    /// Any RPC-serializable object.
    pub env: Option<v8::TracedGlobal<v8::Object>>,
    /// The loaded Worker's global outbound: inherited from the caller if undefined, blocked if
    /// `null`, or, as C++ JSG converts a `kj::Maybe<jsg::Ref<Fetcher>>`, if it isn't a `Fetcher`
    /// and the `dont_substitute_null_on_type_error` compatibility flag is off.
    pub globalOutbound: jsg::NullOrUndefined<jsg_cpp::Ref<Fetcher>>,
    pub tails: Option<Vec<jsg_cpp::Ref<Fetcher>>>,
    pub streamingTails: Option<Vec<jsg_cpp::Ref<Fetcher>>>,
}
