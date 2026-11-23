// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! `BenchController`, which a Worker's `bench(b, env, ctx)` handler uses to register cases, and
//! the runner that times them. Only `workerd bench` dispatches the bench event, through
//! [`ffi`]'s `bench_run()`.
//!
//! The runtime runs the timing loop: user code supplies the function to time and never sees a
//! high-resolution clock. For each case, the runner calls the function repeatedly to warm up,
//! calibrates a batch size, then times `samples` batches, reading the monotonic and thread CPU
//! clocks only between batches. Each sample is a batch's time divided by its number of calls.
//!
//! The bench types aren't in the generated TypeScript definitions: only `workerd bench` delivers
//! the bench event, so they aren't part of the API that deployed Workers see.

mod ffi;
mod options;
mod runner;

use std::cell::Cell;
use std::cell::RefCell;

use bench_core::DEFAULT_MIN_TIME_NS;
use bench_core::DEFAULT_SAMPLES;
use bench_core::MAX_SAMPLES;
use bench_core::QUICK_MIN_TIME_NS;
use bench_core::QUICK_SAMPLES;
use jsg::FromJS;
use jsg::v8;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_traced;
pub use options::BenchCaseOptions;
pub use options::BenchDuration;
pub use options::BenchOptions;
use options::parse_count;
use options::parse_duration;

/// The warmup, when neither the case nor the command line sets one.
const DEFAULT_WARMUP_NS: u64 = 200_000_000;

/// Under `--quick`, warmups are capped at this.
const QUICK_WARMUP_NS: u64 = 2_000_000;

/// The most calls a batch makes.
const MAX_CALLS_PER_BATCH: u64 = 1 << 40;

/// How long a case may run for, and how often it is sampled.
#[jsg_traced]
#[derive(Debug, Clone, Copy)]
struct Budget {
    min_time_ns: u64,
    warmup_ns: u64,
    samples: u32,
    /// Calls per batch; 0 calibrates.
    batch: u64,
}

/// A registered case.
struct Case {
    name: Vec<u8>,
    func: v8::TracedGlobal<v8::Function>,
    /// `this` for calls of `func`; undefined if none.
    receiver: Option<v8::TracedGlobal<v8::Value>>,
    /// Whether `func` gets an argument when there is no `setup`, in which case it is undefined.
    pass_argument: bool,
    setup: Option<v8::TracedGlobal<v8::Function>>,
    teardown: Option<v8::TracedGlobal<v8::Function>>,
    /// The options object, which is `this` for `setup` and `teardown`, as C++ JSG calls the
    /// functions of a struct's fields with the struct's object.
    options: Option<v8::TracedGlobal<v8::Value>>,
    skip: bool,
    budget: Budget,
    /// A case that measures overhead, which the case filter doesn't apply to.
    internal: bool,
    /// Whether `func` returns a promise, once it has been called. Every call must agree, since
    /// the overhead subtracted from the samples depends on it.
    returns_promise: Cell<Option<bool>>,
}

impl jsg::Traced for Case {
    fn trace(&self, visitor: &mut v8::GcVisitor) {
        self.func.trace(visitor);
        self.receiver.trace(visitor);
        self.setup.trace(visitor);
        self.teardown.trace(visitor);
        self.options.trace(visitor);
    }
}

/// The first argument of a `bench()` handler.
#[jsg_resource]
pub struct BenchController {
    case_filter: Vec<u8>,
    quick: bool,
    defaults: Budget,
    cases: jsg::RefCell<Vec<Case>>,
    /// Set once the cases start running; registering a case after that is an error.
    running: Cell<bool>,
    /// The runner's state, which `runner` owns.
    run: RefCell<runner::Run>,
    /// The result of the setup of the batch in progress, which every call gets.
    state: jsg::RefCell<Option<v8::TracedGlobal<v8::Value>>>,
    /// Settles the promise `bench_run()` returned.
    resolver: jsg::RefCell<Option<v8::TracedGlobal<v8::PromiseResolver>>>,
}

#[jsg_resource]
impl BenchController {
    /// Registers a case. Cases run, in registration order, after `bench()` returns or its
    /// promise resolves. `fn` may return a promise, which is awaited before the next call.
    #[jsg_method]
    fn run<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        name: jsg::JsString<'s>,
        func: v8::Local<'s, v8::Function>,
        options: Option<v8::Local<'s, v8::Value>>,
    ) -> jsg::Result<()> {
        if self.running.get() {
            return Err(jsg::Error::new_error(
                "BenchController.run() must be called by bench(), before it returns or its \
                 promise resolves.",
            ));
        }
        let name = wtf8(lock, *name);
        if self.cases.borrow().iter().any(|c| c.name == name) {
            return Err(jsg::Error::new_error(format!(
                "A case named \"{}\" is already registered.",
                String::from_utf8_lossy(&name)
            )));
        }

        let options_object = options.unwrap_or_else(|| lock.undefined());
        let options = BenchCaseOptions::from_js(lock, options_object)?;
        let mut budget = self.defaults;
        if let Some(min_time) = &options.minTime {
            budget.min_time_ns = parse_duration(min_time, "minTime")?;
        }
        if let Some(warmup) = &options.warmup {
            budget.warmup_ns = parse_duration(warmup, "warmup")?;
        }
        if let Some(samples) = options.samples {
            budget.samples = u32::try_from(parse_count(samples, MAX_SAMPLES.into(), "samples")?)
                .unwrap_or_else(|_| unreachable!("at most MAX_SAMPLES"));
        }
        if let Some(batch) = options.batch {
            budget.batch = parse_count(batch, MAX_CALLS_PER_BATCH, "batch")?;
        }
        if self.quick {
            budget.min_time_ns = budget.min_time_ns.min(QUICK_MIN_TIME_NS);
            budget.warmup_ns = budget.warmup_ns.min(QUICK_WARMUP_NS);
            budget.samples = budget.samples.min(QUICK_SAMPLES);
        }
        let case = Case {
            name,
            func: func.to_traced_global(lock),
            receiver: None,
            pass_argument: false,
            setup: options.setup,
            teardown: options.teardown,
            options: Some(options_object.to_traced_global(lock)),
            skip: options.skip.unwrap_or(false),
            budget,
            internal: false,
            returns_promise: Cell::new(None),
        };
        self.cases.borrow_mut(lock).push(case);
        Ok(())
    }

    /// Returns `value`. Passing a result through it keeps V8 from optimizing away the code that
    /// computed it, since V8 can't see into the native call.
    #[jsg_method]
    fn black_box<'s>(&self, value: v8::Local<'s, v8::Value>) -> v8::Local<'s, v8::Value> {
        value
    }

    #[jsg_property(prototype, readonly)]
    fn get_options(&self) -> BenchOptions {
        BenchOptions {
            filter: match self.case_filter.as_slice() {
                b"" | b"*" => None,
                filter => Some(String::from_utf8_lossy(filter).into_owned()),
            },
            quick: self.quick,
        }
    }
}

impl BenchController {
    /// A controller for `params`, an encoded `BenchParams` message, in words.
    fn read(params: &[u64]) -> capnp::Result<Self> {
        let bytes: Vec<u8> = params.iter().flat_map(|word| word.to_ne_bytes()).collect();
        let message =
            capnp::serialize::read_message(bytes.as_slice(), capnp::message::ReaderOptions::new())?;
        let params: bench_capnp::bench_params::Reader<'_> = message.get_root()?;
        let defaults = params.get_defaults()?;
        let or = |value: u64, default: u64| if value > 0 { value } else { default };
        Ok(Self {
            case_filter: params.get_case_filter()?.as_bytes().to_vec(),
            quick: params.get_quick(),
            defaults: Budget {
                min_time_ns: or(defaults.get_min_time_ns(), DEFAULT_MIN_TIME_NS),
                warmup_ns: or(defaults.get_warmup_ns(), DEFAULT_WARMUP_NS),
                samples: match defaults.get_samples() {
                    0 => DEFAULT_SAMPLES,
                    samples => samples,
                },
                batch: defaults.get_batch(),
            },
            cases: jsg::RefCell::new(Vec::new()),
            running: Cell::new(false),
            run: RefCell::new(runner::Run::default()),
            state: jsg::RefCell::new(None),
            resolver: jsg::RefCell::new(None),
        })
    }
}

/// The string as WTF-8, as C++ JSG converts a string to a `kj::String`: a lone surrogate is kept
/// as its three bytes rather than replaced.
fn wtf8(lock: &mut jsg::Lock<'_>, string: v8::Local<'_, v8::String>) -> Vec<u8> {
    let mut bytes = vec![0; string.utf8_length(lock)];
    let written = string.write_utf8(lock, &mut bytes, v8::WriteFlags::None);
    bytes.truncate(written);
    bytes
}

/// The exception's stack if it has one, or else its string conversion, as WTF-8.
///
/// # Errors
/// Fails only while execution is terminating; what the conversion throws is caught.
fn describe_exception<'s>(
    lock: &mut jsg::Lock<'s>,
    exception: v8::Local<'s, v8::Value>,
) -> jsg::Result<Vec<u8>> {
    let description = lock.try_catch(|lock| {
        if let Ok(object) = v8::Local::<v8::Object>::try_from(exception) {
            let stack = object.get(lock, "stack")?;
            if let Ok(stack) = v8::Local::<v8::String>::try_from(stack) {
                return Ok(wtf8(lock, stack));
            }
        }
        let string = exception.to_js_string(lock)?;
        Ok(wtf8(lock, string))
    })?;
    Ok(description
        .unwrap_or_else(|_| b"(an exception that can't be converted to a string)".to_vec()))
}

#[cfg(test)]
#[path = "lib-test.rs"]
mod tests;
