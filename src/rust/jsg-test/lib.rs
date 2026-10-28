// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::pin::Pin;

use jsg::FromJS;
use jsg::feature_flags::compatibility_flags;
use jsg::v8;
use kj_rs::KjOwn;

#[cfg(test)]
mod tests;

#[cxx::bridge(namespace = "workerd::rust::jsg_test")]
mod ffi {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        type Isolate = jsg::v8::ffi::Isolate;
        type Local = jsg::v8::ffi::Local;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
    }

    #[derive(Debug, Clone, Copy)]
    struct EvalResult {
        success: bool,
        /// The completion value, or the exception if `success` is false; empty if evaluation
        /// failed without one.
        value: MaybeLocal,
    }

    enum GcType {
        /// Full (major) GC — collects both young and old generations, plus cppgc heap.
        Full = 0,
        /// Minor (scavenge) GC — collects only the young generation.
        Minor = 1,
    }

    /// Whether resource methods get V8 Fast API callbacks.
    enum FastApi {
        Disabled = 0,
        Enabled = 1,
    }

    unsafe extern "C++" {
        include!("workerd/rust/jsg-test/ffi.h");
        type TestHarness;
        type EvalContext;

        pub unsafe fn create_test_harness(
            flags: &[u8],
            fast_api: FastApi,
        ) -> Result<KjOwn<TestHarness>>;
        pub unsafe fn run_in_context(
            self: &TestHarness,
            data: usize, /* callback */
            callback: unsafe fn(usize /* callback */, *mut Isolate, Pin<&mut EvalContext>),
        );

        /// Drops the realm, then calls `callback` and runs a full GC, under the isolate lock.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub unsafe fn drop_realm_then_gc(
            self: &TestHarness,
            data: usize, /* callback */
            callback: unsafe fn(usize /* callback */),
        );

        pub unsafe fn eval(
            self: &EvalContext,
            code: &str,
            resource_name: KjMaybe<&str>,
        ) -> EvalResult;
        pub unsafe fn set_global(self: &EvalContext, name: &str, value: Local);
        /// Has `register_modules` add builtin modules, then calls the callback of the one named
        /// `specifier`, as importing it does. See `call_builtin_module` in `ffi.h`.
        pub unsafe fn call_builtin_module(
            self: &EvalContext,
            register_modules: fn(Pin<&mut ModuleRegistry>),
            specifier: &str,
        ) -> Result<EvalResult>;

        /// Triggers garbage collection for testing purposes.
        /// Note: For GC to actually collect objects, they must not be reachable from the
        /// current HandleScope.
        #[expect(clippy::allow_attributes)] // Only used in tests, but #[expect(dead_code)] fails during test builds
        #[allow(dead_code)]
        pub unsafe fn request_gc(isolate: *mut Isolate, gc_type: GcType);
        /// Requests a termination and runs JavaScript until V8 delivers it, so that the
        /// termination exception is pending on return. See `terminate_and_deliver` in `ffi.h`.
        pub unsafe fn terminate_and_deliver(isolate: *mut Isolate);

        /// Runs the isolate's pending microtasks.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub unsafe fn run_microtasks(isolate: *mut Isolate);

        /// Calls `callback` with the isolate unlocked (`v8::Unlocker`), then locks it again.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub unsafe fn run_unlocked(
            isolate: *mut Isolate,
            data: usize, /* callback */
            callback: unsafe fn(usize /* callback */),
        );

        /// `v8::HeapStatistics::used_global_handles_size()`: the bytes of the isolate's
        /// global and traced handle nodes in use.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub unsafe fn used_global_handles_size(isolate: *mut Isolate) -> usize;

        /// Creates a V8 object with the C++ `WORKERD_WRAPPABLE_TAG` in its internal fields.
        /// Used to test that Rust unwrap correctly rejects non-Rust wrappable objects.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub unsafe fn create_cpp_tagged_object(isolate: *mut Isolate) -> Local;

        /// Calls a function that C++ JSG wrapped and returns whether
        /// `jsg::v8::ffi::fci_get_data_wrappable` finds a Rust Wrappable in its data.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub unsafe fn cpp_wrapped_function_data_is_rust_wrappable(isolate: *mut Isolate) -> bool;

        /// Returns an empty `v8::MaybeLocal`.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub fn empty_maybe_local() -> MaybeLocal;

        /// Returns whether C++ sees `value` as an empty `v8::MaybeLocal`.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub fn cpp_sees_empty_maybe_local(value: MaybeLocal) -> bool;

        /// Serializes `value` with C++ JSG's `jsg::Serializer`. Fails if serialization throws.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub unsafe fn cpp_serialize(isolate: *mut Isolate, value: Local) -> Result<Vec<u8>>;

        /// Deserializes `data` with C++ JSG's `jsg::Deserializer`. Fails if deserialization
        /// throws.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub unsafe fn cpp_deserialize(isolate: *mut Isolate, data: &[u8]) -> Result<Local>;

        /// Returns whether `a` and `b` belong to the same `v8::IsolateGroup`.
        pub unsafe fn isolates_share_group(a: *mut Isolate, b: *mut Isolate) -> bool;

        /// Throws a `kj::Exception` with `description`, such as a tunneled
        /// `jsg-internal.DOMException(OperationError): ...`.
        #[expect(clippy::allow_attributes)]
        #[allow(dead_code)]
        pub fn throw_kj_exception(description: &str) -> Result<()>;

        /// Holds a value the way C++ code holds one that Rust created: calls `create`, keeps
        /// its result only in a `v8::Global` while a full GC runs, and calls `check` with it.
        /// Then resets the `Global` and runs a full GC again. See `hold_across_gc` in `ffi.h`.
        pub unsafe fn hold_across_gc(
            isolate: *mut Isolate,
            create: unsafe fn(*mut Isolate) -> MaybeLocal,
            check: unsafe fn(*mut Isolate, Local),
        );
    }
}

pub struct Harness(KjOwn<ffi::TestHarness>);

pub struct EvalContext<'a> {
    inner: &'a ffi::EvalContext,
}

#[derive(Debug)]
pub enum EvalError<'s> {
    UncoercibleResult {
        value: v8::Local<'s, v8::Value>,
        message: String,
    },
    Exception(v8::Local<'s, v8::Value>),
    EvalFailed,
}

impl<'s> EvalError<'s> {
    /// Extracts a `jsg::Error` from an `EvalError::Exception` variant.
    ///
    /// # Panics
    ///
    /// Panics if `self` is not `EvalError::Exception`, or if the value cannot be converted to a
    /// `jsg::Error`.
    pub fn unwrap_jsg_err(&self, lock: &mut jsg::Lock<'s>) -> jsg::Error {
        match self {
            EvalError::Exception(value) => jsg::Error::from_js(lock, *value)
                .expect("Failed to convert exception to jsg::Error"),
            _ => panic!("Unexpected error"),
        }
    }
}

impl EvalContext<'_> {
    /// Evaluates JavaScript code and converts the completion value to `T`.
    pub fn eval<'s, T>(&self, lock: &mut jsg::Lock<'s>, code: &str) -> Result<T, EvalError<'s>>
    where
        T: jsg::FromJS<'s, ResultType = T>,
    {
        let local = self.eval_raw(lock, code)?;
        T::from_js(lock, local).map_err(|e| EvalError::UncoercibleResult {
            value: local,
            message: e.to_string(),
        })
    }

    /// Evaluates JavaScript code and returns the raw `Local<Value>` without conversion.
    ///
    /// Useful for obtaining handles (e.g. functions) that aren't `FromJS` types.
    pub fn eval_raw<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        code: &str,
    ) -> Result<v8::Local<'s, v8::Value>, EvalError<'s>> {
        // SAFETY: self.inner is a valid EvalContext from C++; code is a valid str.
        let result = unsafe { self.inner.eval(code, None.into()) };
        Self::raw_result(lock, result)
    }

    /// Like [`EvalContext::eval_raw`], but compiles the code with a `ScriptOrigin` whose resource
    /// name is `resource_name`.
    pub fn eval_raw_named<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        code: &str,
        resource_name: &str,
    ) -> Result<v8::Local<'s, v8::Value>, EvalError<'s>> {
        // SAFETY: self.inner is a valid EvalContext from C++; code and resource_name are valid strs.
        let result = unsafe { self.inner.eval(code, Some(resource_name).into()) };
        Self::raw_result(lock, result)
    }

    fn raw_result<'s>(
        _lock: &jsg::Lock<'s>,
        result: ffi::EvalResult,
    ) -> Result<v8::Local<'s, v8::Value>, EvalError<'s>> {
        // SAFETY: the C++ harness evaluates in `lock`'s HandleScope, so the completion value or
        // exception lives in scope `'s`.
        let value = unsafe { v8::Local::from_ffi_maybe(result.value) };
        match (result.success, value) {
            (true, Some(value)) => Ok(value),
            (true, None) => unreachable!(),
            (false, Some(exception)) => Err(EvalError::Exception(exception)),
            (false, None) => Err(EvalError::EvalFailed),
        }
    }

    /// Has `register` add builtin modules with `jsg::modules::add_builtin`, then produces the
    /// exports of the one named `specifier`, as importing it does.
    ///
    /// # Panics
    ///
    /// If `register` adds no module named `specifier`.
    pub fn import_builtin<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        register: fn(Pin<&mut v8::ffi::ModuleRegistry>),
        specifier: &str,
    ) -> Result<v8::Local<'s, v8::Value>, EvalError<'s>> {
        // SAFETY: self.inner is a valid EvalContext, and the callback runs in `lock`'s scope.
        let result = unsafe { self.inner.call_builtin_module(register, specifier) }
            .expect("no such builtin module");
        Self::raw_result(lock, result)
    }

    /// Converts `value` to JavaScript and stores it as the global `name`.
    ///
    /// # Panics
    ///
    /// If the conversion fails.
    pub fn set_global<'s>(&self, lock: &mut jsg::Lock<'s>, name: &str, value: impl jsg::ToJS<'s>) {
        let value = value
            .to_js(lock)
            .expect("converting a global's value failed");
        // SAFETY: self.inner is a valid EvalContext and value is a valid Local handle.
        unsafe { self.inner.set_global(name, value.into_ffi()) }
    }
}

impl Harness {
    /// Creates a harness whose compatibility flags are all false.
    pub fn new() -> Self {
        Self::with_flags(|_| {})
    }

    /// Creates a harness whose compatibility flags are set by `set_flags`.
    ///
    /// All flags start false; no compatibility date or flag implications are applied.
    /// The flags are visible through `Lock::feature_flags()` and, as in a real worker, also
    /// configure the JSG isolate through the same flag handling used by workers.
    ///
    /// # Panics
    ///
    /// Panics if the harness cannot be created.
    pub fn with_flags(set_flags: impl FnOnce(compatibility_flags::Builder<'_>)) -> Self {
        Self::create(set_flags, ffi::FastApi::Disabled)
    }

    /// Like [`Harness::new`], with `JsgConfig::fastApiEnabled` set so that resource methods get
    /// V8 Fast API callbacks.
    pub fn with_fast_api() -> Self {
        Self::create(|_| {}, ffi::FastApi::Enabled)
    }

    fn create(
        set_flags: impl FnOnce(compatibility_flags::Builder<'_>),
        fast_api: ffi::FastApi,
    ) -> Self {
        let mut message = capnp::message::Builder::new_default();
        set_flags(message.init_root::<compatibility_flags::Builder<'_>>());
        let words = message
            .into_reader()
            .canonicalize()
            .expect("canonicalizing a freshly built message cannot fail");
        let bytes = capnp::Word::words_to_bytes(&words);
        // SAFETY: create_test_harness initializes the V8 platform and returns a valid harness.
        Self(
            unsafe { ffi::create_test_harness(bytes, fast_api) }
                .expect("failed to create test harness"),
        )
    }

    /// Runs a callback within a V8 context.
    ///
    /// The callback is passed through C++ via a data pointer since CXX doesn't support
    /// closures directly. The monomorphized trampoline function receives the pointer
    /// and reconstructs the closure.
    ///
    /// The callback receives a [`jsg::Lock`] for the harness's top-level `HandleScope`;
    /// handles it creates cannot outlive the callback.
    ///
    /// The callback returns `Result<(), jsg::Error>` to allow use of the `?` operator.
    /// If an error is returned, the test will panic. A panic in the callback is caught before
    /// it reaches C++ and resumed once `run_in_context` returns.
    pub fn run_in_context<F>(&self, callback: F)
    where
        F: for<'s> FnOnce(&mut jsg::Lock<'s>, &mut EvalContext<'_>) -> Result<(), jsg::Error>,
    {
        struct Call<F> {
            callback: Option<F>,
            panic: Option<Box<dyn std::any::Any + Send>>,
        }

        #[expect(clippy::needless_pass_by_value)]
        fn trampoline<F>(
            data: usize,
            isolate: *mut v8::ffi::Isolate,
            context: Pin<&mut ffi::EvalContext>,
        ) where
            F: for<'s> FnOnce(&mut jsg::Lock<'s>, &mut EvalContext<'_>) -> Result<(), jsg::Error>,
        {
            // SAFETY: data was cast from &raw mut Call<F> in run_in_context below.
            let call = unsafe { &mut *(data as *mut Call<F>) };
            let Some(callback) = call.callback.take() else {
                return;
            };
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let mut eval_context = EvalContext { inner: &context };
                // SAFETY: the C++ harness calls this with the isolate locked, a context
                // entered and a HandleScope open for the duration of the call.
                jsg::callback_scope!(unsafe let lock, isolate);
                if let Err(e) = callback(lock, &mut eval_context) {
                    panic!("Test failed: {}: {}", e.name, e.message);
                }
            }));
            if let Err(payload) = result {
                call.panic = Some(payload);
            }
        }

        let mut call = Call {
            callback: Some(callback),
            panic: None,
        };
        // SAFETY: `call` is valid for the duration of run_in_context.
        unsafe {
            self.0
                .run_in_context(&raw mut call as usize, trampoline::<F>);
        }
        if let Some(payload) = call.panic {
            std::panic::resume_unwind(payload);
        }
    }

    /// Drops the realm, as the isolate's teardown does before it disposes the isolate, then runs
    /// `f` and a full GC, all with the isolate locked but no context or scope. `f` therefore
    /// cannot use any handle. The harness, which cannot run anything without its realm, is
    /// dropped afterwards.
    ///
    /// # Panics
    ///
    /// Panics if `f` panics, once the GC has run.
    pub fn drop_realm_then_gc<F: FnOnce()>(self, f: F) {
        struct Call<F> {
            f: Option<F>,
            panic: Option<Box<dyn std::any::Any + Send>>,
        }

        fn trampoline<F: FnOnce()>(data: usize) {
            // SAFETY: data was cast from &raw mut Call<F> in drop_realm_then_gc below.
            let call = unsafe { &mut *(data as *mut Call<F>) };
            let Some(f) = call.f.take() else {
                return;
            };
            if let Err(payload) = std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
                call.panic = Some(payload);
            }
        }

        let mut call = Call {
            f: Some(f),
            panic: None,
        };
        // SAFETY: `call` is valid for the duration of drop_realm_then_gc.
        unsafe {
            self.0
                .drop_realm_then_gc(&raw mut call as usize, trampoline::<F>);
        }
        if let Some(payload) = call.panic {
            std::panic::resume_unwind(payload);
        }
    }

    pub fn request_gc(lock: &mut jsg::Lock<'_>) {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { ffi::request_gc(lock.isolate().as_ffi(), ffi::GcType::Full) };
    }

    /// Runs the pending microtasks, such as promise reactions, as the event loop does after
    /// each task. The isolate's microtask policy is explicit, so nothing else runs them.
    pub fn run_microtasks(lock: &mut jsg::Lock<'_>) {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { ffi::run_microtasks(lock.isolate().as_ffi()) };
    }

    pub fn request_minor_gc(lock: &mut jsg::Lock<'_>) {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { ffi::request_gc(lock.isolate().as_ffi(), ffi::GcType::Minor) };
    }

    /// Terminates execution the way running out of CPU time does. On return the termination
    /// exception is pending and `lock.is_execution_terminating()` is true. (`terminate_execution`
    /// only requests a termination; V8 delivers it the next time JavaScript runs.)
    pub fn terminate_and_deliver(lock: &mut jsg::Lock<'_>) {
        // SAFETY: isolate is valid and locked, with a context entered (guaranteed by Lock).
        unsafe { ffi::terminate_and_deliver(lock.isolate().as_ffi()) };
    }

    /// Runs `f` with the isolate unlocked, as code that runs on a thread without the isolate
    /// lock would, and locks it again afterwards. `lock` is borrowed for the duration, so `f`
    /// cannot use any handle; it must not touch the isolate in any other way either.
    ///
    /// # Panics
    ///
    /// Panics if `f` panics, once the isolate is locked again.
    pub fn with_isolate_unlocked<F: FnOnce() -> R, R>(lock: &mut jsg::Lock<'_>, f: F) -> R {
        struct Call<F, R> {
            f: Option<F>,
            result: Option<Result<R, Box<dyn std::any::Any + Send>>>,
        }

        fn trampoline<F: FnOnce() -> R, R>(data: usize) {
            // SAFETY: data was cast from &raw mut Call<F, R> in with_isolate_unlocked below.
            let call = unsafe { &mut *(data as *mut Call<F, R>) };
            let Some(f) = call.f.take() else {
                return;
            };
            // The scopes open on this thread stand for the isolate lock, which `f` runs
            // without, so they are set aside while it runs.
            // SAFETY: `f` has no handle to use, and opens no scope.
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| unsafe {
                jsg::allow_scopes_of_another_isolate(f)
            }));
            call.result = Some(result);
        }

        let mut call = Call::<F, R> {
            f: Some(f),
            result: None,
        };
        // SAFETY: the isolate is locked (guaranteed by Lock), and `call` is valid for the
        // duration of run_unlocked.
        unsafe {
            ffi::run_unlocked(
                lock.isolate().as_ffi(),
                &raw mut call as usize,
                trampoline::<F, R>,
            );
        }
        match call.result.expect("the callback ran") {
            Ok(result) => result,
            Err(payload) => std::panic::resume_unwind(payload),
        }
    }

    /// The bytes of the isolate's global and traced handle nodes in use: grows when a `Global`
    /// or `TracedGlobal` is created or GC installs a traced reference, and shrinks when V8 frees
    /// a node.
    ///
    /// Tests compare it with counts taken earlier, which encodes V8's handle accounting: the
    /// sizes of global and traced handle nodes, and the handles V8 itself keeps in use, such as
    /// the one each `run_in_context` call leaves for its context. A comparison that fails after
    /// a V8 update may mean that this accounting changed rather than that a handle leaked.
    #[must_use]
    pub fn used_global_handles_size(lock: &mut jsg::Lock<'_>) -> usize {
        // SAFETY: isolate is valid and locked (guaranteed by Lock).
        unsafe { ffi::used_global_handles_size(lock.isolate().as_ffi()) }
    }

    /// Creates a V8 object tagged with the C++ `WORKERD_WRAPPABLE_TAG`.
    /// Used to test that Rust unwrap rejects non-Rust wrappable objects.
    pub fn create_cpp_tagged_object<'s>(lock: &mut jsg::Lock<'s>) -> v8::Local<'s, v8::Value> {
        let isolate = lock.isolate();
        // SAFETY: `lock` proves the isolate is locked and a HandleScope for `'s` is open.
        unsafe {
            let local = ffi::create_cpp_tagged_object(isolate.as_ffi());
            v8::Local::from_ffi(local)
        }
    }

    /// Calls a function that C++ JSG wrapped, whose data is an opaque wrapper like that of a
    /// `v8::Function::new` function, and returns whether the data is taken for a Rust Wrappable.
    pub fn cpp_wrapped_function_data_is_rust_wrappable(lock: &mut jsg::Lock<'_>) -> bool {
        // SAFETY: `lock` proves the isolate is locked, a context is entered and a HandleScope is
        // open.
        unsafe { ffi::cpp_wrapped_function_data_is_rust_wrappable(lock.isolate().as_ffi()) }
    }

    /// Serializes `value` with C++ JSG's `jsg::Serializer`, to compare with
    /// [`jsg::ser::serialize`].
    ///
    /// # Panics
    ///
    /// Panics if serialization throws.
    pub fn cpp_serialize<'s>(lock: &mut jsg::Lock<'s>, value: v8::Local<'s, v8::Value>) -> Vec<u8> {
        // SAFETY: `lock` proves the isolate is locked, a context is entered and a HandleScope is
        // open, which `value` belongs to.
        unsafe { ffi::cpp_serialize(lock.isolate().as_ffi(), value.into_ffi()) }
            .expect("C++ JSG should serialize the value")
    }

    /// Deserializes `data` with C++ JSG's `jsg::Deserializer`, to compare with
    /// [`jsg::ser::deserialize`].
    ///
    /// # Panics
    ///
    /// Panics if deserialization throws.
    pub fn cpp_deserialize<'s>(lock: &mut jsg::Lock<'s>, data: &[u8]) -> v8::Local<'s, v8::Value> {
        // SAFETY: as for `cpp_serialize`; the result belongs to the lock's scope.
        unsafe {
            v8::Local::from_ffi(
                ffi::cpp_deserialize(lock.isolate().as_ffi(), data)
                    .expect("C++ JSG should deserialize the data"),
            )
        }
    }

    /// Holds a value the way C++ code holds one that Rust created. C++ calls `create`, as it
    /// would call a crate's `extern "Rust"` constructor, and keeps the result only in a
    /// `v8::Global` while a full GC runs. It then calls `check` with the value, resets the
    /// `Global` and runs a full GC again.
    ///
    /// `create` and `check` run with the isolate locked, a context entered and a `HandleScope`
    /// open; `lock` is borrowed for the duration, so they open their own scopes on the isolate.
    pub fn hold_across_gc(
        lock: &mut jsg::Lock<'_>,
        create: fn(*mut v8::ffi::Isolate) -> v8::ffi::MaybeLocal,
        check: fn(*mut v8::ffi::Isolate, v8::ffi::Local),
    ) {
        // SAFETY: the isolate is valid and locked with a context entered (guaranteed by Lock).
        unsafe { ffi::hold_across_gc(lock.isolate().as_ffi(), create, check) };
    }
}

/// Whether `a` and `b` belong to the same `v8::IsolateGroup`.
pub fn isolates_share_group(a: &jsg::Isolate, b: &jsg::Isolate) -> bool {
    // SAFETY: both references keep their isolates alive; reading an isolate's group needs no
    // lock.
    unsafe { ffi::isolates_share_group(a.as_ffi(), b.as_ffi()) }
}

impl Default for Harness {
    fn default() -> Self {
        Self::new()
    }
}
