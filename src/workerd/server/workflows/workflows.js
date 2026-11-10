// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Workflows: the engine a Worker's `workflowsEngine` names, and the binding
// its Workflows are reached through, `cloudflare-internal:workflows-api` over
// a service binding (src/cloudflare/internal/workflows-api.ts).
//
// The config that runs this module (`bundled = "/workerd/workflows.js"`) is
// an interface: cli/wrangler.rs and hand-written configs write it. Anything
// added to it later must be optional. It is:
//
//   - the app Worker's `workflowsEngine`: `actorClass` this service's
//     `Engine`, and each Workflow's `bindingService` this service's
//     `WorkflowBinding`, with props `{"workflow": "<name>"}`;
//   - this service's `durableObjectStorage`, a local disk: workerd keeps each
//     Workflow's instances there, in `miniflare-workflows-<name>`;
//   - these bindings:
//
//   <name>     for each Workflow, its instances: `durableObjectNamespace =
//              (className = "miniflare-workflows-<name>", serviceName =
//              "<app>")`
//   TEST_CLOCK JSON `true` for tests (optional)
//
// An instance is one Durable Object, named by its id. Its storage holds the
// instance (`meta`), one record per step it reached (`s:<type>:<n>:<name>`,
// the n-th of that name), the events sent to it not yet taken (`e:<seq>`),
// and its history for `subscribe()` (`l:<id>`). `run()` runs once per
// instance in memory: a step that waits (a sleep, a retry's delay, an event)
// waits there, and the instance's alarm, set for the earliest wait, ends it.
// After a crash, or a restart of the instance, `run()` runs again from the
// start: a step that completed returns its result without running again, a
// wait keeps its deadline. While user code runs, the alarm is a heartbeat
// (`HEARTBEAT_MS`), so a crash runs it again soon after. A finished instance
// is deleted after its retention.
//
// With `TEST_CLOCK`, POST /clock?advance=<ms> moves this process's clock; an
// instance acts on it at its next call or alarm.

import {
  DurableObject,
  RpcTarget,
  WorkerEntrypoint,
  env,
} from 'cloudflare:workers';

const HEARTBEAT_MS = 5_000;
const RETENTION_MS = 30 * 24 * 60 * 60 * 1000;
const MAX_BATCH = 100;
const STEP_LIMIT = 10_000;
const DEFAULTS = {
  retries: { limit: 5, delay: 10_000, backoff: 'exponential' },
  timeout: '10 minutes',
};
const ID = /^[a-zA-Z0-9_][a-zA-Z0-9-_]*$/;
const ADDRESS = /^[a-zA-Z0-9, */#_-]{1,271}$/;
const DAY = 24 * 60 * 60 * 1000;
const UNITS = {
  millisecond: 1,
  second: 1000,
  minute: 60 * 1000,
  hour: 60 * 60 * 1000,
  day: DAY,
  week: 7 * DAY,
  month: 30 * DAY,
  year: 365 * DAY,
};
const TERMINAL = new Set(['complete', 'errored', 'terminated']);
// The statuses a `run()` may take steps in.
const LIVE = new Set([
  'queued',
  'running',
  'paused',
  'waitingForPause',
  'waiting',
]);
const EVENTS = {
  queued: 'workflow_queued',
  rollingBack: 'rollback_started',
  running: 'workflow_running',
  paused: 'workflow_paused',
  waitingForPause: 'workflow_waiting_for_pause',
  waiting: 'workflow_waiting',
  complete: 'workflow_completed',
  errored: 'workflow_errored',
  terminated: 'workflow_terminated',
};
const FINAL = new Set([
  'workflow_completed',
  'workflow_errored',
  'workflow_terminated',
]);
const TIMEOUT = Symbol('timeout');

let skew = 0;
const clock = () => Date.now() + skew;
const pad = (n) => String(n).padStart(12, '0');

const error = (name, message) => Object.assign(new Error(message), { name });
const workflowError = (code, message) =>
  error('WorkflowError', `(${code}) ${message}`);
const fatalError = (message) => error('WorkflowFatalError', message);
// What a step of a `run()` that may no longer take steps throws.
const aborted = () =>
  error('WorkflowAbortedError', 'The instance has finished or restarted');
const notFound = () =>
  workflowError('instance.not_found', 'Instance does not exist');
const plain = (e) => ({
  name: e?.name ?? 'Error',
  message: e?.message ?? String(e),
});
// A NonRetryableError keeps its name across RPC only in its message.
const nonRetryable = (e) =>
  e?.name === 'NonRetryableError' ||
  String(e?.message).startsWith('NonRetryableError');

// Milliseconds: a number, or "<n> <unit>[s]".
function ms(d) {
  if (typeof d === 'number') return d;
  const m = /^\s*(\d+(?:\.\d+)?)\s*([a-z]+?)s?\s*$/.exec(String(d));
  const unit = m && UNITS[m[2]];
  if (!unit) throw new TypeError(`Invalid duration "${d}"`);
  return Number(m[1]) * unit;
}

// The steps `run()` takes, each a call into the engine instance that made it.
class Step extends RpcTarget {
  #step;
  constructor(step) {
    super();
    this.#step = step;
  }
  do(...args) {
    return this.#step.do(...args);
  }
  sleep(name, duration) {
    return this.#step.sleep(name, duration);
  }
  sleepUntil(name, timestamp) {
    return this.#step.sleep(name, undefined, timestamp);
  }
  waitForEvent(name, options) {
    return this.#step.waitForEvent(name, options);
  }
}

class Subscription extends RpcTarget {
  #next;
  #dispose;
  constructor(next, dispose) {
    super();
    this.#next = next;
    this.#dispose = dispose;
  }
  next() {
    return this.#next();
  }
  [Symbol.dispose]() {
    this.#dispose();
  }
}

// One instance of the Workflow `ctx.props.workflowName`.
export class Engine extends DurableObject {
  #kv = this.ctx.storage.kv;
  // The generation whose `run()` is in memory, else null.
  #drive = null;
  // Step callbacks of that `run()` executing.
  #busy = 0;
  // In-memory waits: `{at, type, wake}`, woken with TIMEOUT at `at`, or by an
  // event of `type`.
  #waiters = new Set();
  // While paused, what steps wait on to go on.
  #gate = null;
  #counts = new Map();
  #rollbacks = [];
  #steps = 0;
  #watchers = new Set();
  #alarm = null;

  get #meta() {
    return this.#kv.get('meta');
  }

  #live(generation) {
    const m = this.#meta;
    return m?.generation === generation && LIVE.has(m.status);
  }

  #log(type, fields = {}) {
    const id = (this.#kv.get('logs') ?? 0) + 1;
    this.#kv.put('logs', id);
    const instanceId = this.ctx.id.name;
    const event = { type, instanceId, eventId: id, timestamp: clock() };
    this.#kv.put(`l:${pad(id)}`, { ...event, ...fields });
    this.#notify();
  }

  #notify() {
    const watchers = [...this.#watchers];
    this.#watchers.clear();
    for (const wake of watchers) wake();
  }

  #set(status, fields = {}) {
    this.#kv.put('meta', { ...this.#meta, ...fields, status });
    const { output, error } = fields;
    const extra =
      status === 'complete'
        ? { output }
        : status === 'errored'
          ? { error }
          : {};
    this.#log(EVENTS[status], extra);
  }

  #finish(status, fields = {}) {
    const { retention } = this.#meta;
    const keep = status === 'complete' ? retention.success : retention.error;
    this.#set(status, { ...fields, expires: clock() + keep });
    this.#release();
    this.#arm();
  }

  #open() {
    this.#gate?.open();
    this.#gate = null;
  }

  // Lets every waiting step go on, to find it may not.
  #release() {
    this.#open();
    const waiters = [...this.#waiters];
    this.#waiters.clear();
    for (const w of waiters) w.wake();
  }

  // Whether the step may go on: false once the instance has finished or
  // restarted; while it is paused, once it resumes.
  async #enter(generation) {
    for (;;) {
      if (!this.#live(generation)) return false;
      const { status } = this.#meta;
      if (status !== 'paused' && status !== 'waitingForPause') return true;
      if (!this.#gate) {
        let open;
        const promise = new Promise((wake) => (open = wake));
        this.#gate = { promise, open };
      }
      await this.#gate.promise;
    }
  }

  // Waits until `at`, or for an event of `type`, in memory.
  #wait(at, type) {
    let waiter;
    const p = new Promise((wake) => (waiter = { at, type, wake }));
    this.#waiters.add(waiter);
    p.cancel = () => this.#waiters.delete(waiter);
    this.#settle();
    this.#arm();
    return p;
  }

  // Moves between running, waiting and pausing as steps start and wait.
  #settle() {
    const { status } = this.#meta ?? {};
    const idle = this.#busy === 0;
    if (status === 'waitingForPause' && idle) {
      this.#set('paused', { pausedAt: clock() });
    } else if (status === 'running' && idle && this.#waiters.size > 0) {
      this.#set('waiting');
    } else if (status === 'waiting' && (!idle || this.#waiters.size === 0)) {
      this.#set('running');
    }
  }

  // Ends due waits, runs what should run and sets the alarm. Every call and
  // alarm starts here.
  #wake() {
    const m = this.#meta;
    if (!m) return;
    const now = clock();
    if (TERMINAL.has(m.status)) {
      if (m.expires <= now) return this.#destroy();
      return this.#arm();
    }
    // What the process was doing when it went away.
    if (this.#drive !== m.generation) {
      if (m.status === 'waitingForPause')
        this.#set('paused', { pausedAt: now });
      if (m.status === 'rollingBack') {
        this.#log('rollback_errored');
        return this.#finish(m.after, { error: m.error });
      }
    }
    if (this.#meta.status !== 'paused') {
      for (const w of this.#waiters) {
        if (w.at > now) continue;
        this.#waiters.delete(w);
        w.wake(TIMEOUT);
      }
      if (this.#drive !== m.generation) this.#run(m);
    }
    this.#arm();
  }

  // The alarm: at the earliest wait, a heartbeat while user code runs, now
  // when `run()` should be in memory and is not, and a finished instance's
  // expiry. None while paused.
  #arm() {
    const m = this.#meta;
    let next = Infinity;
    if (m && TERMINAL.has(m.status)) {
      next = m.expires;
    } else if (m && m.status !== 'paused') {
      if (this.#drive !== m.generation) next = clock();
      for (const w of this.#waiters) next = Math.min(next, w.at);
      if (m.status !== 'waiting') next = Math.min(next, clock() + HEARTBEAT_MS);
    }
    // A stale alarm costs one wake.
    if (next === Infinity) return;
    const now = clock();
    if (this.#alarm === null || next < this.#alarm || this.#alarm <= now) {
      this.ctx.storage.setAlarm(next - skew);
      this.#alarm = next;
    }
  }

  async alarm() {
    this.#alarm = null;
    this.#wake();
  }

  // Runs `run()` for the instance's generation, from the start.
  #run(m) {
    const generation = (this.#drive = m.generation);
    this.#busy = 0;
    this.#counts = new Map();
    this.#rollbacks = [];
    this.#steps = 0;
    if (!m.started) {
      this.#kv.put('meta', { ...m, started: true });
      this.#log('workflow_started', { params: m.params });
    }
    if (m.status === 'queued') this.#set('running');
    const event = {
      payload: m.params,
      timestamp: new Date(m.created),
      instanceId: m.id,
      workflowName: this.ctx.props.workflowName,
    };
    const step = new Step({
      do: (...args) => this.#do(generation, ...args),
      sleep: (name, duration, until) =>
        this.#sleep(generation, name, duration, until),
      waitForEvent: (name, options) =>
        this.#waitForEvent(generation, name, options),
    });
    const done = (async () => {
      let output, failure;
      try {
        output = await this.ctx.props.workflowClass.run(event, step);
      } catch (e) {
        failure = e;
      }
      if (!this.#live(generation)) return;
      const { status } = this.#meta;
      // A pause asked for before `run()` returned wins: it runs again on
      // resume, and returns then.
      if (status === 'paused' || status === 'waitingForPause') {
        if (status === 'waitingForPause')
          this.#set('paused', { pausedAt: clock() });
        this.#drive = null;
        return;
      }
      if (failure === undefined) return this.#finish('complete', { output });
      await this.#undo('errored', { error: plain(failure) }, failure);
    })();
    this.ctx.waitUntil(done);
  }

  #record(type, name) {
    if (
      typeof name !== 'string' ||
      name.length > 256 ||
      /[\x00-\x1f]/.test(name)
    ) {
      throw fatalError(
        `Step name "${name}" exceeds max length (256 chars) or invalid ` +
          'characters found'
      );
    }
    const k = `${type}:${name}`;
    const count = (this.#counts.get(k) ?? 0) + 1;
    this.#counts.set(k, count);
    const key = `s:${type}:${count}:${name}`;
    const fresh = () => {
      const ord = (this.#kv.get('ord') ?? 0) + 1;
      this.#kv.put('ord', ord);
      return { ord, type, name, count };
    };
    return { key, count, record: this.#kv.get(key) ?? fresh() };
  }

  async #do(generation, name, config, callback, rollback) {
    if (typeof config === 'function') {
      [config, callback, rollback] = [{}, config, callback];
    }
    if (typeof callback !== 'function') {
      throw fatalError(`Step "${name}" requires a callback function`);
    }
    const r = config?.retries;
    if (
      (r && (r.limit === undefined || r.delay === undefined)) ||
      config?.timeout === 0
    ) {
      throw fatalError(
        `Step config for "${name}" is in a invalid format. See ` +
          'https://developers.cloudflare.com/workflows/build/' +
          'sleeping-and-retrying/'
      );
    }
    if (!(await this.#enter(generation))) throw aborted();
    if (++this.#steps > STEP_LIMIT) {
      throw fatalError(
        `The limit of ${STEP_LIMIT} steps has been reached. This limit can ` +
          'be changed in your worker configuration.'
      );
    }
    const cfg = {
      ...DEFAULTS,
      ...config,
      retries: { ...DEFAULTS.retries, ...config.retries },
    };
    const dynamic = typeof cfg.retries.delay === 'function';
    const shown = { ...cfg, retries: { ...cfg.retries } };
    if (dynamic) delete shown.retries.delay;
    const { key, count, record } = this.#record('do', name);
    const ctx = (attempt) => ({
      step: { name, count },
      attempt,
      config: shown,
    });
    const undo = rollback && {
      name,
      fn: rollback.rollback.dup?.() ?? rollback.rollback,
      config: rollback.rollbackConfig,
      ctx: ctx(record.attempts ?? 1),
      output: record.value,
    };
    if (undo) this.#rollbacks.push(undo);
    if (record.done) return record.value;
    if (record.error) throw error(record.error.name, record.error.message);
    const stepName = `${name}-${count}`;
    if (record.attempts === undefined) {
      record.attempts = 0;
      this.#kv.put(key, record);
      const logged = { ...shown, retries: { ...shown.retries } };
      if (dynamic) logged.retries.delay = '[dynamic]';
      this.#log('step_started', { stepName, config: logged });
    }
    for (;;) {
      if (record.retryAt > clock()) {
        await this.#wait(record.retryAt);
      }
      if (!(await this.#enter(generation))) throw aborted();
      // An attempt that ended the process failed.
      if (record.attempts > cfg.retries.limit) {
        const e = error(
          'WorkflowInternalError',
          `Attempt ${record.attempts} of step "${name}" did not finish`
        );
        this.#kv.put(key, { ...record, error: plain(e) });
        this.#log('step_errored', { stepName });
        throw e;
      }
      const attempt = ++record.attempts;
      this.#kv.put(key, record);
      this.#log('attempt_started', { stepName, attempt });
      this.#busy++;
      this.#settle();
      const limit = ms(cfg.timeout);
      const timeout = this.#wait(clock() + limit);
      let value, failure;
      try {
        value = await Promise.race([
          callback(ctx(attempt)),
          timeout.then(() => {
            throw error(
              'WorkflowTimeoutError',
              `Execution timed out after ${limit}ms`
            );
          }),
        ]);
      } catch (e) {
        failure = e;
      } finally {
        timeout.cancel();
        if (this.#drive === generation) this.#busy--;
      }
      if (!this.#live(generation)) throw aborted();
      this.#settle();
      if (failure === undefined) {
        try {
          this.#kv.put(key, { ...record, done: true, value });
        } catch {
          this.#finish('errored', {
            error: plain(
              fatalError(
                'The execution of the Workflow instance was terminated, as ' +
                  `the step "${name}" returned a value which is not ` +
                  'serialisable'
              )
            ),
          });
          throw aborted();
        }
        if (undo) undo.output = value;
        this.#log('attempt_completed', { stepName, attempt });
        const output = cfg.sensitive === 'output' ? '[REDACTED]' : value;
        this.#log('step_completed', { stepName, output });
        return value;
      }
      const failed = plain(failure);
      if (nonRetryable(failure) || attempt > cfg.retries.limit) {
        this.#kv.put(key, { ...record, error: failed });
        this.#log('attempt_errored', { stepName, attempt, error: failed });
        this.#log('step_errored', { stepName });
        throw failure;
      }
      const retryDelayMs = await this.#delay(
        cfg,
        failure,
        ctx(attempt),
        attempt
      );
      record.retryAt = clock() + retryDelayMs;
      this.#kv.put(key, record);
      this.#log('attempt_errored', {
        stepName,
        attempt,
        retryDelayMs,
        error: failed,
      });
    }
  }

  async #delay(cfg, failure, ctx, attempt) {
    const { delay, backoff } = cfg.retries;
    if (typeof delay === 'function') {
      const step = `${ctx.step.name}-${ctx.step.count}`;
      try {
        return ms(await delay({ ctx, error: failure }));
      } catch (e) {
        throw error(
          'NonRetryableDelayError',
          `The delay function for step "${step}" threw an error: ${e.message}`
        );
      }
    }
    const base = ms(delay);
    if (backoff === 'linear') return base * attempt;
    if (backoff === 'exponential') return base * 2 ** (attempt - 1);
    return base;
  }

  async #sleep(generation, name, duration, until) {
    if (!(await this.#enter(generation))) throw aborted();
    const { key, count, record } = this.#record('sleep', name);
    if (record.done) return;
    const stepName = `${name}-${count}`;
    if (record.deadline === undefined) {
      const now = clock();
      record.deadline =
        until === undefined ? now + ms(duration) : +new Date(until);
      if (record.deadline < now) {
        throw new Error(
          "You can't sleep until a time in the past, time-traveler"
        );
      }
      this.#kv.put(key, record);
      this.#log('sleep_started', {
        stepName,
        durationMs: record.deadline - now,
      });
    }
    await this.#wait(record.deadline);
    if (!(await this.#enter(generation))) throw aborted();
    this.#kv.put(key, { ...record, done: true });
    this.#log('sleep_completed', { stepName });
  }

  async #waitForEvent(generation, name, { type, timeout = '24 hours' } = {}) {
    if (!(await this.#enter(generation))) throw aborted();
    const { key, count, record } = this.#record('waitForEvent', name);
    if (record.done) return record.value;
    if (record.error) throw error(record.error.name, record.error.message);
    const stepName = `${name}-${count}`;
    if (record.deadline === undefined) {
      record.deadline = clock() + ms(timeout);
      this.#kv.put(key, record);
      this.#log('wait_started', { stepName, eventType: type });
    }
    for (;;) {
      if (!(await this.#enter(generation))) throw aborted();
      // Taken and recorded in one turn, so a crash keeps one or the other.
      const event = this.#take(type);
      if (event) {
        this.#kv.put(key, { ...record, done: true, value: event });
        this.#log('wait_completed', { stepName });
        return event;
      }
      if ((await this.#wait(record.deadline, type)) !== TIMEOUT) continue;
      if (!(await this.#enter(generation))) throw aborted();
      const e = error(
        'WorkflowTimeoutError',
        `Execution timed out after ${ms(timeout)}ms`
      );
      this.#kv.put(key, { ...record, error: plain(e) });
      this.#log('wait_timed_out', { stepName });
      throw e;
    }
  }

  // The oldest event of `type` sent and not yet taken.
  #take(type) {
    for (const [key, event] of this.#kv.list({ prefix: 'e:' })) {
      if (event.type !== type) continue;
      this.#kv.delete(key);
      return event;
    }
  }

  // Rolls back the steps that registered a rollback, then finishes as
  // `status`. Those are the steps this `run()` reached: after a crash, the
  // ones it reached again.
  async #undo(status, fields, failure) {
    if (this.#rollbacks.length > 0) {
      this.#set('rollingBack', { ...fields, after: status });
      this.#release();
      const ok = await this.#rollback(failure);
      if (this.#meta?.status !== 'rollingBack') return;
      this.#log(ok ? 'rollback_completed' : 'rollback_errored');
    }
    this.#finish(status, fields);
  }

  // Runs the rollbacks newest first: whether all of them succeeded.
  async #rollback(failure) {
    for (const undo of this.#rollbacks.toReversed()) {
      const stepName = undo.name;
      this.#log('rollback_step_started', { stepName });
      const cfg = {
        ...DEFAULTS,
        ...undo.config,
        retries: { ...DEFAULTS.retries, ...undo.config?.retries },
      };
      for (let attempt = 1; ; attempt++) {
        if (this.#meta?.status !== 'rollingBack') return false;
        try {
          await undo.fn({
            ctx: undo.ctx,
            error: failure,
            output: undo.output,
            stepName,
          });
          this.#log('rollback_step_completed', { stepName });
          break;
        } catch (e) {
          if (nonRetryable(e) || attempt > cfg.retries.limit) {
            this.#log('rollback_step_errored', { stepName, error: plain(e) });
            return false;
          }
          await this.#wait(
            clock() + (await this.#delay(cfg, e, undo.ctx, attempt))
          );
        }
      }
    }
    return true;
  }

  #destroy() {
    this.ctx.storage.deleteAll();
    this.ctx.storage.deleteAlarm();
    this.#drive = null;
    this.#alarm = null;
    this.#release();
    this.#notify();
  }

  // The binding's calls, each for the instance this object is.

  create(params, retention) {
    const id = this.ctx.id.name;
    if (this.#meta) {
      throw workflowError(
        'instance.already_exists',
        `Workflow instance with id "${id}" already exists`
      );
    }
    this.#kv.put('meta', {
      id,
      params,
      created: clock(),
      status: 'queued',
      generation: crypto.randomUUID(),
      retention: {
        success: ms(retention?.successRetention ?? RETENTION_MS),
        error: ms(retention?.errorRetention ?? RETENTION_MS),
      },
    });
    this.#log('workflow_queued');
    this.#wake();
  }

  exists() {
    this.#wake();
    return this.#meta !== undefined;
  }

  status() {
    this.#wake();
    const m = this.#meta;
    if (!m) throw notFound();
    return { status: m.status, error: m.error, output: m.output ?? null };
  }

  pause() {
    const m = this.#meta;
    if (!m) throw notFound();
    if (m.status === 'running' && this.#busy > 0) this.#set('waitingForPause');
    else if (['queued', 'running', 'waiting'].includes(m.status)) {
      this.#set('paused', { pausedAt: clock() });
    }
    this.#arm();
  }

  resume() {
    const m = this.#meta;
    if (!m) throw notFound();
    if (m.status === 'waitingForPause') {
      this.#set('running');
    } else if (m.status === 'paused') {
      // Waits resume with what they had left.
      const shift = clock() - m.pausedAt;
      for (const w of this.#waiters) w.at += shift;
      for (const [key, r] of [...this.#kv.list({ prefix: 's:' })]) {
        if (r.done || r.error) continue;
        if (r.deadline !== undefined) r.deadline += shift;
        if (r.retryAt !== undefined) r.retryAt += shift;
        this.#kv.put(key, r);
      }
      this.#set(this.#drive === m.generation ? 'running' : 'queued');
    } else {
      throw workflowError(
        'instance.cannot_resume',
        `Cannot resume instance with status "${m.status}"`
      );
    }
    this.#open();
    this.#settle();
    this.#wake();
  }

  async terminate(options = {}) {
    const m = this.#meta;
    if (!m) throw notFound();
    if (TERMINAL.has(m.status)) {
      throw workflowError(
        'instance.cannot_terminate',
        'Cannot terminate instance since its on a finite state'
      );
    }
    if (options.rollback && m.status !== 'rollingBack') {
      const e = error('Terminated', 'Instance terminated during rollback');
      return this.#undo('terminated', {}, e);
    }
    this.#finish('terminated');
  }

  restart({ from } = {}) {
    const m = this.#meta;
    if (!m) throw notFound();
    const steps = [...this.#kv.list({ prefix: 's:' })];
    steps.sort((a, b) => a[1].ord - b[1].ord);
    let keep = 0;
    if (from) {
      const type = from.type ?? 'do';
      const count = from.count ?? 1;
      keep = steps.findIndex(
        ([, r]) => r.type === type && r.name === from.name && r.count === count
      );
      if (keep < 0) {
        throw workflowError(
          'instance.cannot_restart',
          `Step "${from.name}" not found in execution history`
        );
      }
    }
    for (const [key] of steps.slice(keep)) this.#kv.delete(key);
    for (const prefix of ['e:', 'l:']) {
      for (const [key] of [...this.#kv.list({ prefix })]) this.#kv.delete(key);
    }
    const { id, params, created, retention } = m;
    this.#kv.put('meta', {
      id,
      params,
      created,
      retention,
      status: 'queued',
      generation: crypto.randomUUID(),
    });
    this.#log('workflow_queued');
    this.#drive = null;
    this.#release();
    this.#wake();
  }

  sendEvent({ type, payload }) {
    const m = this.#meta;
    if (!m) throw notFound();
    const seq = (this.#kv.get('events') ?? 0) + 1;
    this.#kv.put('events', seq);
    this.#kv.put(`e:${pad(seq)}`, {
      type,
      payload,
      timestamp: new Date(clock()),
    });
    for (const w of this.#waiters) {
      if (w.type !== type) continue;
      this.#waiters.delete(w);
      w.wake();
      break;
    }
    this.#wake();
  }

  subscribe({ cursor = 0, filter } = {}) {
    if (!this.#meta) throw notFound();
    let done = false;
    const next = async () => {
      for (;;) {
        if (done) return { done: true, value: undefined };
        const start = `l:${pad(cursor + 1)}`;
        const [entry] = this.#kv.list({ prefix: 'l:', start, limit: 1 });
        if (entry) {
          const event = entry[1];
          cursor = event.eventId;
          done = FINAL.has(event.type);
          if (!filter || filter.includes(event.type)) {
            return { done: false, value: event };
          }
          continue;
        }
        const m = this.#meta;
        if (!m || TERMINAL.has(m.status))
          return { done: true, value: undefined };
        await new Promise((wake) => this.#watchers.add(wake));
      }
    };
    return new Subscription(next, () => (done = true));
  }

  destroy() {
    if (!this.#meta) throw notFound();
    this.#destroy();
  }
}

const body = (message) => workflowError('body', message);
const invalidId = () =>
  workflowError('instance.invalid_id', 'Instance ID is invalid');
const valid = (id) => typeof id === 'string' && id.length <= 100 && ID.test(id);
const reserved = (id) =>
  ['batch', 'terminate', 'terminateAll'].includes(id) ||
  /^cf_[0-9a-f]{64}$/.test(id);

// A Workflow's API, `ctx.props.workflow` naming it.
export class WorkflowBinding extends WorkerEntrypoint {
  #instance(id) {
    return this.env[this.ctx.props.workflow].getByName(id);
  }

  async #get(id) {
    const instance = this.#instance(id);
    if (!(await instance.exists())) throw new Error('instance.not_found');
    return instance;
  }

  async getInstance(id) {
    await this.#get(id);
    return { id };
  }

  async create(options = {}) {
    const id = 'id' in options ? options.id : crypto.randomUUID();
    if (!valid(id))
      throw error('WorkflowError', 'Workflow instance has invalid id');
    await this.#instance(id).create(options.params ?? {}, options.retention);
    return { id };
  }

  async createBatch(options) {
    if (Array.isArray(options)) {
      if (options.length === 0) {
        throw new Error(
          'WorkflowError: batchCreate should have at least 1 instance'
        );
      }
      if (options.length > MAX_BATCH) {
        throw new Error(
          'WorkflowError: batchCreate only supports 100 instances at a time'
        );
      }
      const { created } = await this.#createAll(options);
      return created;
    }
    let instances = options?.instances;
    if (options && 'count' in options) {
      const { count, params, retention } = options;
      if (!Number.isInteger(count) || count < 1) {
        throw body('count must be a positive integer');
      }
      if (count > MAX_BATCH) {
        throw body('batchCreate only supports 100 instances at a time');
      }
      instances = Array.from({ length: count }, () => ({ params, retention }));
    } else if (!Array.isArray(instances)) {
      throw body('Provided argument is invalid');
    } else if (instances.length === 0 || instances.length > MAX_BATCH) {
      throw body('Batch size exceeds maximum allowed');
    }
    return this.#createAll(instances, true);
  }

  // Creates `instances` in order. An id given twice, or that exists, is an
  // error entry: reported with `errors`, else skipped. Any other failure
  // throws.
  async #createAll(instances, errors = false) {
    const out = { created: [], errors: [] };
    const seen = new Set();
    for (const [index, options] of instances.entries()) {
      const id = options.id ?? crypto.randomUUID();
      if (!valid(id) || (errors && reserved(id))) throw invalidId();
      if (seen.has(id)) {
        out.errors.push({
          index,
          id,
          code: 10415,
          message: 'workflows.api.error.instance.duplicate_in_batch',
        });
        continue;
      }
      seen.add(id);
      try {
        await this.create({ ...options, id });
        out.created.push({ id });
      } catch (e) {
        if (!String(e?.message).includes('(instance.already_exists)')) throw e;
        out.errors.push({
          index,
          id,
          code: 10405,
          message: 'workflows.api.error.instance.already_exists',
        });
      }
    }
    return errors ? out : { created: out.created };
  }

  async deleteInstance(id) {
    if (typeof id !== 'string' || !ADDRESS.test(id)) throw invalidId();
    await this.#instance(id).destroy();
  }

  async deleteBatch(options) {
    const ids = options?.instances;
    if (!Array.isArray(ids)) throw body('Provided argument is invalid');
    if (ids.length > MAX_BATCH) {
      throw body('batchDeleteInstances only supports 100 instances at a time');
    }
    if (ids.length === 0) {
      throw body('batchDeleteInstances should have at least 1 instance');
    }
    if (!ids.every((id) => typeof id === 'string' && ADDRESS.test(id))) {
      throw invalidId();
    }
    const unique = [...new Set(ids)];
    const results = await Promise.allSettled(
      unique.map((id) => this.#instance(id).destroy())
    );
    const outcome = new Map(unique.map((id, i) => [id, results[i]]));
    const out = { deleted: [], errors: [] };
    for (const id of ids) {
      const r = outcome.get(id);
      if (r.status === 'fulfilled') out.deleted.push({ id });
      else if (String(r.reason?.message).includes('(instance.not_found)')) {
        out.errors.push({
          id,
          code: 10400,
          message: 'workflows.api.error.instance.not_found',
        });
      } else {
        out.errors.push({
          id,
          code: 10001,
          message: 'workflows.api.error.internal_server',
        });
      }
    }
    return out;
  }

  async pause(id) {
    await (await this.#get(id)).pause();
  }

  async resume(id) {
    await (await this.#get(id)).resume();
  }

  async terminate(id, options) {
    await (await this.#get(id)).terminate(options);
  }

  async restart(id, options) {
    await (await this.#get(id)).restart(options);
  }

  async status(id) {
    return (await this.#get(id)).status();
  }

  async sendEvent(id, event) {
    await (await this.#get(id)).sendEvent(event);
  }

  async subscribe(id, options = {}) {
    const { cursor, filter } = options;
    if (
      Object.keys(options).some((k) => k !== 'cursor' && k !== 'filter') ||
      (cursor !== undefined && !(Number.isInteger(cursor) && cursor >= 0)) ||
      (filter !== undefined && !Array.isArray(filter))
    ) {
      throw new Error('Invalid Workflow subscription options');
    }
    return (await this.#get(id)).subscribe(options);
  }
}

export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (env.TEST_CLOCK && url.pathname === '/clock') {
      skew += Number(url.searchParams.get('advance'));
      return Response.json({ now: clock() });
    }
    return new Response('Not Found', { status: 404 });
  },
};
