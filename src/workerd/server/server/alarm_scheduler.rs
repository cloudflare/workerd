// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The alarm scheduler of a Durable Object namespace.
//!
//! It runs each root actor's alarm once it is due, retries a failed run with backoff, and keeps
//! every alarm in an `AlarmStorage` so that it survives a restart. It also keeps them all in
//! memory, and reads the storage only when it starts.
//!
//! An alarm is waiting for its time, started (its handler runs), or finished (it waits to retry,
//! or for its actor to be told that it was abandoned). One task runs each alarm. An alarm set
//! while one has started or finished is queued behind it, and runs once the handler is done,
//! instead of any retry.

use std::cell::RefCell;
use std::collections::HashMap;
use std::future::Future;
use std::rc::Rc;
use std::rc::Weak;
use std::task::Context;
use std::task::Poll;
use std::task::Waker;
use std::time::Duration;
use std::time::SystemTime;

use futures::future::LocalBoxFuture;
use outcome_capnp::EventOutcome;
use rand::Rng;

use crate::Result;

// keep in sync with `WorkerInterface::ALARM_RETRY_START_SECONDS` and
// `WorkerInterface::ALARM_RETRY_MAX_TRIES` in `src/workerd/io/worker-interface.h`
/// The delay before an alarm's first retry, in seconds. Each retry doubles it.
pub const RETRY_START_SECONDS: u64 = 2;
/// The number of retries that count against the limit (the handler failed) before the alarm is
/// abandoned.
pub const RETRY_MAX_TRIES: u32 = 6;
/// The most times the retry delay doubles, which retries that do not count against the limit
/// (internal errors) reach: 2 << 9 seconds is about 17 minutes.
pub const RETRY_BACKOFF_MAX: u32 = 9;
/// The largest random delay added to a retry, as a fraction of its delay, so that alarms that
/// failed together do not all retry at once.
pub const RETRY_JITTER_FACTOR: f64 = 0.25;

/// The root actor an alarm belongs to.
///
/// That is its id's hex, and the name it was created with by `idFromName()`, if any, so that
/// `ctx.id.name` is set when the alarm runs. The scheduler files alarms by the id alone.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ActorKey {
    pub id: String,
    pub name: Option<String>,
}

/// An alarm as the storage holds it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StoredAlarm {
    pub actor: ActorKey,
    pub scheduled_time: SystemTime,
}

/// Where the scheduler keeps its alarms: `AlarmStore` (`alarm_store.rs`) in the server.
pub trait AlarmStorage {
    fn load(&self) -> Result<Vec<StoredAlarm>>;
    /// Stores `actor`'s alarm at `scheduled_time`. The stored name changes only when `actor`
    /// has one. Returns whether a row changed.
    fn set(&self, actor: &ActorKey, scheduled_time: SystemTime) -> Result<bool>;
    /// Removes the alarm of the actor with id `id`. Returns whether it had one.
    fn remove(&self, id: &str) -> Result<bool>;
    fn clear(&self) -> Result<()>;
}

/// What the scheduler runs on.
pub trait AlarmEnv {
    /// The wall clock that alarm times are compared with.
    fn now(&self) -> SystemTime;
    /// Completes `delay` from now on the timer that alarms wait on.
    fn sleep(&self, delay: Duration) -> LocalBoxFuture<'static, ()>;
    /// Runs `task` until it ends, or until the environment does.
    fn spawn(&self, task: LocalBoxFuture<'static, ()>);
    /// The actor to run `actor`'s alarm on, or to tell that its alarm was abandoned.
    fn actor(&self, actor: &ActorKey) -> Result<Box<dyn worker::Interface>>;
}

/// The scheduler. Dropping it cancels every alarm's task; the storage keeps the alarms.
pub struct AlarmScheduler(Rc<Inner>);

impl AlarmScheduler {
    /// Loads the alarms in `storage` and schedules them, those already due to run at once.
    pub fn new(env: Rc<dyn AlarmEnv>, storage: Box<dyn AlarmStorage>) -> Result<Self> {
        let stored = storage.load()?;
        let inner = Rc::new_cyclic(|this| Inner {
            this: Weak::clone(this),
            env,
            storage,
            alarms: RefCell::new(HashMap::new()),
        });
        let now = inner.env.now();
        for alarm in stored {
            let scheduled = inner.schedule_alarm(now, &alarm.actor, alarm.scheduled_time);
            inner.alarms.borrow_mut().insert(alarm.actor.id, scheduled);
        }
        Ok(Self(inner))
    }

    /// The time of the actor's alarm. While its handler runs, only an alarm queued behind it.
    #[must_use]
    pub fn get_alarm(&self, id: &str) -> Option<SystemTime> {
        let alarms = self.0.alarms.borrow();
        let alarm = alarms.get(id)?;
        if alarm.status == Status::Started {
            alarm.queued_alarm
        } else {
            Some(alarm.scheduled_time)
        }
    }

    /// Sets `actor`'s alarm to `scheduled_time`: it replaces a waiting alarm, and queues behind
    /// one that has started or finished, even at the same time, as each set is a write the actor
    /// made. Returns whether the storage changed.
    pub fn set_alarm(&self, actor: &ActorKey, scheduled_time: SystemTime) -> Result<bool> {
        let changed = self.0.storage.set(actor, scheduled_time)?;
        let now = self.0.env.now();
        let replaced = {
            let mut alarms = self.0.alarms.borrow_mut();
            match alarms.get_mut(&actor.id) {
                None => {
                    let scheduled = self.0.schedule_alarm(now, actor, scheduled_time);
                    alarms.insert(actor.id.clone(), scheduled);
                    None
                }
                Some(alarm) if alarm.status != Status::Waiting => {
                    alarm.queued_alarm = Some(scheduled_time);
                    None
                }
                Some(alarm) => {
                    let actor = alarm.actor.clone();
                    let scheduled = self.0.schedule_alarm(now, &actor, scheduled_time);
                    Some(std::mem::replace(alarm, scheduled))
                }
            }
        };
        drop(replaced);
        Ok(changed)
    }

    /// Deletes the actor's alarm. A running alarm stays, and only an alarm queued behind it is
    /// dropped; a finished one is replaced by an alarm queued behind it, if any. Returns whether
    /// the storage had an alarm.
    pub fn delete_alarm(&self, id: &str) -> Result<bool> {
        let mut garbage = Garbage::default();
        self.0.delete_alarm(id, &mut garbage)
    }

    /// Cancels every alarm and removes them from the storage.
    pub fn delete_all(&self) -> Result<()> {
        let alarms = std::mem::take(&mut *self.0.alarms.borrow_mut());
        drop(alarms);
        self.0.storage.clear()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Status {
    Waiting,
    Started,
    Finished,
}

struct ScheduledAlarm {
    actor: ActorKey,
    scheduled_time: SystemTime,
    task: Task,
    queued_alarm: Option<SystemTime>,
    status: Status,
    previous_retry_counted_against_limit: bool,
    /// Counts the retry delay's doublings. It is separate from `counted_retry`, so that the
    /// backoff can start over without losing count of the retries.
    backoff: u32,
    /// Counts the retries that count against the limit.
    counted_retry: u32,
}

/// An alarm's run, as the scheduler decides what comes next.
struct AlarmOutcome {
    retry: bool,
    retry_counts_against_limit: bool,
    outcome: EventOutcome,
}

/// What the scheduler replaced while its alarms were borrowed: dropped after they are not, as
/// dropping a task drops what it awaits.
#[derive(Default)]
struct Garbage {
    alarms: Vec<ScheduledAlarm>,
    tasks: Vec<Task>,
}

struct Inner {
    this: Weak<Self>,
    env: Rc<dyn AlarmEnv>,
    storage: Box<dyn AlarmStorage>,
    alarms: RefCell<HashMap<String, ScheduledAlarm>>,
}

impl Inner {
    fn schedule_alarm(
        &self,
        now: SystemTime,
        actor: &ActorKey,
        scheduled_time: SystemTime,
    ) -> ScheduledAlarm {
        // An alarm that is due waits for no time; the timer then completes it on its next turn.
        let delay = scheduled_time.duration_since(now).unwrap_or_default();
        ScheduledAlarm {
            actor: actor.clone(),
            scheduled_time,
            task: self.alarm_task(delay, actor.clone(), scheduled_time),
            queued_alarm: None,
            status: Status::Waiting,
            previous_retry_counted_against_limit: false,
            backoff: 0,
            counted_retry: 0,
        }
    }

    fn alarm_task(&self, delay: Duration, actor: ActorKey, scheduled_time: SystemTime) -> Task {
        let future = run_alarm(
            Weak::clone(&self.this),
            Rc::clone(&self.env),
            delay,
            actor,
            scheduled_time,
        );
        Task::spawn(&*self.env, future)
    }

    fn abandon_task(&self, actor: ActorKey, scheduled_time: SystemTime) -> Task {
        let future = abandon_alarm(
            Weak::clone(&self.this),
            Rc::clone(&self.env),
            actor,
            scheduled_time,
        );
        Task::spawn(&*self.env, future)
    }

    fn delete_alarm(&self, id: &str, garbage: &mut Garbage) -> Result<bool> {
        let removed = self.storage.remove(id)?;
        let now = self.env.now();
        let mut alarms = self.alarms.borrow_mut();
        if let Some(alarm) = alarms.get_mut(id) {
            if let Some(queued) = alarm.queued_alarm {
                if alarm.status == Status::Started {
                    alarm.queued_alarm = None;
                } else {
                    let actor = alarm.actor.clone();
                    let scheduled = self.schedule_alarm(now, &actor, queued);
                    garbage.alarms.push(std::mem::replace(alarm, scheduled));
                }
            } else if alarm.status != Status::Started {
                garbage.alarms.extend(alarms.remove(id));
            }
        }
        Ok(removed)
    }

    /// Decides what follows a run of `actor`'s alarm at `scheduled_time`: the alarm queued
    /// behind it, a retry, its abandonment, or nothing. The task that ran it then ends.
    fn alarm_ran(
        &self,
        actor: &ActorKey,
        scheduled_time: SystemTime,
        outcome: &AlarmOutcome,
        garbage: &mut Garbage,
    ) -> Result<()> {
        let now = self.env.now();
        {
            let mut alarms = self.alarms.borrow_mut();
            let alarm = alarms
                .get_mut(&actor.id)
                .ok_or_else(|| kj::failed!("the alarm that ran is gone"))?;

            // An alarm queued behind this one makes retrying this one pointless.
            if let Some(queued) = alarm.queued_alarm {
                let queued_actor = alarm.actor.clone();
                let scheduled = self.schedule_alarm(now, &queued_actor, queued);
                garbage.alarms.push(std::mem::replace(alarm, scheduled));
                return Ok(());
            }

            // A finished alarm can be deleted while it waits to retry.
            alarm.status = Status::Finished;

            if outcome.retry {
                if alarm.counted_retry >= RETRY_MAX_TRIES {
                    // The actor clears its own alarm state, so that `getAlarm()` reflects the
                    // deletion. If telling it fails, the alarm stays here, not silently lost.
                    let task = self.abandon_task(actor.clone(), scheduled_time);
                    garbage.tasks.push(std::mem::replace(&mut alarm.task, task));
                    return Ok(());
                }
                if outcome.retry_counts_against_limit {
                    alarm.counted_retry += 1;
                    if !alarm.previous_retry_counted_against_limit {
                        // The last retry was for an internal error and this one is for the
                        // handler's, most likely a different error: retries the handler causes
                        // start their backoff over.
                        alarm.backoff = 0;
                    }
                }
                alarm.previous_retry_counted_against_limit = outcome.retry_counts_against_limit;

                alarm.backoff = alarm.backoff.min(RETRY_BACKOFF_MAX);
                let delay = Duration::from_secs(RETRY_START_SECONDS << alarm.backoff);
                let jitter = rand::rng().random_range(0..=max_jitter_ms(delay));
                let delay = delay + Duration::from_millis(jitter);
                alarm.backoff += 1;

                let task = self.alarm_task(delay, actor.clone(), scheduled_time);
                garbage.tasks.push(std::mem::replace(&mut alarm.task, task));
                return Ok(());
            }

            if outcome.outcome == EventOutcome::Aborted {
                let task = self.abandon_task(actor.clone(), scheduled_time);
                garbage.tasks.push(std::mem::replace(&mut alarm.task, task));
                return Ok(());
            }
        }
        self.delete_alarm(&actor.id, garbage)?;
        Ok(())
    }
}

/// The largest jitter added to a retry `delay` later, in whole milliseconds, as the C++ scheduler
/// computed it: a floating-point fraction of the delay, rounded down.
#[expect(
    clippy::cast_precision_loss,
    clippy::cast_sign_loss,
    reason = "a retry delay is at most 2 << 9 seconds, exact as a double, and its jitter positive"
)]
fn max_jitter_ms(delay: Duration) -> u64 {
    (RETRY_JITTER_FACTOR * delay.as_millis() as f64).floor() as u64
}

/// Completes once `delay` has passed on the timer and the clock has reached `scheduled_time`:
/// the timer may run ahead of the clock.
async fn check_timestamp(env: &dyn AlarmEnv, mut delay: Duration, scheduled_time: SystemTime) {
    loop {
        env.sleep(delay).await;
        match scheduled_time.duration_since(env.now()) {
            Ok(remaining) if !remaining.is_zero() => delay = remaining,
            _ => return,
        }
    }
}

/// The task of `actor`'s alarm at `scheduled_time`: it waits `delay`, runs the handler, and
/// decides what follows.
async fn run_alarm(
    this: Weak<Inner>,
    env: Rc<dyn AlarmEnv>,
    delay: Duration,
    actor: ActorKey,
    scheduled_time: SystemTime,
) {
    check_timestamp(&*env, delay, scheduled_time).await;
    let retry_count = {
        let Some(inner) = this.upgrade() else {
            return;
        };
        let mut alarms = inner.alarms.borrow_mut();
        let Some(alarm) = alarms.get_mut(&actor.id) else {
            tracing::error!("an alarm's task ran without its alarm");
            return;
        };
        alarm.status = Status::Started;
        alarm.counted_retry
    };

    let outcome = run_handler(&*env, &actor, scheduled_time, retry_count).await;

    let Some(inner) = this.upgrade() else {
        return;
    };
    let mut garbage = Garbage::default();
    if let Err(e) = inner.alarm_ran(&actor, scheduled_time, &outcome, &mut garbage) {
        tracing::error!(
            "Failed to run alarm and was unable to schedule a retry: {}",
            e.description()
        );
    }
}

async fn run_handler(
    env: &dyn AlarmEnv,
    actor: &ActorKey,
    scheduled_time: SystemTime,
    retry_count: u32,
) -> AlarmOutcome {
    let result = async {
        let mut worker = env.actor(actor)?;
        Ok::<_, crate::Error>(worker.run_alarm(&scheduled_time, retry_count).await?)
    }
    .await;
    match result {
        Ok(result) => AlarmOutcome {
            retry: result.outcome != EventOutcome::Ok && result.retry,
            retry_counts_against_limit: result.retry_counts_against_limit,
            outcome: result.outcome,
        },
        Err(e) => {
            tracing::warn!("{}", e.description());
            // The handler's own errors come back as outcomes, so an error here is not one of
            // them, and does not count against the limit.
            AlarmOutcome {
                retry: true,
                retry_counts_against_limit: false,
                outcome: EventOutcome::Exception,
            }
        }
    }
}

/// The task that tells `actor` its alarm at `scheduled_time` was abandoned, then deletes the
/// alarm, or runs the one queued behind it.
async fn abandon_alarm(
    this: Weak<Inner>,
    env: Rc<dyn AlarmEnv>,
    actor: ActorKey,
    scheduled_time: SystemTime,
) {
    // The actor's answer, its own alarm time, is not needed: the scheduler sees every alarm the
    // actor sets.
    let told = async {
        let mut worker = env.actor(&actor)?;
        worker.abandon_alarm(&scheduled_time).await?;
        Ok::<_, crate::Error>(())
    }
    .await;
    if let Err(e) = told {
        tracing::warn!(
            "abandonAlarm notification failed, keeping alarm in scheduler: {}",
            e.description()
        );
        return;
    }

    let Some(inner) = this.upgrade() else {
        return;
    };
    let mut garbage = Garbage::default();
    let delete = {
        let now = inner.env.now();
        let mut alarms = inner.alarms.borrow_mut();
        let Some(alarm) = alarms.get_mut(&actor.id) else {
            return;
        };
        if alarm.status != Status::Finished || alarm.scheduled_time != scheduled_time {
            return;
        }
        if let Some(replacement) = alarm.queued_alarm {
            let replacement_actor = alarm.actor.clone();
            let scheduled = inner.schedule_alarm(now, &replacement_actor, replacement);
            garbage.alarms.push(std::mem::replace(alarm, scheduled));
            false
        } else {
            true
        }
    };
    if delete && let Err(e) = inner.delete_alarm(&actor.id, &mut garbage) {
        tracing::warn!("{}", e.description());
    }
}

/// An alarm's task, run in the environment's task set. Dropping it cancels the task at once,
/// dropping what it awaits, unless the task itself drops it as it runs: then it runs on to its
/// end.
struct Task(Rc<TaskCell>);

struct TaskCell {
    future: RefCell<Option<LocalBoxFuture<'static, ()>>>,
    /// The waker of the spawned task's last poll, so that a cancelled task ends.
    waker: RefCell<Option<Waker>>,
}

impl Task {
    fn spawn(env: &dyn AlarmEnv, future: impl Future<Output = ()> + 'static) -> Self {
        let cell = Rc::new(TaskCell {
            future: RefCell::new(Some(Box::pin(future))),
            waker: RefCell::new(None),
        });
        let spawned = Rc::clone(&cell);
        env.spawn(Box::pin(std::future::poll_fn(move |cx| spawned.poll(cx))));
        Self(cell)
    }
}

impl TaskCell {
    fn poll(&self, cx: &mut Context<'_>) -> Poll<()> {
        let mut future = self.future.borrow_mut();
        let Some(running) = future.as_mut() else {
            return Poll::Ready(());
        };
        if running.as_mut().poll(cx).is_ready() {
            let done = future.take();
            drop(future);
            drop(done);
            return Poll::Ready(());
        }
        drop(future);
        *self.waker.borrow_mut() = Some(cx.waker().clone());
        Poll::Pending
    }
}

impl Drop for Task {
    fn drop(&mut self) {
        // Borrowed while the task runs: it is replacing its own alarm.
        let Ok(mut future) = self.0.future.try_borrow_mut() else {
            return;
        };
        let cancelled = future.take();
        drop(future);
        drop(cancelled);
        let waker = self.0.waker.borrow_mut().take();
        if let Some(waker) = waker {
            waker.wake();
        }
    }
}

#[cfg(test)]
#[path = "alarm_scheduler-test.rs"]
mod tests;
