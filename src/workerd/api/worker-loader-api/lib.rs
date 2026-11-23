// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The Worker Loader binding in Rust: `WorkerLoader`, which loads dynamic Workers, and
//! `WorkerStub`, a loaded Worker's entrypoints. It does what `api/worker-loader.c++` does, and
//! C++ creates it for a Worker Loader binding when the `RUST_WORKER_LOADER` autogate is on.
//!
//! The checks are plain Rust in [`validate`]; `api/worker-loader-interop` builds the C++
//! `DynamicWorkerSource`, compiles its compatibility flags and calls
//! `IoChannelFactory::loadIsolate()`.

mod code;
mod dicts;
mod ffi;
pub mod validate;

use channel_interop::DurableObjectClass;
use channel_interop::Fetcher;
pub use dicts::EntrypointOptions;
pub use dicts::Module;
pub use dicts::ModuleContent;
pub use dicts::ResourceLimits;
pub use dicts::WorkerCode;
use frankenvalue::Frankenvalue;
use io_context::IoContext;
use jsg::FromJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use kj_rs::KjOwn;
use worker_loader_interop::Limits;
use worker_loader_interop::SourceBuilder;
use worker_loader_interop::WorkerStubHandle;

use crate::code::Bytes;
use crate::code::Wasm;
use crate::validate::ErrorKind;
use crate::validate::ModuleKind;
use crate::validate::ValidationError;

/// The compatibility dates a `WorkerLoader` accepts, as C++ `CompatibilityDateValidation`,
/// whose values these are.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum DateValidation {
    CodeVersion = 0,
    CurrentDateForCloudflare = 1,
    FutureForTest = 2,
}

fn limits_to_ffi(limits: Option<&ResourceLimits>) -> Limits {
    Limits {
        present: limits.is_some(),
        has_cpu_ms: limits.is_some_and(|l| l.cpuMs.is_some()),
        cpu_ms: limits.and_then(|l| l.cpuMs).unwrap_or_default(),
        has_sub_requests: limits.is_some_and(|l| l.subRequests.is_some()),
        sub_requests: limits.and_then(|l| l.subRequests).unwrap_or_default(),
    }
}

/// What a `WorkerLoader` is: its loader channel and the compatibility dates it accepts. It is
/// also what a startup snapshot keeps of it.
#[derive(Clone, Copy, Debug)]
pub struct LoaderState {
    channel: u32,
    date_validation: DateValidation,
}

// Plain data.
jsg::jsg_traced_unchecked!(LoaderState);

/// `env.LOADER`: loads dynamic Workers on a loader channel.
#[jsg_resource(name = "WorkerLoader", ts_root)]
pub struct WorkerLoader {
    state: LoaderState,
}

impl WorkerLoader {
    #[must_use]
    pub fn new(channel: u32, date_validation: DateValidation) -> Self {
        Self {
            state: LoaderState {
                channel,
                date_validation,
            },
        }
    }
}

#[jsg_resource]
impl WorkerLoader {
    /// `get(name, getCode)`: the Worker named `name`, loaded with the code `getCode()` returns
    /// or resolves to when it needs loading, which may be more than once. An unnamed Worker is
    /// never shared.
    #[jsg_method]
    pub fn get<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        name: jsg::NullOrUndefined<String>,
        get_code: v8::Local<'s, v8::Function>,
    ) -> jsg::Result<jsg::Rc<WorkerStub>> {
        let io_context = IoContext::current(lock)?;
        let name = match name {
            jsg::NullOrUndefined::Some(name) => Some(name),
            _ => None,
        };
        let callback = code_callback(lock, &io_context, self.state, get_code)?;
        let handle = worker_loader_interop::get(
            lock,
            &io_context,
            self.state.channel,
            name.as_deref(),
            callback,
        )?;
        Ok(jsg::Rc::new(lock, WorkerStub::new(handle)))
    }

    /// `load(code)`: a new Worker of `code`, as `get(null, () => code)`.
    #[jsg_method]
    pub fn load<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        code: WorkerCode,
    ) -> jsg::Result<jsg::Rc<WorkerStub>> {
        let io_context = IoContext::current(lock)?;
        let source = to_source(lock, &io_context, self.state, code)?;
        let handle = worker_loader_interop::load(lock, &io_context, self.state.channel, source)?;
        Ok(jsg::Rc::new(lock, WorkerStub::new(handle)))
    }
}

impl jsg::SnapshotTransfer for WorkerLoader {
    type Snapshot = LoaderState;

    fn snapshot(&self) -> LoaderState {
        self.state
    }

    fn restore(state: &LoaderState) -> Self {
        Self { state: *state }
    }
}

/// The `WorkerStubChannel` a `WorkerStub` holds.
struct StubChannel(KjOwn<WorkerStubHandle>);

// It holds a C++ channel, which refers to nothing in the V8 heap.
jsg::jsg_traced_unchecked!(StubChannel);

/// A loaded Worker: not one entrypoint but the whole Worker, whose entrypoints the caller picks,
/// with any props.
#[jsg_resource(
    name = "WorkerStub",
    ts_override = "{ getEntrypoint<T extends Rpc.WorkerEntrypointBranded | undefined>(name?: \
                   string, options?: WorkerStubEntrypointOptions): Fetcher<T>; \
                   getDurableObjectClass<T extends Rpc.DurableObjectBranded | undefined>(name?: \
                   string, options?: WorkerStubEntrypointOptions): DurableObjectClass<T>; }"
)]
pub struct WorkerStub {
    channel: StubChannel,
}

impl WorkerStub {
    fn new(handle: KjOwn<WorkerStubHandle>) -> Self {
        Self {
            channel: StubChannel(handle),
        }
    }
}

#[jsg_resource]
impl WorkerStub {
    /// `getEntrypoint(name?, options?)`: a `Fetcher` for the entrypoint `name`, the default one
    /// if it is absent, `null` or `"default"`.
    #[jsg_method]
    pub fn get_entrypoint<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        name: Option<jsg::Nullable<String>>,
        options: Option<EntrypointOptions>,
    ) -> jsg::Result<jsg_cpp::Ref<Fetcher>> {
        let (props, limits) = entrypoint_options(lock, options)?;
        let name = validate::entrypoint_name(nullable_name(name));
        let io_context = IoContext::current(lock)?;
        self.channel
            .0
            .entrypoint(lock, &io_context, name.as_deref(), props, &limits)
    }

    /// `getDurableObjectClass(name?, options?)`: a `DurableObjectClass` for the class `name`, as
    /// for `getEntrypoint()`.
    #[jsg_method]
    pub fn get_durable_object_class<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        name: Option<jsg::Nullable<String>>,
        options: Option<EntrypointOptions>,
    ) -> jsg::Result<jsg_cpp::Ref<DurableObjectClass>> {
        let (props, limits) = entrypoint_options(lock, options)?;
        let name = validate::entrypoint_name(nullable_name(name));
        let io_context = IoContext::current(lock)?;
        self.channel
            .0
            .actor_class(lock, &io_context, name.as_deref(), props, &limits)
    }
}

fn nullable_name(name: Option<jsg::Nullable<String>>) -> Option<String> {
    match name {
        Some(jsg::Nullable::Some(name)) => Some(name),
        _ => None,
    }
}

/// The props, as a `Frankenvalue`, and the limits of entrypoint options.
fn entrypoint_options(
    lock: &mut jsg::Lock<'_>,
    options: Option<EntrypointOptions>,
) -> jsg::Result<(KjOwn<Frankenvalue>, Limits)> {
    let Some(options) = options else {
        return Ok((Frankenvalue::empty(), limits_to_ffi(None)));
    };
    let props = match &options.props {
        Some(props) => {
            let props = props.as_local(lock);
            Frankenvalue::from_js(lock, props.into())?
        }
        None => Frankenvalue::empty(),
    };
    Ok((props, limits_to_ffi(options.limits.as_ref())))
}

/// The callback that `get()` hands to C++, which calls `get_code` in `io_context` each time the
/// Worker needs loading, and converts what it resolves to in the same `IoContext`.
///
/// The callback holds the `IoContext` weakly, as C++ does: the `WorkerStub` and the stubs it
/// vends hold it until they are collected, and a strong one would keep the request open, which
/// blocks Durable Object hibernation.
fn code_callback<'s>(
    lock: &mut jsg::Lock<'s>,
    io_context: &IoContext<'s>,
    state: LoaderState,
    get_code: v8::Local<'s, v8::Function>,
) -> jsg::Result<worker_loader_interop::CodeCallback> {
    let get_code = get_code.to_global(lock);
    let callback = io_context.make_reentry_callback_weak(move |lock, context| {
        let code = get_code.as_local(lock).call(lock, None, &[])?;
        let code = v8::Promise::resolved(lock, code)?;
        // `getCode()` may resolve its promise from another request: the conversion runs only in
        // this `IoContext`, as C++ `awaitJs(js, promise, func)` ensures.
        context.await_js_local(lock, code, move |lock, context, value| {
            let code = WorkerCode::from_js(lock, value)?;
            to_source(lock, context, state, code)
        })
    })?;
    Ok(worker_loader_interop::CodeCallback(callback))
}

fn js_error(err: ValidationError) -> jsg::Error {
    match err.kind {
        ErrorKind::Error => jsg::Error::new_error(err.message),
        ErrorKind::TypeError => jsg::Error::new_type_error(err.message),
    }
}

/// The source of the Worker that `code` describes, checked as C++ `toDynamicWorkerSource()`
/// checks it, in the same order.
fn to_source<'s>(
    lock: &mut jsg::Lock<'s>,
    io_context: &IoContext<'s>,
    state: LoaderState,
    code: WorkerCode,
) -> jsg::Result<KjOwn<SourceBuilder>> {
    let WorkerCode {
        compatibilityDate: compat_date,
        compatibilityFlags: compat_flags,
        allowExperimental: allow_experimental,
        limits,
        mainModule: main_module,
        modules,
        env,
        globalOutbound: global_outbound,
        tails,
        streamingTails: streaming_tails,
    } = code;
    let mut builder = SourceBuilder::new(&main_module);

    validate::require_modules(modules.len()).map_err(js_error)?;
    let mut kinds: Vec<(String, ModuleKind, usize)> = Vec::with_capacity(modules.len());
    for (name, content) in modules {
        let (kind, size) = add_module(lock, builder.pin_mut(), &name, &main_module, content)?;
        kinds.push((name, kind, size));
    }
    validate::check_modules(
        &main_module,
        kinds
            .iter()
            .map(|(name, kind, size)| (name.as_str(), *kind, *size)),
    )
    .map_err(js_error)?;

    let flags = validate::python_flags(&main_module, compat_flags);
    let allow_experimental = allow_experimental.unwrap_or(false);
    let caller_experimental = lock.feature_flags().get_workerd_experimental();
    validate::check_allow_experimental(allow_experimental, caller_experimental)
        .map_err(js_error)?;
    builder.pin_mut().compile_compat_flags(
        lock,
        &compat_date,
        flags.as_deref().unwrap_or_default(),
        allow_experimental,
        state.date_validation as u8,
    )?;

    if let Some(env) = env {
        let env = env.as_local(lock);
        let env = Frankenvalue::from_js(lock, env.into())?;
        validate::check_env_size(env.estimate_size()).map_err(js_error)?;
        builder.pin_mut().set_env(env);
    }

    match global_outbound {
        jsg::NullOrUndefined::Undefined => builder
            .pin_mut()
            .inherit_global_outbound(lock, io_context)?,
        jsg::NullOrUndefined::Null => {}
        jsg::NullOrUndefined::Some(fetcher) => {
            let channel = fetcher
                .get(lock)
                .subrequest_channel(lock, io_context)?
                .require_allows_transfer(lock)?;
            builder.pin_mut().set_global_outbound(channel);
        }
    }

    for tail in tails.unwrap_or_default() {
        let channel = tail
            .get(lock)
            .subrequest_channel(lock, io_context)?
            .require_allows_transfer(lock)?;
        builder.pin_mut().add_tail(channel);
    }

    if let Some(streaming_tails) = streaming_tails {
        validate::check_streaming_tails(allow_experimental).map_err(js_error)?;
        for tail in streaming_tails {
            let channel = tail
                .get(lock)
                .subrequest_channel(lock, io_context)?
                .require_allows_transfer(lock)?;
            builder.pin_mut().add_streaming_tail(channel);
        }
    }

    builder
        .pin_mut()
        .set_limits(&limits_to_ffi(limits.as_ref()));
    Ok(builder)
}

/// Adds the module `name` to `builder`, and returns its kind and the size of its body.
fn add_module(
    lock: &mut jsg::Lock<'_>,
    builder: std::pin::Pin<&mut SourceBuilder>,
    name: &str,
    main_module: &str,
    content: ModuleContent,
) -> jsg::Result<(ModuleKind, usize)> {
    match content {
        ModuleContent::Text(text) => {
            let kind = validate::classify_string_module(name, main_module).map_err(js_error)?;
            let size = text.len();
            builder.add_text(to_ffi_kind(kind), name, text)?;
            Ok((kind, size))
        }
        ModuleContent::Wasm(module) => add_wasm(lock, builder, name, Wasm::Module(module)),
        ModuleContent::Module(module) => {
            let count = [
                module.js.is_some(),
                module.cjs.is_some(),
                module.text.is_some(),
                module.data.is_some(),
                module.json.is_some(),
                module.py.is_some(),
                module.wasm.is_some(),
            ]
            .into_iter()
            .filter(|&set| set)
            .count();
            validate::require_exactly_one_field(name, count).map_err(js_error)?;
            let Module {
                js,
                cjs,
                text,
                data,
                json,
                py,
                wasm,
            } = module;
            let (kind, body) = if let Some(js) = js {
                (ModuleKind::Es, js)
            } else if let Some(cjs) = cjs {
                (ModuleKind::CommonJs, cjs)
            } else if let Some(text) = text {
                (ModuleKind::Text, text)
            } else if let Some(py) = py {
                (ModuleKind::Python, py)
            } else if let Some(json) = json {
                let json = json.as_local(lock);
                (ModuleKind::Json, jsg::json::stringify(lock, json)?)
            } else if let Some(Bytes(bytes)) = data {
                let size = bytes.len();
                builder.add_bytes(to_ffi_kind(ModuleKind::Data), name, bytes)?;
                return Ok((ModuleKind::Data, size));
            } else if let Some(wasm) = wasm {
                return add_wasm(lock, builder, name, wasm);
            } else {
                unreachable!("exactly one field is set")
            };
            let size = body.len();
            builder.add_text(to_ffi_kind(kind), name, body)?;
            Ok((kind, size))
        }
    }
}

fn add_wasm(
    lock: &mut jsg::Lock<'_>,
    builder: std::pin::Pin<&mut SourceBuilder>,
    name: &str,
    wasm: Wasm,
) -> jsg::Result<(ModuleKind, usize)> {
    match wasm {
        Wasm::Bytes(bytes) => {
            let size = bytes.len();
            builder.add_bytes(to_ffi_kind(ModuleKind::Wasm), name, bytes)?;
            Ok((ModuleKind::Wasm, size))
        }
        // No copy: the wire bytes are owned by the compiled module, not the V8 heap.
        Wasm::Module(module) => {
            let module = module.as_local(lock).compiled_module();
            let size = module.wire_bytes().len();
            builder.add_compiled_wasm(lock, name, module)?;
            Ok((ModuleKind::Wasm, size))
        }
    }
}

fn to_ffi_kind(kind: ModuleKind) -> worker_loader_interop::ModuleKind {
    use worker_loader_interop::ModuleKind as Ffi;
    match kind {
        ModuleKind::Es => Ffi::Es,
        ModuleKind::CommonJs => Ffi::CommonJs,
        ModuleKind::Text => Ffi::Text,
        ModuleKind::Data => Ffi::Data,
        ModuleKind::Json => Ffi::Json,
        ModuleKind::Python => Ffi::Python,
        ModuleKind::Wasm => Ffi::Wasm,
    }
}
