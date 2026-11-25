// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The loader of the per-isolate bootstrap: the scripts in `src/per_isolate/` that run in every
//! context before user code. `api/per-isolate-bootstrap.c++` calls it under the
//! `RUST_PER_ISOLATE_BOOTSTRAP` autogate, and otherwise runs its own C++ loader; see
//! `api/per-isolate-bootstrap.h` for what the bootstrap does.
//!
//! Each script is compiled as a function whose parameters are the pseudo-globals
//! ([`pseudo_globals()`]) and run when something first `require()`s it. A context's
//! [`State`] holds the `require()` cache and the pseudo-globals' values. C++ keeps it in the
//! context's `BOOTSTRAP_STATE` slot (`ffi.c++`), where the deserializer finds the bootstrap's
//! classes, and frees it when the context is disposed.

use std::cell::RefCell;
use std::sync::OnceLock;

use foldhash::HashMap;
use foldhash::HashSet;
use jsg::Error;
use jsg::Lock;
use jsg::ToJS;
use jsg::v8;
use jsg::v8::Function;
use jsg::v8::Global;
use jsg::v8::Local;
use jsg::v8::Object;
use jsg::v8::Value;

mod ffi;
mod utils;

pub use ffi::bridge;

/// The pseudo-globals that scripts reach as variables, in the order of the parameters of the
/// function each script is compiled as: C++ `jsg::BOOTSTRAP_FUNCTION_PARAMS`
/// (`jsg/modules.capnp`), which `gen-compile-cache` compiles the scripts with too.
fn pseudo_globals() -> &'static [&'static str] {
    static NAMES: OnceLock<Vec<String>> = OnceLock::new();
    static PARAMS: OnceLock<Vec<&'static str>> = OnceLock::new();
    PARAMS.get_or_init(|| {
        NAMES
            .get_or_init(bridge::bootstrap_function_params)
            .iter()
            .map(String::as_str)
            .collect()
    })
}

// Property names of the object a zygote pins into its startup snapshot: the bootstrap's
// JavaScript state.
const STASH_REQUIRE: &str = "require";
// Present only when the zygote ran with an embedder bootstrap.
const STASH_EMBEDDER_REQUIRE: &str = "embedderRequire";
const STASH_COMPAT_FLAGS: &str = "compatFlags";
const STASH_AUTOGATES: &str = "autogates";
const STASH_UTILS: &str = "utils";
const STASH_PRIMORDIALS: &str = "primordials";
// A null-prototype object mapping each specifier in the `require()` cache to its exports.
const STASH_CACHE: &str = "cache";

/// Which scripts a `require()` function resolves: workerd's only, or the embedder's as well.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Scope {
    Workerd,
    Embedder,
}

/// A `require()` specifier with a leading `./` removed.
fn normalize(specifier: &str) -> &str {
    specifier.strip_prefix("./").unwrap_or(specifier)
}

/// The bootstrap state of one context.
///
/// Reached as `&State` by every `require()`, which can run a script that calls `require()`
/// again, so what changes is behind `RefCell`s, borrowed only between calls into JavaScript.
/// The handles are strong `Global`s, as C++ `jsg::JsRef`s are: nothing traces the state, which
/// C++ frees when the context is disposed.
pub struct State {
    host: cxx::UniquePtr<bridge::ScriptHost>,
    compat_flags: Global<Object>,
    autogates: Global<Object>,
    utils: Global<Object>,
    /// The `require()` given to workerd's scripts, which resolves only workerd's.
    require: Global<Function>,
    /// The `require()` given to the embedder's scripts, which also resolves the embedder's. Set
    /// when there is an embedder.
    embedder_require: Option<Global<Function>>,
    /// Loaded before any other script, and given to every other script.
    primordials: RefCell<Option<Global<Value>>>,
    /// The exports of each loaded module, by normalized specifier.
    cache: RefCell<HashMap<String, Global<Value>>>,
    /// Functions found by [`State::function`], by module specifier, then name.
    functions: RefCell<HashMap<String, HashMap<String, Global<Function>>>>,
    /// The modules whose scripts are running.
    loading: RefCell<HashSet<String>>,
}

/// Marks a module as loading while it lives.
struct Loading<'a> {
    state: &'a State,
    specifier: &'a str,
}

impl Drop for Loading<'_> {
    fn drop(&mut self) {
        self.state.loading.borrow_mut().remove(self.specifier);
    }
}

fn internal_error(message: impl Into<String>) -> Error {
    Error::new_internal_error(message)
}

/// The bootstrap state of the current context, valid for the caller's call; see
/// [`ffi::current_state()`].
fn state<'a>(lock: &mut Lock<'_>) -> jsg::Result<&'a State> {
    ffi::current_state(lock)
        .ok_or_else(|| internal_error("per-isolate bootstrap state not initialized"))
}

/// Hands `state` to C++ as the current context's, and returns it.
fn install<'a>(lock: &mut Lock<'_>, state: State) -> jsg::Result<&'a State> {
    ffi::install_state(lock, state).map_err(|e| internal_error(e.what().to_owned()))
}

/// An object with a `true` property for each of `names`.
fn flags_object<'s, 'n>(
    lock: &mut Lock<'s>,
    names: impl IntoIterator<Item = &'n str>,
) -> jsg::Result<Local<'s, Object>> {
    let object = lock.new_object();
    for name in names {
        let value = true.to_js(lock)?;
        object.set(lock, name, value)?;
    }
    Ok(object)
}

fn new_require(lock: &mut Lock<'_>, scope: Scope) -> jsg::Result<Global<Function>> {
    let callback = match scope {
        Scope::Workerd => jsg::function_callback(|lock, args| {
            require_callback(lock, args, Scope::Workerd);
        }),
        Scope::Embedder => jsg::function_callback(|lock, args| {
            require_callback(lock, args, Scope::Embedder);
        }),
    };
    let function = Function::from_callback(lock, callback)?;
    Ok(Global::new(lock, function))
}

/// The `require()` given to the bootstrap's scripts. It reads the state from the current
/// context, so the function needs no data.
fn require_callback<'s>(
    lock: &mut Lock<'s>,
    args: &mut v8::FunctionCallbackInfo<'s>,
    scope: Scope,
) {
    let specifier = args.get(0);
    let result = state(lock).and_then(|state| {
        let specifier = Local::<v8::String>::try_from(specifier)
            .map_err(|_| internal_error("require() expects a string argument"))?
            .to_string(lock);
        state.require(lock, normalize(&specifier), scope)
    });
    match result {
        Ok(exports) => args.set_return_value(exports),
        Err(error) => lock.throw_exception(&error),
    }
}

/// Bootstraps the current context: from the scripts, or, for a context deserialized from a
/// startup snapshot, from the state the zygote stashed. `compat_flags` is the worker's
/// `CompatibilityFlags`, canonical.
fn run(
    lock: &mut Lock<'_>,
    host: cxx::UniquePtr<bridge::ScriptHost>,
    compat_flags: &[u8],
) -> jsg::Result<()> {
    let flags = jsg::FeatureFlags::from_bytes(compat_flags);
    let flags = flags.reader();
    if lock.is_starting_from_snapshot() {
        // The scripts' effects came with the context: the globals main installed, every
        // module's exports, the closures holding require(), primordials and utils. The zygote
        // ran under the same autogate as this isolate, so a missing stash would mean two class
        // sets.
        perfetto::trace_event!("PerIsolateBootstrap::restore");
        run_from_snapshot(lock, host)?;
    } else {
        let compat_flags = compat_date::decompile(flags, compat_date::Decompile::All)
            .map_err(|e| internal_error(format!("cannot read the compatibility flags: {e}")))?;
        run_from_scripts(lock, host, &compat_flags)?;
    }

    if !flags.get_js_weak_ref() {
        let global = lock.global_object();
        for name in ["WeakRef", "FinalizationRegistry"] {
            let name = v8::String::new_internalized_from_str(lock, name)
                .ok_or_else(|| internal_error("cannot create a string"))?;
            global.delete(lock, name.into())?;
        }
    }
    Ok(())
}

fn run_from_scripts(
    lock: &mut Lock<'_>,
    host: cxx::UniquePtr<bridge::ScriptHost>,
    compat_flags: &[&str],
) -> jsg::Result<()> {
    let utils = ffi::api_natives(lock, &host).ok_or_else(Error::pending)?;
    let utils = Local::<Object>::try_from(utils)
        .map_err(|_| internal_error("the bootstrap's utils is not an object"))?;
    utils::add_natives(lock, utils)?;
    let embedder_entry_point = host.embedder_entry_point().to_owned();
    let compat_flags = flags_object(lock, compat_flags.iter().copied())?;
    let autogates = flags_object(lock, autogate::enabled())?;
    let require = new_require(lock, Scope::Workerd)?;
    let embedder_require = if embedder_entry_point.is_empty() {
        None
    } else {
        Some(new_require(lock, Scope::Embedder)?)
    };
    let state = State {
        host,
        compat_flags: Global::new(lock, compat_flags),
        autogates: Global::new(lock, autogates),
        utils: Global::new(lock, utils),
        require,
        embedder_require,
        primordials: RefCell::new(None),
        cache: RefCell::new(HashMap::default()),
        functions: RefCell::new(HashMap::default()),
        loading: RefCell::new(HashSet::default()),
    };
    let state = install(lock, state)?;

    // Primordials first, so that they capture the builtins before anything can change them.
    {
        perfetto::trace_event!("PerIsolateBootstrap::primordials");
        let primordials = state.require(lock, "primordials", Scope::Workerd)?;
        *state.primordials.borrow_mut() = Some(Global::new(lock, primordials));
    }

    {
        perfetto::trace_event!("PerIsolateBootstrap::main");
        state.require(lock, "main", Scope::Workerd)?;
    }

    // The embedder's entry point runs once workerd's globals are installed, and before the
    // WeakRef globals are removed, so that its scripts see the same environment as workerd's.
    if !embedder_entry_point.is_empty() {
        perfetto::trace_event!("PerIsolateBootstrap::embedderMain");
        state.require(lock, &embedder_entry_point, Scope::Embedder)?;
    }
    Ok(())
}

fn take_object<'s>(
    lock: &mut Lock<'s>,
    stash: Local<'s, Object>,
    name: &str,
) -> jsg::Result<Local<'s, Object>> {
    let value = stash.get(lock, name)?;
    Local::<Object>::try_from(value).map_err(|_| {
        internal_error(format!(
            "snapshot bootstrap state is missing an object: {name}"
        ))
    })
}

fn take_function<'s>(
    lock: &mut Lock<'s>,
    stash: Local<'s, Object>,
    name: &str,
) -> jsg::Result<Global<Function>> {
    let value = stash.get(lock, name)?;
    let function = Local::<Function>::try_from(value).map_err(|_| {
        internal_error(format!(
            "snapshot bootstrap state is missing a function: {name}"
        ))
    })?;
    Ok(Global::new(lock, function))
}

/// Rebuilds the state from what a zygote stashed, which V8 hands out once. Nothing is loading.
/// The embedder is the restoring isolate's, which must be the zygote's.
fn run_from_snapshot(
    lock: &mut Lock<'_>,
    host: cxx::UniquePtr<bridge::ScriptHost>,
) -> jsg::Result<()> {
    let stash = ffi::snapshot_state_index(lock)
        .and_then(|index| lock.extract_once_from_snapshot(index))
        .ok_or_else(|| {
            internal_error(
                "context deserialized from a startup snapshot carries no per-isolate bootstrap \
                 state",
            )
        })?;
    let stash = Local::<Object>::try_from(stash)
        .map_err(|_| internal_error("snapshot bootstrap state is not an object"))?;
    let compat_flags = take_object(lock, stash, STASH_COMPAT_FLAGS)?;
    let autogates = take_object(lock, stash, STASH_AUTOGATES)?;
    let utils = take_object(lock, stash, STASH_UTILS)?;
    let require = take_function(lock, stash, STASH_REQUIRE)?;
    let embedder_require = if host.embedder_entry_point().is_empty() {
        None
    } else {
        Some(take_function(lock, stash, STASH_EMBEDDER_REQUIRE)?)
    };
    let primordials = stash.get(lock, STASH_PRIMORDIALS)?;

    let cache_object = take_object(lock, stash, STASH_CACHE)?;
    let mut cache = HashMap::default();
    for name in cache_object.keys(lock)? {
        let specifier = name.to_string(lock);
        let exports = cache_object.get(lock, &specifier)?;
        cache.insert(specifier, Global::new(lock, exports));
    }

    let state = State {
        host,
        compat_flags: Global::new(lock, compat_flags),
        autogates: Global::new(lock, autogates),
        utils: Global::new(lock, utils),
        require,
        embedder_require,
        primordials: RefCell::new(Some(Global::new(lock, primordials))),
        cache: RefCell::new(cache),
        functions: RefCell::new(HashMap::default()),
        loading: RefCell::new(HashSet::default()),
    };
    install(lock, state)?;
    Ok(())
}

/// Pins the current context's bootstrap state into the zygote's snapshot, as one object, and
/// records where for the isolates restored from it.
fn stash_for_snapshot(lock: &mut Lock<'_>) -> jsg::Result<()> {
    let stash = stash(lock)?;
    let index = lock.pin_into_snapshot(stash.into())?;
    ffi::set_snapshot_state_index(lock, index);
    Ok(())
}

/// The current context's bootstrap state, as one null-prototype object.
fn stash<'s>(lock: &mut Lock<'s>) -> jsg::Result<Local<'s, Object>> {
    let state = state(lock)?;
    if !state.loading.borrow().is_empty() {
        return Err(internal_error(
            "require() still in progress at snapshot time",
        ));
    }

    // Read now rather than when the bootstrap finished: a lazy require() during the zygote's
    // top-level evaluation grows the cache after `main` returned.
    let entries: Vec<(String, Local<'s, Value>)> = state
        .cache
        .borrow()
        .iter()
        .map(|(specifier, exports)| (specifier.clone(), exports.as_local(lock)))
        .collect();
    let cache = lock.new_object_with_null_prototype();
    for (specifier, exports) in entries {
        cache.set(lock, &specifier, exports)?;
    }

    let primordials = state
        .primordials
        .borrow()
        .as_ref()
        .map(|primordials| primordials.as_local(lock))
        .ok_or_else(|| internal_error("bootstrap did not load primordials"))?;

    let stash = lock.new_object_with_null_prototype();
    let require = state.require.as_local(lock);
    stash.set(lock, STASH_REQUIRE, require.into())?;
    if let Some(embedder_require) = &state.embedder_require {
        let embedder_require = embedder_require.as_local(lock);
        stash.set(lock, STASH_EMBEDDER_REQUIRE, embedder_require.into())?;
    }
    let compat_flags = state.compat_flags.as_local(lock);
    stash.set(lock, STASH_COMPAT_FLAGS, compat_flags.into())?;
    let autogates = state.autogates.as_local(lock);
    stash.set(lock, STASH_AUTOGATES, autogates.into())?;
    let utils = state.utils.as_local(lock);
    stash.set(lock, STASH_UTILS, utils.into())?;
    stash.set(lock, STASH_PRIMORDIALS, primordials)?;
    stash.set(lock, STASH_CACHE, cache.into())?;
    Ok(stash)
}

impl State {
    /// The exports of the module `specifier` if it has been loaded. Runs no JavaScript.
    fn loaded<'s>(&self, lock: &mut Lock<'s>, specifier: &str) -> Option<Local<'s, Value>> {
        self.cache
            .borrow()
            .get(specifier)
            .map(|exports| exports.as_local(lock))
    }

    /// `require(specifier)` from a script of `scope`. `specifier` is normalized.
    fn require<'s>(
        &self,
        lock: &mut Lock<'s>,
        specifier: &str,
        scope: Scope,
    ) -> jsg::Result<Local<'s, Value>> {
        // workerd's scripts cannot see the embedder's, even once they are cached.
        let is_embedder_script = self.host.in_embedder_namespace(specifier);
        if is_embedder_script && scope == Scope::Workerd {
            return Err(internal_error(format!(
                "per-isolate bootstrap: workerd's scripts cannot require the embedder's: \
                 {specifier}"
            )));
        }

        if let Some(exports) = self.loaded(lock, specifier) {
            return Ok(exports);
        }

        if self.loading.borrow().contains(specifier) {
            return Err(internal_error(format!(
                "circular dependency in per-isolate bootstrap: {specifier}"
            )));
        }

        // The script, and the require() it gets: the embedder's scripts resolve everything,
        // workerd's only workerd's, and primordials nothing.
        let (script, require) = if is_embedder_script {
            (
                ffi::find_embedder_script(&self.host, specifier),
                self.embedder_require.as_ref(),
            )
        } else {
            (
                ffi::find_workerd_script(&self.host, specifier),
                (specifier != "primordials").then_some(&self.require),
            )
        };
        // The script lives as long as the host, which this state owns.
        let script = script.ok_or_else(|| {
            internal_error(format!(
                "per-isolate bootstrap: unknown script: {specifier}"
            ))
        })?;

        self.loading.borrow_mut().insert(specifier.to_owned());
        let _loading = Loading {
            state: self,
            specifier,
        };

        let module = lock.new_object();
        let exports = lock.new_object();
        module.set(lock, "exports", exports.into())?;

        let params = pseudo_globals();
        let mut args = Vec::with_capacity(params.len());
        for &name in params {
            let value: Local<'s, Value> = match name {
                "require" => match require {
                    Some(require) => require.as_local(lock).into(),
                    None => lock.undefined(),
                },
                "compatFlags" => self.compat_flags.as_local(lock).into(),
                "autogates" => self.autogates.as_local(lock).into(),
                "module" => module.into(),
                "exports" => exports.into(),
                "primordials" => match self.primordials.borrow().as_ref() {
                    Some(primordials) => primordials.as_local(lock),
                    None => lock.undefined(),
                },
                "utils" => self.utils.as_local(lock).into(),
                _ => {
                    return Err(internal_error(format!(
                        "unknown per-isolate bootstrap pseudo-global: {name}"
                    )));
                }
            };
            args.push(value);
        }

        let source = self.source(lock, script)?;
        let origin = format!("workerd:per-isolate/{specifier}");
        let function = v8::ScriptCompiler::compile_function(
            lock,
            source,
            &origin,
            params,
            bridge::script_compile_cache(script),
        )?;

        function.call(lock, None, &args)?;

        // The script may have replaced module.exports.
        let exports = module.get(lock, "exports")?;
        self.cache
            .borrow_mut()
            .insert(specifier.to_owned(), Global::new(lock, exports));
        Ok(exports)
    }

    /// The source of `script`: from the host's `jsg::ModuleSource` if it has one, and otherwise
    /// the compiled-in bytes, which V8 reads as a one-byte string, without transcoding.
    fn source<'s>(
        &self,
        lock: &mut Lock<'s>,
        script: &bridge::Script,
    ) -> jsg::Result<Local<'s, v8::String>> {
        match ffi::module_source(lock, &self.host, script) {
            Some(source) => Ok(source),
            None => v8::String::new_external_one_byte(lock, bridge::script_source(script))
                .ok_or_else(|| internal_error("cannot create a bootstrap script's source")),
        }
    }

    /// The function property `name` of the exports of the loaded module `specifier`, or `None`.
    /// Remembers what it finds, so later lookups skip the property read.
    fn function<'s>(
        &self,
        lock: &mut Lock<'s>,
        specifier: &str,
        name: &str,
    ) -> jsg::Result<Option<Local<'s, Function>>> {
        if let Some(function) = self
            .functions
            .borrow()
            .get(specifier)
            .and_then(|functions| functions.get(name))
        {
            return Ok(Some(function.as_local(lock)));
        }

        let Some(exports) = self.loaded(lock, specifier) else {
            return Ok(None);
        };
        let Ok(exports) = Local::<Object>::try_from(exports) else {
            return Ok(None);
        };
        let value = exports.get(lock, name)?;
        let Ok(function) = Local::<Function>::try_from(value) else {
            return Ok(None);
        };
        self.functions
            .borrow_mut()
            .entry(specifier.to_owned())
            .or_default()
            .insert(name.to_owned(), Global::new(lock, function));
        Ok(Some(function))
    }

    /// Makes `exports` the exports of the module `specifier`.
    fn set_exports<'s>(&self, lock: &mut Lock<'s>, specifier: &str, exports: Local<'s, Value>) {
        self.functions.borrow_mut().remove(specifier);
        self.cache
            .borrow_mut()
            .insert(specifier.to_owned(), Global::new(lock, exports));
    }
}
