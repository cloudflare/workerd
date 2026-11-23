// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The runner: after the `bench()` handler has registered its cases, it measures its own
//! overhead (once per thread) and runs the cases, then settles the promise `bench_run()`
//! returned with the group, encoded as a `BenchReport.Group` message in an `ArrayBuffer`.
//!
//! The runner is a state machine in the controller, which promise reactions and the yields
//! between batches advance. Its reactions are registered with [`IoContext::then_or_catch`], with
//! a [`jsg::Rc`] of the controller as their data, so that a reaction that runs in another request
//! fails, as the continuations that C++ `IoContext::addFunctor()` wraps do. Each step that can run
//! JavaScript runs in a [`jsg::Lock::try_catch`]: what a case throws, or a promise of it
//! rejects with, fails the case, and the runner goes on to the next one. What fails the runner
//! itself, such as measuring the overhead, rejects the promise.

use std::future::Future;
use std::pin::Pin;
use std::task::Context;
use std::task::Poll;

use bench_capnp::bench_report::case::Status;
use bench_core::CaseResult;
use bench_core::GlobFilter;
use bench_core::GroupResult;
use bench_core::Overhead;
use bench_core::QUICK_MIN_TIME_NS;
use bench_core::QUICK_SAMPLES;
use bench_core::Timing;
use bench_core::clock;
use io_context::IoContext;
use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;

use crate::BenchController;
use crate::Budget;
use crate::Case;
use crate::MAX_CALLS_PER_BATCH;
use crate::QUICK_WARMUP_NS;
use crate::describe_exception;

/// A calibrated batch takes at least this long, so that clock resolution and the cost of reading
/// the clocks don't matter.
const MIN_BATCH_NS: u64 = 1_000_000;

/// Budgets for the cases that measure overhead, which are capped further under `--quick`.
const OVERHEAD_MIN_TIME_NS: u64 = 200_000_000;
const OVERHEAD_WARMUP_NS: u64 = 50_000_000;
const OVERHEAD_SAMPLES: u32 = 20;

const MIXED_RESULTS: &str = "The function returned a promise from some calls but not from others. \
                             It must either always or never return a promise.";

thread_local! {
    /// The overhead is measured once per thread. `workerd bench` runs every group on the same
    /// thread, so all groups in a run share one measurement.
    static MEASURED_OVERHEAD: std::cell::Cell<Option<Overhead>> =
        const { std::cell::Cell::new(None) };
}

/// The cases the runner is going through.
#[derive(Debug, Clone, Copy, Default)]
enum Stage {
    /// The internal cases, from `begin`, that measure the overhead.
    Overhead { begin: usize },
    /// The handler's cases.
    #[default]
    Cases,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Phase {
    Warmup,
    Calibrate,
    Sample,
}

/// The case in progress.
#[derive(Debug)]
struct CaseRun {
    phase: Phase,
    calls: u64,
    warmed_up_ns: u64,
    /// Calibration grows batches until one takes at least this long.
    target_batch_ns: u64,
    wall_ns: Vec<f64>,
    cpu_ns: Vec<f64>,
    /// The calls of the batch in progress that are still to be made.
    remaining: u64,
    /// The clocks when the batch in progress started, and then how long it took.
    start: Timing,
    timing: Timing,
}

/// The runner's state.
#[derive(Debug, Default)]
pub struct Run {
    stage: Stage,
    /// The case to run next, or the case in progress, and the end of the stage's cases.
    index: usize,
    end: usize,
    results: Vec<CaseResult>,
    case: Option<CaseRun>,
    filter: Option<GlobFilter>,
    overhead: Option<Overhead>,
}

// Holds no JavaScript handles; its fields are plain data from bench-core.
jsg::jsg_traced_unchecked!(Run);

/// A reaction of the runner, which gets the controller.
trait Reaction:
    Copy
    + 'static
    + for<'a> Fn(
        &mut jsg::Lock<'a>,
        &IoContext<'a>,
        &jsg::Rc<BenchController>,
        v8::Local<'a, v8::Value>,
    ) -> jsg::Result<v8::Local<'a, v8::Value>>
{
}

impl<F> Reaction for F where
    F: Copy
        + 'static
        + for<'a> Fn(
            &mut jsg::Lock<'a>,
            &IoContext<'a>,
            &jsg::Rc<BenchController>,
            v8::Local<'a, v8::Value>,
        ) -> jsg::Result<v8::Local<'a, v8::Value>>
{
}

/// A future that is pending once, so that awaiting it yields to the event loop, as
/// `kj::evalLater()` does.
struct YieldOnce(bool);

impl Future for YieldOnce {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if self.0 {
            return Poll::Ready(());
        }
        self.0 = true;
        cx.waker().wake_by_ref();
        Poll::Pending
    }
}

/// Starts the `bench()` handler `bench` of `handler`, with `controller`, and returns the promise
/// that the group settles; see the module documentation.
pub fn start<'s>(
    lock: &mut jsg::Lock<'s>,
    controller: &jsg::Rc<BenchController>,
    bench: v8::Local<'s, v8::Function>,
    handler: v8::Local<'s, v8::Value>,
    env: v8::Local<'s, v8::Value>,
    ctx: v8::Local<'s, v8::Value>,
) -> jsg::Result<v8::Local<'s, v8::Promise>> {
    let resolver = v8::PromiseResolver::new(lock)?;
    let promise = resolver.promise();
    *controller.get(lock).resolver.borrow_mut(lock) = Some(resolver.to_traced_global(lock));

    let controller_js = controller.clone(lock).to_js(lock)?;
    let returned = lock.try_catch(|lock| {
        let returned = bench.call(lock, Some(handler), &[controller_js, env, ctx])?;
        Ok(v8::Local::<v8::Promise>::from(v8::VoidPromise::from_js(
            lock, returned,
        )?))
    })?;
    let returned = match returned {
        Ok(returned) => returned,
        Err(exception) => v8::Promise::rejected(lock, exception)?,
    };
    react_to(
        lock,
        controller,
        returned,
        on_handler_fulfilled,
        on_handler_rejected,
    )?;
    Ok(promise)
}

/// Registers `on_fulfilled` and `on_rejected` on `promise`, to run in the request's `IoContext`
/// with the controller.
fn react_to<'s>(
    lock: &mut jsg::Lock<'s>,
    this: &jsg::Rc<BenchController>,
    promise: v8::Local<'s, v8::Promise>,
    on_fulfilled: impl Reaction,
    on_rejected: impl Reaction,
) -> jsg::Result<()> {
    let data = this.clone(lock);
    IoContext::current(lock)?.then_or_catch(lock, promise, data, on_fulfilled, on_rejected)?;
    Ok(())
}

/// Runs `f`, a step of the runner, for a reaction. What `f` fails with rejects the group's
/// promise; a termination propagates.
fn drive<'a>(
    lock: &mut jsg::Lock<'a>,
    this: &jsg::Rc<BenchController>,
    f: impl FnOnce(&mut jsg::Lock<'a>) -> jsg::Result<()>,
) -> jsg::Result<()> {
    if let Err(exception) = lock.try_catch(f)? {
        settle(lock, this, Err(exception))?;
    }
    Ok(())
}

/// A promise reaction that runs `f`; see [`drive`].
fn react<'a>(
    lock: &mut jsg::Lock<'a>,
    this: &jsg::Rc<BenchController>,
    f: impl FnOnce(&mut jsg::Lock<'a>, &jsg::Rc<BenchController>) -> jsg::Result<()>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    drive(lock, this, |lock| f(lock, this))?;
    Ok(lock.undefined())
}

/// Runs `f`, a step of the case in progress, which returns the case's result once the case has
/// finished. What `f` fails with fails the case. The runner goes on to the next case outside
/// `f`, so that what fails the runner then, such as measuring the overhead, rejects the group's
/// promise rather than failing a case that has finished.
fn case_step<'a>(
    lock: &mut jsg::Lock<'a>,
    this: &jsg::Rc<BenchController>,
    f: impl FnOnce(&mut jsg::Lock<'a>) -> jsg::Result<Option<CaseResult>>,
) -> jsg::Result<()> {
    let finished = match lock.try_catch(f)? {
        Ok(finished) => finished,
        Err(exception) => Some(fail_case(lock, this, exception)?),
    };
    match finished {
        Some(result) => finish_case(lock, this, result),
        None => Ok(()),
    }
}

fn on_handler_fulfilled<'a>(
    lock: &mut jsg::Lock<'a>,
    _context: &IoContext<'a>,
    this: &jsg::Rc<BenchController>,
    _value: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    react(lock, this, start_cases)
}

/// The handler threw, or its promise rejected: the group fails with the exception.
fn on_handler_rejected<'a>(
    lock: &mut jsg::Lock<'a>,
    _context: &IoContext<'a>,
    this: &jsg::Rc<BenchController>,
    exception: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    react(lock, this, |lock, this| {
        let error = describe_exception(lock, exception)?;
        settle_group(
            lock,
            this,
            &GroupResult {
                error: Some(error),
                ..GroupResult::default()
            },
        )
    })
}

fn on_setup_fulfilled<'a>(
    lock: &mut jsg::Lock<'a>,
    _context: &IoContext<'a>,
    this: &jsg::Rc<BenchController>,
    state: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    react(lock, this, |lock, this| {
        case_step(lock, this, |lock| start_batch(lock, this, state))
    })
}

fn on_call_fulfilled<'a>(
    lock: &mut jsg::Lock<'a>,
    _context: &IoContext<'a>,
    this: &jsg::Rc<BenchController>,
    _value: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    react(lock, this, |lock, this| {
        case_step(lock, this, |lock| call_batch(lock, this))
    })
}

fn on_batch_timed<'a>(
    lock: &mut jsg::Lock<'a>,
    _context: &IoContext<'a>,
    this: &jsg::Rc<BenchController>,
    _value: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    react(lock, this, |lock, this| {
        case_step(lock, this, |lock| tear_down(lock, this))
    })
}

fn on_teardown_fulfilled<'a>(
    lock: &mut jsg::Lock<'a>,
    _context: &IoContext<'a>,
    this: &jsg::Rc<BenchController>,
    _value: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    react(lock, this, |lock, this| {
        case_step(lock, this, |lock| next_batch(lock, this))
    })
}

/// A promise of the case in progress rejected: the case fails with the exception.
fn on_case_rejected<'a>(
    lock: &mut jsg::Lock<'a>,
    _context: &IoContext<'a>,
    this: &jsg::Rc<BenchController>,
    exception: v8::Local<'a, v8::Value>,
) -> jsg::Result<v8::Local<'a, v8::Value>> {
    react(lock, this, |lock, this| {
        let result = fail_case(lock, this, exception)?;
        finish_case(lock, this, result)
    })
}

/// Runs the registered cases, after the handler has registered them.
fn start_cases(lock: &mut jsg::Lock<'_>, this: &jsg::Rc<BenchController>) -> jsg::Result<()> {
    let controller = this.get(lock);
    controller.running.set(true);
    let count = controller.cases.borrow().len();
    if count == 0 {
        return settle_group(
            lock,
            this,
            &GroupResult {
                error: Some(b"bench() did not register any cases.".to_vec()),
                ..GroupResult::default()
            },
        );
    }
    {
        let mut run = controller.run.borrow_mut();
        let filter: &[u8] = match controller.case_filter.as_slice() {
            b"" => b"*",
            filter => filter,
        };
        run.filter = Some(GlobFilter::new(filter));
    }

    if let Some(overhead) = MEASURED_OVERHEAD.get() {
        controller.run.borrow_mut().overhead = Some(overhead);
        return start_stage(lock, this, Stage::Cases, 0, count);
    }

    // The internal cases go after the handler's, and are removed once they have run.
    // The empty function is a V8 callback that does nothing, so that a call of it costs no
    // more than the call itself.
    let empty = v8::Function::from_callback(lock, empty_callback)?;
    let empty_async = v8::Function::from_callback(
        lock,
        jsg::function_callback(|lock, args| {
            let promise = (|| {
                let resolver = v8::PromiseResolver::new(lock)?;
                let undefined = lock.undefined();
                resolver.resolve(lock, undefined)?;
                jsg::Result::Ok(resolver.promise())
            })();
            match promise {
                Ok(promise) => args.set_return_value(promise.into()),
                Err(err) => lock.throw_exception(&err),
            }
        }),
    )?;
    // `blackBox(undefined)`, called as a method of this controller.
    let wrapper = this.clone(lock).to_js(lock)?;
    let black_box = v8::Local::<v8::Object>::try_from(wrapper)
        .map_err(|_| jsg::Error::new_internal_error("the controller's wrapper is not an object"))?
        .get(lock, "blackBox")?;
    let black_box = v8::Local::<v8::Function>::try_from(black_box).map_err(|_| {
        jsg::Error::new_internal_error("the controller's blackBox is not a function")
    })?;

    let controller = this.get(lock);
    let quick = controller.quick;
    let budget = Budget {
        min_time_ns: if quick {
            QUICK_MIN_TIME_NS
        } else {
            OVERHEAD_MIN_TIME_NS
        },
        warmup_ns: if quick {
            QUICK_WARMUP_NS
        } else {
            OVERHEAD_WARMUP_NS
        },
        samples: if quick {
            QUICK_SAMPLES
        } else {
            OVERHEAD_SAMPLES
        },
        batch: 0,
    };
    let added = [
        internal_case(lock, b"(empty)", budget, empty, None),
        internal_case(lock, b"(empty async)", budget, empty_async, None),
        internal_case(lock, b"(blackBox)", budget, black_box, Some(wrapper)),
    ];
    controller.cases.borrow_mut(lock).extend(added);
    let end = controller.cases.borrow().len();
    start_stage(lock, this, Stage::Overhead { begin: count }, count, end)
}

/// A case that measures overhead by calling `func`, with `receiver` as `this` and an undefined
/// argument if there is one.
fn internal_case<'s>(
    lock: &mut jsg::Lock<'s>,
    name: &[u8],
    budget: Budget,
    func: v8::Local<'s, v8::Function>,
    receiver: Option<v8::Local<'s, v8::Value>>,
) -> Case {
    Case {
        name: name.to_vec(),
        func: func.to_traced_global(lock),
        receiver: receiver.map(|receiver| receiver.to_traced_global(lock)),
        pass_argument: receiver.is_some(),
        setup: None,
        teardown: None,
        options: None,
        skip: false,
        budget,
        internal: true,
        returns_promise: std::cell::Cell::new(None),
    }
}

fn start_stage(
    lock: &mut jsg::Lock<'_>,
    this: &jsg::Rc<BenchController>,
    stage: Stage,
    index: usize,
    end: usize,
) -> jsg::Result<()> {
    {
        let mut run = this.get(lock).run.borrow_mut();
        run.stage = stage;
        run.index = index;
        run.end = end;
        run.results.clear();
    }
    next_case(lock, this)
}

/// Starts the next case of the stage that the filter selects, or finishes the stage.
fn next_case(lock: &mut jsg::Lock<'_>, this: &jsg::Rc<BenchController>) -> jsg::Result<()> {
    let controller = this.get(lock);
    loop {
        let cases = controller.cases.borrow();
        let mut run = controller.run.borrow_mut();
        while run.index < run.end {
            let case = &cases[run.index];
            if case.internal
                || run
                    .filter
                    .as_ref()
                    .is_none_or(|filter| filter.matches(&case.name))
            {
                break;
            }
            run.index += 1;
        }
        if run.index == run.end {
            drop((cases, run));
            return finish_stage(lock, this);
        }

        let case = &cases[run.index];
        if case.skip {
            run.results.push(CaseResult {
                name: case.name.clone(),
                status: Status::Skipped,
                error: None,
                iterations_per_sample: 0,
                wall_ns: Vec::new(),
                cpu_ns: Vec::new(),
                returns_promise: false,
            });
            run.index += 1;
            continue;
        }

        let budget = case.budget;
        run.case = Some(CaseRun {
            phase: if budget.warmup_ns > 0 {
                Phase::Warmup
            } else if budget.batch > 0 {
                Phase::Sample
            } else {
                Phase::Calibrate
            },
            calls: if budget.batch > 0 && budget.warmup_ns == 0 {
                budget.batch
            } else {
                1
            },
            warmed_up_ns: 0,
            target_batch_ns: (budget.min_time_ns / u64::from(budget.samples)).max(MIN_BATCH_NS),
            wall_ns: Vec::new(),
            cpu_ns: Vec::new(),
            remaining: 0,
            start: Timing::default(),
            timing: Timing::default(),
        });
        drop((cases, run));
        return step(lock, this);
    }
}

/// Yields to the event loop, so that pending I/O and timers make progress outside the timed
/// region, then runs the next batch.
fn step(lock: &mut jsg::Lock<'_>, this: &jsg::Rc<BenchController>) -> jsg::Result<()> {
    let context = IoContext::current(lock)?;
    let this = this.clone(lock);
    context.await_io_and_run(YieldOnce(false), move |lock, ()| {
        drive(lock, &this, |lock| {
            case_step(lock, &this, |lock| run_batch(lock, &this))
        })
    })
}

/// Runs the setup of a batch, if the case has one, then the batch.
fn run_batch(
    lock: &mut jsg::Lock<'_>,
    this: &jsg::Rc<BenchController>,
) -> jsg::Result<Option<CaseResult>> {
    let controller = this.get(lock);
    let (setup, receiver) = {
        let cases = controller.cases.borrow();
        let case = &cases[controller.run.borrow().index];
        (
            case.setup.as_ref().map(|setup| setup.as_local(lock)),
            case.options.as_ref().map(|options| options.as_local(lock)),
        )
    };
    let Some(setup) = setup else {
        let undefined = lock.undefined();
        return start_batch(lock, this, undefined);
    };
    let state = setup.call(lock, receiver, &[])?;
    let state = v8::Promise::resolved(lock, state)?;
    react_to(lock, this, state, on_setup_fulfilled, on_case_rejected)?;
    Ok(None)
}

/// Times a batch, whose calls get `state`.
fn start_batch<'s>(
    lock: &mut jsg::Lock<'s>,
    this: &jsg::Rc<BenchController>,
    state: v8::Local<'s, v8::Value>,
) -> jsg::Result<Option<CaseResult>> {
    let controller = this.get(lock);
    *controller.state.borrow_mut(lock) = Some(state.to_traced_global(lock));
    {
        let mut run = controller.run.borrow_mut();
        let Some(case) = run.case.as_mut() else {
            unreachable!("a batch starts in a case");
        };
        case.remaining = case.calls;
        case.start = clock::now();
    }
    call_batch(lock, this)
}

/// The callback of the `(empty)` case, which V8 calls directly.
#[expect(
    unsafe_code,
    reason = "a V8 function callback, which reads nothing of its arguments"
)]
unsafe extern "C" fn empty_callback(_info: *mut v8::ffi::FunctionCallbackInfo) {}

/// Makes the rest of the batch's calls. A call that returns a promise is awaited before the
/// next; the batch's time includes the wait.
fn call_batch(
    lock: &mut jsg::Lock<'_>,
    this: &jsg::Rc<BenchController>,
) -> jsg::Result<Option<CaseResult>> {
    let controller = this.get(lock);
    let undefined = lock.undefined();
    let (func, receiver, argument_count, mut returns_promise) = {
        let cases = controller.cases.borrow();
        let case = &cases[controller.run.borrow().index];
        (
            case.func.as_local(lock),
            Some(
                case.receiver
                    .as_ref()
                    .map_or(undefined, |receiver| receiver.as_local(lock)),
            ),
            usize::from(case.setup.is_some() || case.pass_argument),
            case.returns_promise.get(),
        )
    };
    let state = controller
        .state
        .borrow()
        .as_ref()
        .map_or(undefined, |state| state.as_local(lock));
    let arguments = [state];
    let arguments = &arguments[..argument_count];
    let mut remaining = match &controller.run.borrow().case {
        Some(case) => case.remaining,
        None => unreachable!("a batch runs in a case"),
    };

    let mut awaiting = false;
    let result = (|| -> jsg::Result<()> {
        while remaining > 0 {
            remaining -= 1;
            awaiting = lock.with_handle_scope(|lock, _| -> jsg::Result<bool> {
                let result = func.call(lock, receiver, arguments)?;
                let promise = v8::Local::<v8::Promise>::try_from(result).ok();
                match returns_promise {
                    Some(expected) if expected != promise.is_some() => {
                        return Err(jsg::Error::new_error(MIXED_RESULTS));
                    }
                    _ => returns_promise = Some(promise.is_some()),
                }
                let Some(promise) = promise else {
                    return Ok(false);
                };
                react_to(lock, this, promise, on_call_fulfilled, on_case_rejected)?;
                Ok(true)
            })?;
            if awaiting {
                return Ok(());
            }
        }
        Ok(())
    })();
    // The clocks are read once the batch is done, not before each wait.
    let end = if awaiting {
        Timing::default()
    } else {
        clock::now()
    };
    let controller = this.get(lock);
    controller.cases.borrow()[controller.run.borrow().index]
        .returns_promise
        .set(returns_promise);
    result?;
    {
        let mut run = controller.run.borrow_mut();
        let Some(case) = run.case.as_mut() else {
            unreachable!("a batch runs in a case");
        };
        case.remaining = remaining;
        if awaiting {
            return Ok(None);
        }
        case.timing = end - case.start;
    }

    // The teardown and the next batch run in a reaction, as when the batch's result is a promise.
    let undefined = lock.undefined();
    let resolved = v8::Promise::resolved(lock, undefined)?;
    react_to(lock, this, resolved, on_batch_timed, on_case_rejected)?;
    Ok(None)
}

/// Calls the case's teardown, if it has one, with the batch's state, then goes on.
fn tear_down(
    lock: &mut jsg::Lock<'_>,
    this: &jsg::Rc<BenchController>,
) -> jsg::Result<Option<CaseResult>> {
    let controller = this.get(lock);
    let undefined = lock.undefined();
    let state = controller
        .state
        .borrow_mut(lock)
        .take()
        .map_or(undefined, |state| state.as_local(lock));
    let (teardown, receiver) = {
        let cases = controller.cases.borrow();
        let case = &cases[controller.run.borrow().index];
        (
            case.teardown
                .as_ref()
                .map(|teardown| teardown.as_local(lock)),
            case.options.as_ref().map(|options| options.as_local(lock)),
        )
    };
    let Some(teardown) = teardown else {
        return next_batch(lock, this);
    };
    let result = teardown.call(lock, receiver, &[state])?;
    let result = v8::Promise::resolved(lock, result)?;
    react_to(lock, this, result, on_teardown_fulfilled, on_case_rejected)?;
    Ok(None)
}

/// Advances the case's phase with the batch just timed, then runs the next batch, or returns the
/// case's result once it has all its samples.
#[expect(
    clippy::cast_precision_loss,
    reason = "samples are per-call times, which the report holds as doubles"
)]
fn next_batch(
    lock: &mut jsg::Lock<'_>,
    this: &jsg::Rc<BenchController>,
) -> jsg::Result<Option<CaseResult>> {
    let controller = this.get(lock);
    let finished = {
        let cases = controller.cases.borrow();
        let mut run = controller.run.borrow_mut();
        let index = run.index;
        let case = &cases[index];
        let Some(case_run) = run.case.as_mut() else {
            unreachable!("a batch runs in a case");
        };
        let timing = case_run.timing;
        let wall_ns = timing.wall_ns.max(0).unsigned_abs();
        let grow = |case_run: &mut CaseRun| {
            if wall_ns < case_run.target_batch_ns && case_run.calls < MAX_CALLS_PER_BATCH {
                case_run.calls *= 2;
                true
            } else {
                false
            }
        };
        match case_run.phase {
            Phase::Warmup => {
                case_run.warmed_up_ns += wall_ns;
                if case_run.warmed_up_ns < case.budget.warmup_ns {
                    grow(case_run);
                } else if case.budget.batch > 0 {
                    case_run.phase = Phase::Sample;
                    case_run.calls = case.budget.batch;
                } else {
                    case_run.phase = Phase::Calibrate;
                }
                None
            }
            Phase::Calibrate => {
                if !grow(case_run) {
                    case_run.phase = Phase::Sample;
                }
                None
            }
            Phase::Sample => {
                let calls = case_run.calls as f64;
                case_run.wall_ns.push(timing.wall_ns as f64 / calls);
                case_run.cpu_ns.push(timing.cpu_ns as f64 / calls);
                let sampled = case_run.wall_ns.len() == case.budget.samples as usize;
                sampled
                    .then(|| run.case.take())
                    .flatten()
                    .map(|case_run| CaseResult {
                        name: case.name.clone(),
                        status: Status::Ok,
                        error: None,
                        iterations_per_sample: case_run.calls,
                        wall_ns: case_run.wall_ns,
                        cpu_ns: case_run.cpu_ns,
                        returns_promise: case.returns_promise.get().unwrap_or(false),
                    })
            }
        }
    };
    if finished.is_none() {
        step(lock, this)?;
    }
    Ok(finished)
}

/// The case in progress failed with `exception`: returns its result.
fn fail_case<'s>(
    lock: &mut jsg::Lock<'s>,
    this: &jsg::Rc<BenchController>,
    exception: v8::Local<'s, v8::Value>,
) -> jsg::Result<CaseResult> {
    let error = describe_exception(lock, exception)?;
    let controller = this.get(lock);
    controller.state.borrow_mut(lock).take();
    let name = controller.cases.borrow()[controller.run.borrow().index]
        .name
        .clone();
    controller.run.borrow_mut().case = None;
    Ok(CaseResult {
        name,
        status: Status::Failed,
        error: Some(error),
        iterations_per_sample: 0,
        wall_ns: Vec::new(),
        cpu_ns: Vec::new(),
        returns_promise: false,
    })
}

fn finish_case(
    lock: &mut jsg::Lock<'_>,
    this: &jsg::Rc<BenchController>,
    result: CaseResult,
) -> jsg::Result<()> {
    {
        let mut run = this.get(lock).run.borrow_mut();
        run.results.push(result);
        run.index += 1;
    }
    next_case(lock, this)
}

fn finish_stage(lock: &mut jsg::Lock<'_>, this: &jsg::Rc<BenchController>) -> jsg::Result<()> {
    let controller = this.get(lock);
    let (stage, results) = {
        let mut run = controller.run.borrow_mut();
        (run.stage, std::mem::take(&mut run.results))
    };
    match stage {
        Stage::Overhead { begin } => {
            controller.cases.borrow_mut(lock).truncate(begin);
            for case in &results {
                if case.status != Status::Ok {
                    return Err(jsg::Error::new_error(format!(
                        "Measuring the runner's overhead failed: {}",
                        String::from_utf8_lossy(case.error.as_deref().unwrap_or_default())
                    )));
                }
            }
            let [sync, async_, black_box] = results.as_slice() else {
                unreachable!("three internal cases ran");
            };
            debug_assert!(!sync.returns_promise && async_.returns_promise);
            let overhead = Overhead {
                sync_ns: bench_core::median(&sync.wall_ns),
                sync_cpu_ns: bench_core::median(&sync.cpu_ns),
                async_ns: bench_core::median(&async_.wall_ns),
                async_cpu_ns: bench_core::median(&async_.cpu_ns),
                black_box_ns: bench_core::median(&black_box.wall_ns),
            };
            MEASURED_OVERHEAD.set(Some(overhead));
            controller.run.borrow_mut().overhead = Some(overhead);
            start_stage(lock, this, Stage::Cases, 0, begin)
        }
        Stage::Cases => {
            let overhead = controller.run.borrow().overhead;
            settle_group(
                lock,
                this,
                &GroupResult {
                    error: None,
                    cases: results,
                    overhead,
                },
            )
        }
    }
}

/// Resolves the group's promise with `group`.
fn settle_group(
    lock: &mut jsg::Lock<'_>,
    this: &jsg::Rc<BenchController>,
    group: &GroupResult,
) -> jsg::Result<()> {
    let words = bench_core::encode_group(group)
        .map_err(|err| jsg::Error::new_internal_error(err.to_string()))?;
    let bytes: Vec<u8> = words.iter().flat_map(|word| word.to_ne_bytes()).collect();
    let buffer = v8::ArrayBuffer::new(lock, &bytes);
    settle(lock, this, Ok(buffer.into()))
}

/// Resolves the group's promise with `outcome`'s value, or rejects it with its exception. Does
/// nothing if it has already settled.
fn settle<'s>(
    lock: &mut jsg::Lock<'s>,
    this: &jsg::Rc<BenchController>,
    outcome: Result<v8::Local<'s, v8::Value>, v8::Local<'s, v8::Value>>,
) -> jsg::Result<()> {
    let Some(resolver) = this.get(lock).resolver.borrow_mut(lock).take() else {
        return Ok(());
    };
    let resolver = resolver.as_local(lock);
    match outcome {
        Ok(value) => resolver.resolve(lock, value),
        Err(exception) => resolver.reject(lock, exception),
    }
}
