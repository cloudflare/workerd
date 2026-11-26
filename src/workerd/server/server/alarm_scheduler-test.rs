// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The scheduler's state machine: retries and their backoff, alarms set or deleted while they
//! wait, while their handler runs and while they wait to retry, abandonment, and startup. Each
//! test drives a clock and a timer that only it moves, and actors that follow a script.

use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::collections::VecDeque;
use std::pin::Pin;
use std::rc::Rc;
use std::time::Duration;
use std::time::SystemTime;

use futures::channel::oneshot;
use futures::executor::LocalPool;
use futures::executor::LocalSpawner;
use futures::future::LocalBoxFuture;
use futures::task::LocalSpawnExt;
use kj::http::ConnectResponse;
use kj::http::ConnectSettings;
use kj::http::HeadersRef;
use kj::http::Method;
use kj::http::ServiceResponse;
use kj::io::AsyncInputStream;
use kj::io::AsyncIoStream;
use kj_rs::KjOwn;
use outcome_capnp::EventOutcome;
use worker::AlarmResult;
use worker::CustomEvent;
use worker::CustomEventResult;
use worker::ScheduledResult;

use super::ActorKey;
use super::AlarmEnv;
use super::AlarmScheduler;
use super::AlarmStorage;
use super::RETRY_BACKOFF_MAX;
use super::RETRY_MAX_TRIES;
use super::RETRY_START_SECONDS;
use super::StoredAlarm;
use super::max_jitter_ms;
use crate::Result;

fn succeeded() -> AlarmResult {
    AlarmResult {
        retry: false,
        retry_counts_against_limit: true,
        outcome: EventOutcome::Ok,
        error_description: None,
    }
}

fn failed_counted() -> AlarmResult {
    AlarmResult {
        retry: true,
        retry_counts_against_limit: true,
        outcome: EventOutcome::Exception,
        error_description: None,
    }
}

fn failed_uncounted() -> AlarmResult {
    AlarmResult {
        retry: true,
        retry_counts_against_limit: false,
        outcome: EventOutcome::Exception,
        error_description: None,
    }
}

fn not_retried(outcome: EventOutcome) -> AlarmResult {
    AlarmResult {
        retry: false,
        retry_counts_against_limit: true,
        outcome,
        error_description: None,
    }
}

fn hours(n: u64) -> Duration {
    Duration::from_secs(n * 60 * 60)
}

fn minutes(n: u64) -> Duration {
    Duration::from_secs(n * 60)
}

fn seconds(n: u64) -> Duration {
    Duration::from_secs(n)
}

fn millis(n: u64) -> Duration {
    Duration::from_millis(n)
}

#[derive(Clone, Debug)]
struct AlarmRun {
    actor: ActorKey,
    scheduled_time: SystemTime,
    retry_count: u32,
}

enum Step {
    Finish(AlarmResult),
    Fail,
    /// Finishes when the test sends the result.
    Hang(oneshot::Receiver<AlarmResult>),
}

/// What the test's actors do when the scheduler runs or abandons their alarms. Runs take the
/// next step in order; once the steps run out, every run succeeds.
#[derive(Default)]
struct ScriptedActors {
    steps: RefCell<VecDeque<Step>>,
    runs: RefCell<Vec<AlarmRun>>,
    abandons: RefCell<Vec<SystemTime>>,
    fail_abandon: Cell<bool>,
    /// Holds the next abandonment until the test sends.
    hang_abandon: RefCell<Option<oneshot::Receiver<()>>>,
}

impl ScriptedActors {
    fn then(&self, result: AlarmResult) {
        self.steps.borrow_mut().push_back(Step::Finish(result));
    }

    fn then_fail(&self) {
        self.steps.borrow_mut().push_back(Step::Fail);
    }

    fn then_hang(&self) -> oneshot::Sender<AlarmResult> {
        let (sender, receiver) = oneshot::channel();
        self.steps.borrow_mut().push_back(Step::Hang(receiver));
        sender
    }

    fn runs(&self) -> usize {
        self.runs.borrow().len()
    }

    fn last_run(&self) -> AlarmRun {
        self.runs.borrow().last().expect("no alarm ran").clone()
    }

    fn abandons(&self) -> usize {
        self.abandons.borrow().len()
    }
}

struct ScriptedWorker {
    actors: Rc<ScriptedActors>,
    actor: ActorKey,
}

fn unused() -> cxx::KjException {
    kj::not_implemented!("the alarm scheduler only runs and abandons alarms").into()
}

#[async_trait::async_trait(?Send)]
impl kj::http::Service for ScriptedWorker {
    async fn request<'a>(
        &'a mut self,
        _method: Method,
        _url: &'a [u8],
        _headers: HeadersRef<'a>,
        _request_body: Pin<&'a mut AsyncInputStream>,
        _response: ServiceResponse<'a>,
    ) -> worker::Result<()> {
        Err(unused())
    }

    async fn connect<'a>(
        &'a mut self,
        _host: &'a [u8],
        _headers: HeadersRef<'a>,
        _connection: Pin<&'a mut AsyncIoStream>,
        _response: ConnectResponse<'a>,
        _settings: ConnectSettings<'a>,
    ) -> worker::Result<()> {
        Err(unused())
    }
}

#[async_trait::async_trait(?Send)]
impl worker::Interface for ScriptedWorker {
    async fn run_scheduled(
        &mut self,
        _scheduled_time: &SystemTime,
        _cron: &str,
    ) -> worker::Result<ScheduledResult> {
        Err(unused())
    }

    async fn run_alarm(
        &mut self,
        scheduled_time: &SystemTime,
        retry_count: u32,
    ) -> worker::Result<AlarmResult> {
        self.actors.runs.borrow_mut().push(AlarmRun {
            actor: self.actor.clone(),
            scheduled_time: *scheduled_time,
            retry_count,
        });
        let step = self.actors.steps.borrow_mut().pop_front();
        match step {
            None => Ok(succeeded()),
            Some(Step::Finish(result)) => Ok(result),
            Some(Step::Fail) => Err(kj::failed!("test actor failed").into()),
            Some(Step::Hang(receiver)) => receiver
                .await
                .map_err(|_| kj::failed!("the test dropped a hanging run").into()),
        }
    }

    async fn abandon_alarm(
        &mut self,
        scheduled_time: &SystemTime,
    ) -> worker::Result<Option<SystemTime>> {
        self.actors.abandons.borrow_mut().push(*scheduled_time);
        let hang = self.actors.hang_abandon.borrow_mut().take();
        if let Some(receiver) = hang {
            let _ = receiver.await;
        }
        if self.actors.fail_abandon.get() {
            return Err(kj::failed!("test actor failed to abandon its alarm").into());
        }
        Ok(None)
    }

    async fn custom_event(
        &mut self,
        _event: KjOwn<CustomEvent>,
    ) -> worker::Result<CustomEventResult> {
        Err(unused())
    }
}

/// A timer that only the test moves, from its origin, the Unix epoch. Like kj's `TimerImpl`,
/// it completes a sleep only as it is advanced.
#[derive(Default)]
struct Timer {
    now: Cell<Duration>,
    sleeps: RefCell<Vec<(Duration, oneshot::Sender<()>)>>,
}

impl Timer {
    fn sleep(&self, delay: Duration) -> LocalBoxFuture<'static, ()> {
        let (sender, receiver) = oneshot::channel();
        self.sleeps
            .borrow_mut()
            .push((self.now.get() + delay, sender));
        Box::pin(async move {
            let _ = receiver.await;
        })
    }

    fn advance_to(&self, time: Duration) {
        self.now.set(self.now.get().max(time));
        let now = self.now.get();
        let due: Vec<_> = {
            let mut sleeps = self.sleeps.borrow_mut();
            let (due, waiting) = sleeps.drain(..).partition(|(at, _)| *at <= now);
            *sleeps = waiting;
            due
        };
        for (_, sender) in due {
            let _ = sender.send(());
        }
    }
}

struct TestEnv {
    clock: Cell<SystemTime>,
    timer: Timer,
    spawner: LocalSpawner,
    actors: Rc<ScriptedActors>,
    /// Whether the scheduler may ask for an actor at all.
    actors_allowed: Cell<bool>,
}

impl AlarmEnv for TestEnv {
    fn now(&self) -> SystemTime {
        self.clock.get()
    }

    fn sleep(&self, delay: Duration) -> LocalBoxFuture<'static, ()> {
        self.timer.sleep(delay)
    }

    fn spawn(&self, task: LocalBoxFuture<'static, ()>) {
        self.spawner
            .spawn_local(task)
            .expect("the test's pool is gone");
    }

    fn actor(&self, actor: &ActorKey) -> Result<Box<dyn worker::Interface>> {
        assert!(
            self.actors_allowed.get(),
            "the scheduler asked for an actor"
        );
        Ok(Box::new(ScriptedWorker {
            actors: Rc::clone(&self.actors),
            actor: actor.clone(),
        }))
    }
}

/// The storage that `AlarmStore` (`alarm_store.rs`) is: one row per actor, keeping a stored name
/// when an alarm is set without one.
#[derive(Default)]
struct MemoryStorage {
    rows: RefCell<HashMap<String, (SystemTime, Option<String>)>>,
}

struct SharedStorage(Rc<MemoryStorage>);

impl AlarmStorage for SharedStorage {
    fn load(&self) -> Result<Vec<StoredAlarm>> {
        Ok(self
            .0
            .rows
            .borrow()
            .iter()
            .map(|(id, (scheduled_time, name))| StoredAlarm {
                actor: ActorKey {
                    id: id.clone(),
                    name: name.clone(),
                },
                scheduled_time: *scheduled_time,
            })
            .collect())
    }

    fn set(&self, actor: &ActorKey, scheduled_time: SystemTime) -> Result<bool> {
        let mut rows = self.0.rows.borrow_mut();
        let stored_name = rows.get(&actor.id).and_then(|(_, name)| name.clone());
        let name = actor.name.clone().or(stored_name);
        rows.insert(actor.id.clone(), (scheduled_time, name));
        Ok(true)
    }

    fn remove(&self, id: &str) -> Result<bool> {
        Ok(self.0.rows.borrow_mut().remove(id).is_some())
    }

    fn clear(&self) -> Result<()> {
        self.0.rows.borrow_mut().clear();
        Ok(())
    }
}

fn since_epoch(time: SystemTime) -> Duration {
    time.duration_since(SystemTime::UNIX_EPOCH)
        .expect("test times are after the epoch")
}

/// A scheduler over in-memory storage, with a clock and a timer that only the test moves. The
/// timer reads the same as the clock unless a test moves them apart.
struct Fixture {
    pool: LocalPool,
    env: Rc<TestEnv>,
    storage: Rc<MemoryStorage>,
    scheduler: Option<AlarmScheduler>,
    actor: ActorKey,
    t0: SystemTime,
}

impl Fixture {
    fn new() -> Self {
        let pool = LocalPool::new();
        let env = Rc::new(TestEnv {
            clock: Cell::new(SystemTime::UNIX_EPOCH),
            timer: Timer::default(),
            spawner: pool.spawner(),
            actors: Rc::new(ScriptedActors::default()),
            actors_allowed: Cell::new(true),
        });
        let mut fixture = Self {
            pool,
            env,
            storage: Rc::new(MemoryStorage::default()),
            scheduler: None,
            actor: ActorKey {
                id: "actor".to_owned(),
                name: None,
            },
            t0: SystemTime::UNIX_EPOCH + hours(1),
        };
        fixture.open();
        fixture
    }

    /// Replaces the scheduler with a new one that loads its alarms from the storage.
    fn open(&mut self) -> &AlarmScheduler {
        self.close();
        let env: Rc<dyn AlarmEnv> = Rc::clone(&self.env) as Rc<dyn AlarmEnv>;
        let storage = Box::new(SharedStorage(Rc::clone(&self.storage)));
        self.scheduler
            .insert(AlarmScheduler::new(env, storage).expect("loading alarms failed"))
    }

    fn close(&mut self) {
        self.scheduler = None;
        self.pool.run_until_stalled();
    }

    fn scheduler(&self) -> &AlarmScheduler {
        self.scheduler.as_ref().expect("the scheduler is closed")
    }

    fn actors(&self) -> &ScriptedActors {
        &self.env.actors
    }

    fn get_alarm(&self) -> Option<SystemTime> {
        self.scheduler().get_alarm(&self.actor.id)
    }

    fn set_alarm(&self, time: SystemTime) -> bool {
        self.scheduler()
            .set_alarm(&self.actor, time)
            .expect("set_alarm failed")
    }

    fn delete_alarm(&self) -> bool {
        self.scheduler()
            .delete_alarm(&self.actor.id)
            .expect("delete_alarm failed")
    }

    /// Runs the tasks, then moves the timer to `time` (since its origin) and runs whatever that
    /// makes ready. The tasks run first, as a timer counts a task's delay from when it starts.
    /// A timer completes a sleep only as it is advanced, so this repeats for sleeps that tasks
    /// start for a time already reached.
    fn advance_timer_to(&mut self, time: Duration) {
        for _ in 0..10 {
            self.pool.run_until_stalled();
            self.env.timer.advance_to(time);
            self.pool.run_until_stalled();
        }
    }

    /// Moves the clock and the timer to `time`.
    fn advance_to(&mut self, time: SystemTime) {
        self.env.clock.set(time);
        self.advance_timer_to(since_epoch(time));
    }

    /// Expects the next run `delay` after `from`, within the retry jitter, and returns the time
    /// the test moved to, which the next backoff starts from.
    fn expect_retry(&mut self, from: SystemTime, delay: Duration) -> SystemTime {
        let before = self.actors().runs();
        self.advance_to(from + delay - millis(1));
        assert_eq!(
            self.actors().runs(),
            before,
            "retried before its backoff of {delay:?}"
        );
        let latest = from + delay + millis(max_jitter_ms(delay));
        self.advance_to(latest);
        assert_eq!(
            self.actors().runs(),
            before + 1,
            "not retried within its backoff of {delay:?}"
        );
        latest
    }
}

fn backoff(shift: u32) -> Duration {
    seconds(RETRY_START_SECONDS << shift)
}

#[test]
fn restores_the_stored_name_when_an_alarm_runs_after_a_restart() {
    let mut f = Fixture::new();
    let named = ActorKey {
        id: "named-actor".to_owned(),
        name: Some("my-name".to_owned()),
    };
    f.scheduler().set_alarm(&named, f.t0).unwrap();
    f.close();

    f.open();
    let t0 = f.t0;
    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.actors().last_run().actor, named);
}

#[test]
fn passes_the_name_to_the_storage_and_keeps_it_across_a_nameless_set() {
    let mut f = Fixture::new();
    f.env.actors_allowed.set(false);
    let named = ActorKey {
        id: "named-actor".to_owned(),
        name: Some("my-name".to_owned()),
    };
    let nameless = ActorKey {
        id: "named-actor".to_owned(),
        name: None,
    };
    f.scheduler().set_alarm(&named, f.t0).unwrap();
    f.scheduler().set_alarm(&nameless, f.t0 + hours(1)).unwrap();
    assert_eq!(
        f.storage.rows.borrow().get("named-actor"),
        Some(&(f.t0 + hours(1), Some("my-name".to_owned())))
    );
    assert_eq!(
        f.open().get_alarm("named-actor"),
        Some(SystemTime::UNIX_EPOCH + hours(2))
    );
}

#[test]
fn abandons_an_alarm_when_the_run_was_aborted() {
    let mut f = Fixture::new();
    f.actors().then(not_retried(EventOutcome::Aborted));
    f.set_alarm(f.t0);
    let t0 = f.t0;
    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(*f.actors().abandons.borrow(), vec![t0]);
    assert_eq!(f.get_alarm(), None);
    assert_eq!(f.open().get_alarm("actor"), None);
}

#[test]
fn keeps_an_alarm_queued_while_abandonment_is_pending() {
    let mut f = Fixture::new();
    let replacement = f.t0 + hours(1);
    f.actors().then(not_retried(EventOutcome::Aborted));
    let (abandoned, receiver) = oneshot::channel();
    *f.actors().hang_abandon.borrow_mut() = Some(receiver);
    f.set_alarm(f.t0);
    let t0 = f.t0;
    f.advance_to(t0);
    assert_eq!(f.actors().abandons(), 1);

    f.set_alarm(replacement);
    abandoned.send(()).unwrap();
    f.advance_to(t0);
    assert_eq!(f.get_alarm(), Some(replacement));

    f.env.actors_allowed.set(false);
    assert_eq!(f.open().get_alarm("actor"), Some(replacement));
}

#[test]
fn reports_whether_set_and_delete_changed_storage() {
    let f = Fixture::new();
    assert!(!f.delete_alarm());
    assert!(f.set_alarm(f.t0));
    assert!(f.set_alarm(f.t0 + hours(1)));
    assert!(f.delete_alarm());
    assert!(!f.delete_alarm());
    assert_eq!(f.get_alarm(), None);
}

#[test]
fn runs_a_successful_alarm_once_and_deletes_it() {
    let mut f = Fixture::new();
    let t0 = f.t0;
    f.set_alarm(t0);

    f.advance_to(t0 - millis(1));
    assert_eq!(f.actors().runs(), 0);

    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 1);
    let run = f.actors().last_run();
    assert_eq!(run.actor.id, "actor");
    assert_eq!(run.scheduled_time, t0);
    assert_eq!(run.retry_count, 0);
    assert_eq!(f.get_alarm(), None);

    f.advance_to(t0 + hours(24));
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.open().get_alarm("actor"), None);
}

#[test]
fn runs_an_alarm_that_came_due_while_it_was_not_running_at_startup() {
    let mut f = Fixture::new();
    let t0 = f.t0;
    f.set_alarm(t0);
    f.close();

    // An alarm waits on the timer for its time less the clock's, so one already due runs
    // without the timer moving: the timer stays at its origin, an hour before the alarm.
    f.env.clock.set(t0 + hours(1));
    f.open();
    f.advance_timer_to(Duration::ZERO);
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.actors().last_run().scheduled_time, t0);
    assert_eq!(f.get_alarm(), None);
}

#[test]
fn waits_for_the_clock_when_the_timer_fires_before_the_scheduled_time() {
    let mut f = Fixture::new();
    let t0 = f.t0;
    f.set_alarm(t0);

    // The timer reaches the scheduled time while the clock is 5 seconds behind it: the
    // scheduler waits another 5 seconds on the timer.
    f.env.clock.set(t0 - seconds(5));
    f.advance_timer_to(since_epoch(t0));
    assert_eq!(f.actors().runs(), 0);

    f.env.clock.set(t0);
    f.advance_timer_to(since_epoch(t0) + millis(4_999));
    assert_eq!(f.actors().runs(), 0);

    f.advance_timer_to(since_epoch(t0) + seconds(5));
    assert_eq!(f.actors().runs(), 1);
}

#[test]
fn reschedules_a_waiting_alarm_when_it_is_set_again() {
    let mut f = Fixture::new();
    let t0 = f.t0;

    // Later.
    f.set_alarm(t0);
    f.set_alarm(t0 + hours(1));
    assert_eq!(f.get_alarm(), Some(t0 + hours(1)));
    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 0);
    f.advance_to(t0 + hours(1));
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.actors().last_run().scheduled_time, t0 + hours(1));

    // Earlier.
    let t1 = t0 + hours(2);
    f.set_alarm(t1 + hours(1));
    f.set_alarm(t1);
    f.advance_to(t1);
    assert_eq!(f.actors().runs(), 2);
    assert_eq!(f.actors().last_run().scheduled_time, t1);
    f.advance_to(t1 + hours(2));
    assert_eq!(f.actors().runs(), 2);
}

#[test]
fn never_runs_a_waiting_alarm_that_was_deleted() {
    let mut f = Fixture::new();
    let t0 = f.t0;
    f.set_alarm(t0);
    assert!(f.delete_alarm());
    assert_eq!(f.get_alarm(), None);
    f.advance_to(t0 + hours(24));
    assert_eq!(f.actors().runs(), 0);
    assert_eq!(f.open().get_alarm("actor"), None);
}

#[test]
fn retries_counted_failures_with_exponential_backoff_then_abandons() {
    let mut f = Fixture::new();
    for _ in 0..=RETRY_MAX_TRIES {
        f.actors().then(failed_counted());
    }
    let t0 = f.t0;
    f.set_alarm(t0);

    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.actors().last_run().retry_count, 0);

    // Retries 2, 4, 8, 16, 32 and 64 seconds apart, each told how many counted retries came
    // before.
    let mut now = t0;
    for i in 0..RETRY_MAX_TRIES {
        now = f.expect_retry(now, backoff(i));
        assert_eq!(f.actors().last_run().scheduled_time, t0);
        assert_eq!(f.actors().last_run().retry_count, i + 1);
    }

    // The seventh failure exhausts the retries: the actor is told, and the alarm is gone.
    assert_eq!(*f.actors().abandons.borrow(), vec![t0]);
    assert_eq!(f.get_alarm(), None);
    f.advance_to(now + hours(24));
    assert_eq!(f.actors().runs(), RETRY_MAX_TRIES as usize + 1);
    assert_eq!(f.open().get_alarm("actor"), None);
}

/// Once the counted retries are used up, any failure that asks for a retry abandons the alarm,
/// even one that would not count against the limit.
fn expect_abandoned_at_limit_by(add_last_failure: impl FnOnce(&ScriptedActors)) {
    let mut f = Fixture::new();
    for _ in 0..RETRY_MAX_TRIES {
        f.actors().then(failed_counted());
    }
    add_last_failure(f.actors());
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);

    let mut now = t0;
    for i in 0..RETRY_MAX_TRIES {
        now = f.expect_retry(now, backoff(i));
        assert_eq!(f.actors().last_run().retry_count, i + 1);
    }

    assert_eq!(f.actors().abandons(), 1);
    assert_eq!(f.get_alarm(), None);
    f.advance_to(now + hours(24));
    assert_eq!(f.actors().runs(), RETRY_MAX_TRIES as usize + 1);
    assert_eq!(f.open().get_alarm("actor"), None);
}

#[test]
fn abandons_an_uncounted_failure_once_the_counted_retries_are_used_up() {
    expect_abandoned_at_limit_by(|actors| actors.then(failed_uncounted()));
}

#[test]
fn abandons_a_run_that_failed_once_the_counted_retries_are_used_up() {
    expect_abandoned_at_limit_by(ScriptedActors::then_fail);
}

#[test]
fn retries_uncounted_failures_without_limit_backing_off_up_to_1024_seconds() {
    const RETRIES: u32 = 12;
    let mut f = Fixture::new();
    for _ in 0..RETRIES {
        f.actors().then(failed_uncounted());
    }
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);

    let mut now = t0;
    for i in 0..RETRIES {
        now = f.expect_retry(now, backoff(i.min(RETRY_BACKOFF_MAX)));
        assert_eq!(f.actors().last_run().retry_count, 0);
    }

    // The last run succeeded.
    assert_eq!(f.actors().abandons(), 0);
    assert_eq!(f.get_alarm(), None);
}

#[test]
fn retries_an_alarm_whose_run_failed_without_counting_it() {
    let mut f = Fixture::new();
    f.actors().then_fail();
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 1);

    f.expect_retry(t0, backoff(0));
    assert_eq!(f.actors().last_run().retry_count, 0);
    assert_eq!(f.get_alarm(), None);
}

#[test]
fn resets_the_backoff_when_a_counted_failure_follows_uncounted_ones() {
    let mut f = Fixture::new();
    f.actors().then(failed_uncounted());
    f.actors().then(failed_uncounted());
    f.actors().then(failed_counted());
    f.actors().then(failed_counted());
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);

    let mut now = f.expect_retry(t0, seconds(2));
    assert_eq!(f.actors().last_run().retry_count, 0);
    now = f.expect_retry(now, seconds(4));
    assert_eq!(f.actors().last_run().retry_count, 0);
    // The first counted failure starts the backoff over.
    now = f.expect_retry(now, seconds(2));
    assert_eq!(f.actors().last_run().retry_count, 1);
    f.expect_retry(now, seconds(4));
    assert_eq!(f.actors().last_run().retry_count, 2);

    assert_eq!(f.get_alarm(), None);
}

#[test]
fn resets_the_backoff_on_every_switch_from_uncounted_to_counted_failures() {
    let mut f = Fixture::new();
    f.actors().then(failed_counted());
    f.actors().then(failed_uncounted());
    f.actors().then(failed_counted());
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);

    let mut now = f.expect_retry(t0, seconds(2));
    assert_eq!(f.actors().last_run().retry_count, 1);
    // An uncounted failure after a counted one continues the backoff.
    now = f.expect_retry(now, seconds(4));
    assert_eq!(f.actors().last_run().retry_count, 1);
    // The next counted failure starts it over.
    f.expect_retry(now, seconds(2));
    assert_eq!(f.actors().last_run().retry_count, 2);

    assert_eq!(f.get_alarm(), None);
}

#[test]
fn deletes_without_abandoning_a_failed_run_that_asks_for_no_retry() {
    let mut f = Fixture::new();
    f.actors().then(not_retried(EventOutcome::Exception));
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.actors().abandons(), 0);
    assert_eq!(f.get_alarm(), None);

    f.advance_to(t0 + hours(24));
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.open().get_alarm("actor"), None);
}

#[test]
fn runs_an_alarm_queued_for_the_same_time_again_after_the_handler() {
    let mut f = Fixture::new();
    let run = f.actors().then_hang();
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    f.set_alarm(t0);

    run.send(succeeded()).unwrap();
    f.advance_to(t0);
    let runs = f.actors().runs.borrow().clone();
    assert_eq!(runs.len(), 2);
    assert_eq!(runs[0].scheduled_time, t0);
    assert_eq!(runs[1].scheduled_time, t0);
    assert_eq!(runs[1].retry_count, 0);
    assert_eq!(f.get_alarm(), None);
}

#[test]
fn does_not_retry_a_run_that_asks_for_a_retry_but_succeeded() {
    let mut f = Fixture::new();
    f.actors().then(AlarmResult {
        retry: true,
        retry_counts_against_limit: true,
        outcome: EventOutcome::Ok,
        error_description: None,
    });
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    assert_eq!(f.get_alarm(), None);
    f.advance_to(t0 + hours(24));
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.open().get_alarm("actor"), None);
}

#[test]
fn queues_an_alarm_set_while_the_handler_runs_even_for_the_same_time() {
    let mut f = Fixture::new();
    let run = f.actors().then_hang();
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 1);

    // A running alarm is not reported; only one queued behind it is, and a later set replaces
    // it.
    assert_eq!(f.get_alarm(), None);
    f.set_alarm(t0);
    assert_eq!(f.get_alarm(), Some(t0));
    f.set_alarm(t0 + hours(1));
    assert_eq!(f.get_alarm(), Some(t0 + hours(1)));

    run.send(succeeded()).unwrap();
    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.get_alarm(), Some(t0 + hours(1)));

    f.advance_to(t0 + hours(1));
    assert_eq!(f.actors().runs(), 2);
    assert_eq!(f.actors().last_run().scheduled_time, t0 + hours(1));
    assert_eq!(f.get_alarm(), None);
}

#[test]
fn runs_a_queued_alarm_instead_of_retrying_the_failed_one() {
    let mut f = Fixture::new();
    let run = f.actors().then_hang();
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    f.set_alarm(t0 + minutes(1));

    run.send(failed_counted()).unwrap();
    f.advance_to(t0 + seconds(3));
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.get_alarm(), Some(t0 + minutes(1)));

    f.advance_to(t0 + minutes(1));
    assert_eq!(f.actors().runs(), 2);
    assert_eq!(f.actors().last_run().scheduled_time, t0 + minutes(1));
    assert_eq!(f.actors().last_run().retry_count, 0);
}

#[test]
fn runs_a_queued_alarm_for_the_same_time_instead_of_retrying_the_failed_one() {
    let mut f = Fixture::new();
    let run = f.actors().then_hang();
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    f.set_alarm(t0);

    run.send(failed_counted()).unwrap();
    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 2);
    assert_eq!(f.actors().last_run().scheduled_time, t0);
    assert_eq!(f.actors().last_run().retry_count, 0);
    assert_eq!(f.get_alarm(), None);
}

#[test]
fn deletes_only_the_queued_alarm_while_the_handler_runs() {
    let mut f = Fixture::new();
    let run = f.actors().then_hang();
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    f.set_alarm(t0 + hours(1));

    assert!(f.delete_alarm());
    assert_eq!(f.get_alarm(), None);

    run.send(succeeded()).unwrap();
    f.advance_to(t0 + hours(2));
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.open().get_alarm("actor"), None);
}

#[test]
fn still_retries_an_alarm_deleted_while_its_handler_ran() {
    let mut f = Fixture::new();
    let run = f.actors().then_hang();
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);

    // The running alarm stays in memory, though its row is gone.
    assert!(f.delete_alarm());
    assert_eq!(f.get_alarm(), None);

    run.send(failed_counted()).unwrap();
    f.advance_to(t0);
    // Waiting to retry, the alarm is reported again.
    assert_eq!(f.get_alarm(), Some(t0));

    f.expect_retry(t0, seconds(2));
    assert_eq!(f.actors().last_run().scheduled_time, t0);
    assert_eq!(f.actors().last_run().retry_count, 1);
    assert_eq!(f.get_alarm(), None);
    assert_eq!(f.open().get_alarm("actor"), None);
}

#[test]
fn cancels_a_retry_when_the_alarm_is_deleted_during_the_backoff() {
    let mut f = Fixture::new();
    f.actors().then(failed_counted());
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    assert_eq!(f.get_alarm(), Some(t0));

    assert!(f.delete_alarm());
    assert_eq!(f.get_alarm(), None);
    f.advance_to(t0 + hours(1));
    assert_eq!(f.actors().runs(), 1);
}

#[test]
fn queues_an_alarm_set_during_the_backoff_behind_the_retry() {
    let mut f = Fixture::new();
    f.actors().then(failed_counted());
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);

    // During the backoff the retrying alarm is reported, not the one queued behind it,
    // although the queued time is what storage holds.
    let queued = t0 + minutes(10);
    f.set_alarm(queued);
    assert_eq!(f.get_alarm(), Some(t0));

    f.expect_retry(t0, seconds(2));
    assert_eq!(f.actors().last_run().scheduled_time, t0);
    assert_eq!(f.actors().last_run().retry_count, 1);
    assert_eq!(f.get_alarm(), Some(queued));

    f.advance_to(queued - millis(1));
    assert_eq!(f.actors().runs(), 2);
    f.advance_to(queued);
    assert_eq!(f.actors().runs(), 3);
    assert_eq!(f.actors().last_run().scheduled_time, queued);
    assert_eq!(f.actors().last_run().retry_count, 0);
    assert_eq!(f.get_alarm(), None);
}

// Deleting an alarm during its backoff, with another queued behind it, deletes the stored row
// and cancels the retry, but schedules the queued alarm in memory only.
#[test]
fn runs_the_queued_alarm_when_one_is_deleted_during_its_backoff() {
    let mut f = Fixture::new();
    f.actors().then(failed_counted());
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    let queued = t0 + minutes(10);
    f.set_alarm(queued);

    assert!(f.delete_alarm());
    assert_eq!(f.get_alarm(), Some(queued));

    // No retry.
    f.advance_to(t0 + seconds(3));
    assert_eq!(f.actors().runs(), 1);

    f.advance_to(queued - millis(1));
    assert_eq!(f.actors().runs(), 1);
    f.advance_to(queued);
    assert_eq!(f.actors().runs(), 2);
    assert_eq!(f.actors().last_run().scheduled_time, queued);
    assert_eq!(f.actors().last_run().retry_count, 0);
    assert_eq!(f.get_alarm(), None);
}

#[test]
fn does_not_store_the_queued_alarm_when_one_is_deleted_during_its_backoff() {
    let mut f = Fixture::new();
    f.actors().then(failed_counted());
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    let queued = t0 + minutes(10);
    f.set_alarm(queued);
    assert!(f.delete_alarm());
    assert_eq!(f.get_alarm(), Some(queued));

    // A restart before the queued alarm's time loses it.
    assert_eq!(f.open().get_alarm("actor"), None);
    f.advance_to(queued + hours(1));
    assert_eq!(f.actors().runs(), 1);
}

#[test]
fn keeps_an_alarm_whose_abandonment_failed_but_runs_it_only_after_restart() {
    let mut f = Fixture::new();
    f.actors().then(not_retried(EventOutcome::Aborted));
    f.actors().fail_abandon.set(true);
    let t0 = f.t0;
    f.set_alarm(t0);
    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.actors().abandons(), 1);
    assert_eq!(f.get_alarm(), Some(t0));

    f.advance_to(t0 + hours(24));
    assert_eq!(f.actors().runs(), 1);
    assert_eq!(f.actors().abandons(), 1);

    f.open();
    f.advance_to(t0 + hours(24));
    assert_eq!(f.actors().runs(), 2);
    assert_eq!(f.actors().last_run().scheduled_time, t0);
    assert_eq!(f.get_alarm(), None);
}

#[test]
fn delete_all_cancels_running_and_waiting_alarms_and_empties_storage() {
    let mut f = Fixture::new();
    let run = f.actors().then_hang();
    let other = ActorKey {
        id: "other".to_owned(),
        name: None,
    };
    let t0 = f.t0;
    f.set_alarm(t0);
    f.scheduler().set_alarm(&other, t0 + hours(1)).unwrap();
    f.advance_to(t0);
    assert_eq!(f.actors().runs(), 1);
    assert!(!run.is_canceled());

    f.scheduler().delete_all().unwrap();
    // The running handler's future is dropped at once.
    assert!(run.is_canceled());
    assert_eq!(f.get_alarm(), None);
    assert_eq!(f.scheduler().get_alarm("other"), None);

    f.advance_to(t0 + hours(24));
    assert_eq!(f.actors().runs(), 1);
    let reopened = f.open();
    assert_eq!(reopened.get_alarm("actor"), None);
    assert_eq!(reopened.get_alarm("other"), None);
}
