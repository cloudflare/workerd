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
//   WORKFLOWS  JSON, by Workflow name: `steps`, its step limit (default
//              10,000), and `retention`, `{success, error}` as
//              `WorkflowInstanceCreateOptions.retention` takes, for instances
//              that do not set their own (optional)
//   TEST_CLOCK JSON `true` for tests (optional)
//
// An instance is one Durable Object, named by its id. Its storage holds the
// instance (`meta`), one record per step it reached (`s:<type>:<n>:<name>`,
// the n-th of that name), the events sent to it not yet taken (`e:<seq>`),
// and its history for `subscribe()` (`l:<id>`). `run()` runs once per
// instance in memory: a step that waits (a sleep, a retry's delay, an event)
// waits there, and the instance's alarm, set for the earliest wait, ends it.
// A sleep or event wait longer than `REST_MS` ends the `run()` in memory
// instead (its step throws `WorkflowAbortedError`), and `run()` runs again at
// the deadline or when an event comes. Whenever `run()` runs again, after
// a crash, a long wait, or a restart of the instance, it runs from the start:
// a step that completed returns its result without running again, a wait
// keeps its deadline. While user code runs, the alarm is set no later than
// `WATCH_MS` ahead (an attempt's, its timeout), so a crash runs it again by
// then. A finished instance is deleted after its retention.
//
// With `TEST_CLOCK`, POST /clock?advance=<ms> moves this process's clock; an
// instance acts on it at its next call or alarm.

import {
  DurableObject,
  RpcTarget,
  WorkerEntrypoint,
  env,
} from 'cloudflare:workers';

const REST_MS = 60 * 1000;
const WATCH_MS = 10 * 60 * 1000;
const RETENTION_MS = 30 * 24 * 60 * 60 * 1000;
const MAX_BATCH = 100;
const DELAY_FN_MS = 5_000;
// A stream a step returns is stored as rows of `CHUNK` bytes.
const CHUNK = 256 * 1024;
const PREVIEW = 1024;
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
// Miniflare's numbers for the statuses.
const STATUS_CODES = Object.fromEntries(
  [
    'queued',
    'running',
    'paused',
    'errored',
    'terminated',
    'complete',
    'waitingForPause',
    'waiting',
  ].map((s, i) => [s, i])
);
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
const START = new Set(['workflow_queued', 'workflow_started']);
const EVENT_TYPES = new Set([
  ...Object.values(EVENTS),
  ...['workflow_started', 'rollback_completed', 'rollback_errored'],
  ...['step', 'attempt', 'rollback_step', 'rollback_attempt'].flatMap((t) =>
    ['started', 'completed', 'errored'].map((e) => `${t}_${e}`)
  ),
  ...['sleep_started', 'sleep_completed'],
  ...['wait_started', 'wait_completed', 'wait_timed_out'],
]);
const BACKOFF = new Set(['constant', 'linear', 'exponential']);
const TIMEOUT = Symbol('timeout');
const DELETED = 'The Workflow instance was deleted';

let skew = 0;
const clock = () => Date.now() + skew;
const pad = (n) => String(n).padStart(12, '0');
// What the logs show of a stored stream, from its first chunk.
function preview(head, bytes) {
  const shown = head.subarray(0, PREVIEW * 4);
  const cut = bytes > shown.byteLength;
  try {
    const decoder = new TextDecoder('utf-8', { fatal: true });
    const text = decoder.decode(shown, { stream: cut });
    if (!cut && text.length <= PREVIEW) return text;
    return text.slice(0, PREVIEW) + '[truncated output]';
  } catch {
    return `[ReadableStream (binary): ${bytes} bytes]`;
  }
}

// A step's stream, stored.
class Stored {
  constructor(fields) {
    Object.assign(this, fields);
  }
}

// Releases a step result, an RPC result with its stubs.
const dispose = (v) => v?.[Symbol.dispose]?.();

const error = (name, message) => Object.assign(new Error(message), { name });
const workflowError = (code, message) =>
  error('WorkflowError', `(${code}) ${message}`);
const fatalError = (message) => error('WorkflowFatalError', message);
// What a step of a `run()` that may no longer take steps throws.
const aborted = () =>
  error('WorkflowAbortedError', 'This run of the instance has ended');
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
const PRESERVE =
  globalThis.Cloudflare?.compatibilityFlags
    ?.workflows_preserve_non_retryable_error_message ?? false;
// A NonRetryableError as its step's attempt logs it, else as the instance's
// error when `run()` does not handle it: as thrown under
// workflows_preserve_non_retryable_error_message, else replaced.
function nonRetryableError(e, attempt) {
  const message = String(e?.message).replace(/^NonRetryableError:?\s*/, '');
  if (PRESERVE) return { name: 'NonRetryableError', message };
  return {
    name: 'WorkflowFatalError',
    message: attempt
      ? `Step threw a NonRetryableError with message "${message}"`
      : 'The execution of the Workflow instance was terminated, as a step ' +
        'threw an NonRetryableError and it was not handled',
  };
}
// The instance's error when `run()` throws `e`.
const failureOf = (e) => (nonRetryable(e) ? nonRetryableError(e) : plain(e));

// `ms(d)`, or NaN if `d` is not a duration.
function duration(d) {
  try {
    return ms(d);
  } catch {
    return NaN;
  }
}

const span = (d) =>
  (typeof d === 'number' && d >= 0) ||
  (typeof d === 'string' && duration(d) >= 0);

// Whether `c` is a step config: only these keys, with these values.
function validConfig(c) {
  if (c === undefined) return true;
  if (typeof c !== 'object' || c === null) return false;
  const { retries: r, timeout, sensitive, ...rest } = c;
  if (Object.keys(rest).length > 0) return false;
  if (sensitive !== undefined && sensitive !== 'output') return false;
  if (timeout !== undefined && (!span(timeout) || timeout === 0)) return false;
  if (r === undefined) return true;
  const { limit, delay, backoff, ...other } = r;
  return (
    Object.keys(other).length === 0 &&
    Number.isInteger(limit) &&
    limit >= 0 &&
    (typeof delay === 'function' || span(delay)) &&
    (backoff === undefined || BACKOFF.has(backoff))
  );
}

// Milliseconds: a number, or "<n> <unit>[s]".
function ms(d) {
  if (typeof d === 'number') return d;
  const m = /^\s*(\d+(?:\.\d+)?)\s*([a-z]+?)s?\s*$/.exec(String(d));
  const unit = m && UNITS[m[2]];
  if (!unit) throw new TypeError(`Invalid duration "${d}"`);
  return Number(m[1]) * unit;
}

const stepConfig = (config) => ({
  ...DEFAULTS,
  ...config,
  retries: { ...DEFAULTS.retries, ...config?.retries },
});

// A step's config as its callbacks see it: without a delay function.
function shownConfig(cfg) {
  const shown = { ...cfg, retries: { ...cfg.retries } };
  if (typeof cfg.retries.delay === 'function') delete shown.retries.delay;
  return shown;
}

// What rolling back the step `name` runs.
// A step's rollback: `stream` and `key` for a step that returned a stream.
const undoOf = (rollback, name, ctx, record, key) => ({
  name,
  fn: rollback.rollback.dup?.() ?? rollback.rollback,
  config: rollback.rollbackConfig,
  ctx,
  output: record.value,
  stream: record.stream,
  key,
});

// The testing API's changes to how an instance runs, each a call into it.
class Modifier extends RpcTarget {
  #modify;
  constructor(modify) {
    super();
    this.#modify = modify;
  }
  disableSleeps(steps) {
    return this.#modify('disableSleeps', steps);
  }
  disableRetryDelays(steps) {
    return this.#modify('disableRetryDelays', steps);
  }
  mockStepResult(step, result) {
    return this.#modify('mockStepResult', step, result);
  }
  mockStepError(step, error, times) {
    return this.#modify('mockStepError', step, error, times);
  }
  forceStepTimeout(step, times) {
    return this.#modify('forceStepTimeout', step, times);
  }
  mockEvent(event) {
    return this.#modify('mockEvent', event);
  }
  forceEventTimeout(step) {
    return this.#modify('forceEventTimeout', step);
  }
}

// Applies an introspection session's recorded `op` to a new instance.
function apply(modifier, op) {
  switch (op.type) {
    case 'mockStepResult': {
      const r = op.stepResult;
      const stream =
        r?.__workflowIntrospectionStreamResult &&
        new ReadableStream({
          start(c) {
            for (const chunk of r.chunks) c.enqueue(chunk.slice());
            c.close();
          },
        });
      return modifier.mockStepResult(op.step, stream || r);
    }
    case 'mockStepError': {
      const e = error(op.error.name, op.error.message);
      return modifier.mockStepError(op.step, e, op.times);
    }
    case 'forceStepTimeout':
      return modifier.forceStepTimeout(op.step, op.times);
    case 'mockEvent':
      return modifier.mockEvent(op.event);
    default:
      return modifier[op.type](op.steps ?? op.step);
  }
}

// The testing API's introspection sessions, by Workflow:
// `{id, operations, instanceIds}`.
const sessions = new Map();

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
  #table = false;

  // The `run()` in memory, `{generation}`, else null. Its steps hold it.
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
  // The generation whose rollback runs in memory, else null.
  #undoing = null;
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

  // Whether `run` is the `run()` in memory and may take steps.
  #owns(run) {
    return this.#drive === run && this.#live(run.generation);
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

  // Releases the rollbacks' stubs, for `next`.
  #dropRollbacks(next = []) {
    for (const undo of this.#rollbacks) dispose(undo.fn);
    this.#rollbacks = next;
  }

  #finish(status, fields = {}) {
    this.#dropRollbacks();
    const { retention } = this.#meta;
    const keep = status === 'errored' ? retention.error : retention.success;
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

  // Whether the step may go on: false once its run has ended (the instance
  // finished, restarted or rests); while it is paused, once it resumes.
  async #enter(run) {
    for (;;) {
      if (!this.#owns(run)) return false;
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

  // Moves between running, waiting and pausing as the steps of the `run()`
  // in memory start and wait.
  #settle() {
    if (!this.#drive) return;
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
    if (this.#lost(m)) {
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
      const resting = m.dormant > now;
      if (this.#lost(m) && !resting) this.#run(m);
    }
    this.#arm();
  }

  // The alarm: at the earliest wait, within `WATCH_MS` while user code runs,
  // now when `run()` should be in memory and is not (at the deadline when it
  // rests), and a finished instance's expiry. None while paused.
  #arm() {
    const m = this.#meta;
    let next = Infinity;
    if (m && TERMINAL.has(m.status)) {
      next = m.expires;
    } else if (m && m.status !== 'paused') {
      if (this.#lost(m)) next = m.dormant > clock() ? m.dormant : clock();
      for (const w of this.#waiters) next = Math.min(next, w.at);
      if (m.status !== 'waiting') next = Math.min(next, clock() + WATCH_MS);
    }
    // A stale alarm costs one wake. One already set no later stays: setting a
    // due alarm again defers it.
    const at = next - skew;
    if (next === Infinity || (this.#alarm !== null && this.#alarm <= at))
      return;
    this.ctx.storage.setAlarm(at);
    this.#alarm = at;
  }

  async alarm() {
    this.#alarm = null;
    this.#wake();
  }

  // Whether nothing of `m`'s generation is in memory: neither its `run()`
  // nor its rollback.
  #lost(m) {
    const { generation } = m;
    return (
      this.#drive?.generation !== generation && this.#undoing !== generation
    );
  }

  // Runs `run()` for the instance's generation, from the start.
  #run(m) {
    const run = (this.#drive = { generation: m.generation });
    this.#busy = 0;
    this.#counts = new Map();
    this.#dropRollbacks();
    this.#steps = 0;
    if (!m.started) {
      this.#kv.put('meta', { ...m, started: true });
      this.#log('workflow_started', { params: m.params });
    }
    if (m.status === 'queued' || m.dormant !== undefined)
      this.#set('running', { dormant: undefined, wakesOn: undefined });
    const step = new Step({
      do: (...args) => this.#do(run, ...args),
      sleep: (name, duration, until) => this.#sleep(run, name, duration, until),
      waitForEvent: (name, options) => this.#waitForEvent(run, name, options),
    });
    const done = (async () => {
      let output, failure;
      try {
        output = await this.#call(m, step);
      } catch (e) {
        failure = e;
      }
      if (!this.#owns(run)) return;
      const { status } = this.#meta;
      // A pause asked for before `run()` returned wins: it runs again on
      // resume, and returns then.
      if (status === 'paused' || status === 'waitingForPause') {
        if (status === 'waitingForPause')
          this.#set('paused', { pausedAt: clock() });
        this.#drive = null;
        return;
      }
      if (failure === undefined) {
        try {
          return this.#finish('complete', { output: structuredClone(output) });
        } catch {
          failure = fatalError(
            'The execution of the Workflow instance was terminated, as ' +
              'run() returned a value which is not serialisable'
          );
        } finally {
          dispose(output);
        }
      }
      await this.#undo('errored', { error: failureOf(failure) }, failure);
    })();
    this.ctx.waitUntil(done);
  }

  #call(m, step) {
    const event = {
      payload: m.params,
      timestamp: new Date(m.created),
      instanceId: m.id,
      workflowName: this.ctx.props.workflowName,
    };
    return this.ctx.props.workflowClass.run(event, step);
  }

  get #config() {
    return env.WORKFLOWS?.[this.ctx.props.workflowName] ?? {};
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

  // The rows of the streams steps returned.
  get #sql() {
    const sql = this.ctx.storage.sql;
    this.#table ||= !!sql.exec(
      'CREATE TABLE IF NOT EXISTS chunks (key TEXT, attempt INTEGER, ' +
        'i INTEGER, chunk BLOB, PRIMARY KEY (key, attempt, i)) WITHOUT ROWID'
    );
    return sql;
  }

  // Stores `stream`, which `attempt` of the step at `key` returned, until
  // `reading.cancelled` is set.
  async #store(key, attempt, stream, reading) {
    const reader = stream.getReader();
    if (reading.cancelled) return reader.cancel(reading.cancelled);
    reading.cancel = (e) => reader.cancel(e).catch(() => {});
    const sql = this.#sql;
    sql.exec('DELETE FROM chunks WHERE key = ?', key);
    const buf = new Uint8Array(CHUNK);
    let fill = 0;
    let chunks = 0;
    let bytes = 0;
    let head;
    const flush = () => {
      const chunk = buf.slice(0, fill);
      head ??= chunk;
      sql.exec(
        'INSERT INTO chunks VALUES (?, ?, ?, ?)',
        key,
        attempt,
        chunks++,
        chunk.buffer
      );
      bytes += fill;
      fill = 0;
    };
    for (;;) {
      let r;
      try {
        r = await reader.read();
      } catch (e) {
        if (reading.cancelled) throw reading.cancelled;
        throw error(
          'InvalidStepReadableStreamError',
          `Failed to read from step ReadableStream output. ${e?.message ?? e}`
        );
      }
      if (reading.cancelled) throw reading.cancelled;
      if (r.done) break;
      let v = r.value;
      while (v.byteLength > 0) {
        const n = Math.min(CHUNK - fill, v.byteLength);
        buf.set(v.subarray(0, n), fill);
        fill += n;
        v = v.subarray(n);
        if (fill === CHUNK) flush();
      }
    }
    if (fill > 0) flush();
    const shown = preview(head ?? new Uint8Array(), bytes);
    return new Stored({ attempt, chunks, bytes, preview: shown });
  }

  // The stream of a stored step's output, read a row at a time.
  #replayStream(key, stepName, { attempt, chunks, bytes }) {
    const sql = this.#sql;
    const [row] = sql
      .exec(
        'SELECT COUNT(*) AS n, COALESCE(SUM(LENGTH(chunk)), 0) AS size ' +
          'FROM chunks WHERE key = ? AND attempt = ?',
        key,
        attempt
      )
      .toArray();
    if (row.n !== chunks || row.size !== bytes) {
      throw error(
        'WorkflowInternalError',
        `Stored output for ${stepName} is corrupt or incomplete.`
      );
    }
    let i = 0;
    return new ReadableStream({
      type: 'bytes',
      pull(c) {
        if (i === chunks) return c.close();
        const [{ chunk }] = sql
          .exec(
            'SELECT chunk FROM chunks WHERE key = ? AND attempt = ? AND i = ?',
            key,
            attempt,
            i++
          )
          .toArray();
        c.enqueue(new Uint8Array(chunk));
      },
    });
  }

  async #do(run, name, config, callback, rollback) {
    if (typeof config === 'function') {
      [config, callback, rollback] = [{}, config, callback];
    }
    if (typeof callback !== 'function') {
      throw fatalError(`Step "${name}" requires a callback function`);
    }
    if (!validConfig(config)) {
      throw fatalError(
        `Step config for "${name}" is in a invalid format. See ` +
          'https://developers.cloudflare.com/workflows/build/' +
          'sleeping-and-retrying/'
      );
    }
    if (!(await this.#enter(run))) throw aborted();
    const limit = this.#config.steps ?? STEP_LIMIT;
    if (++this.#steps > limit) {
      throw fatalError(
        `The limit of ${limit} steps has been reached. This limit can ` +
          'be changed in your worker configuration.'
      );
    }
    const cfg = stepConfig(config);
    const dynamic = typeof cfg.retries.delay === 'function';
    const shown = shownConfig(cfg);
    const { key, count, record } = this.#record('do', name);
    const ctx = (attempt) => ({
      step: { name, count },
      attempt,
      config: shown,
    });
    const undo =
      rollback &&
      undoOf(rollback, name, ctx(record.attempts ?? 1), record, key);
    if (undo) this.#rollbacks.push(undo);
    const stepName = `${name}-${count}`;
    if (record.stream) return this.#replayStream(key, stepName, record.stream);
    if (record.done) return record.value;
    if (record.error) throw error(record.error.name, record.error.message);
    if (record.attempts === undefined) {
      record.attempts = 0;
      if (rollback) record.hasRollback = true;
      this.#kv.put(key, record);
      const logged = { ...shown, retries: { ...shown.retries } };
      if (dynamic) logged.retries.delay = '[dynamic]';
      this.#log('step_started', { stepName, config: logged });
    }
    // An attempt the process ended with failed, and is retried at once.
    if (record.attempts > 0 && record.retryAt === undefined) {
      const attempt = record.attempts;
      this.#log('attempt_errored', {
        stepName,
        attempt,
        error: {
          name: 'WorkflowInternalError',
          message: 'Attempt failed due to internal workflows error',
        },
      });
      if (attempt > cfg.retries.limit) {
        const e = error(
          'WorkflowInternalError',
          `Attempt ${attempt} of step "${name}" did not finish`
        );
        this.#kv.put(key, { ...record, error: plain(e) });
        this.#log('step_errored', { stepName });
        throw e;
      }
      record.retryAt = clock();
      this.#kv.put(key, record);
    }
    for (;;) {
      if (record.retryAt > clock()) {
        await this.#wait(record.retryAt);
      }
      if (!(await this.#enter(run))) throw aborted();
      const attempt = ++record.attempts;
      record.retryAt = undefined;
      this.#kv.put(key, record);
      this.#log('attempt_started', { stepName, attempt });
      this.#busy++;
      this.#settle();
      const limit = ms(cfg.timeout);
      const timeout = this.#wait(clock() + limit);
      const reading = {};
      const result = Promise.resolve()
        .then(() => {
          const mock = this.#mocked(stepName, attempt);
          return mock ? mock.value : callback(ctx(attempt));
        })
        .then((v) =>
          v instanceof ReadableStream
            ? this.#store(key, attempt, v, reading)
            : v
        );
      let value, failure;
      try {
        value = await Promise.race([
          result,
          timeout.then(() => {
            throw error(
              'WorkflowTimeoutError',
              `Execution timed out after ${limit}ms`
            );
          }),
        ]);
      } catch (e) {
        failure = e;
        reading.cancelled = e;
        reading.cancel?.(e);
        result.then(dispose, () => {});
      } finally {
        timeout.cancel();
        if (this.#drive === run) this.#busy--;
      }
      if (!this.#owns(run)) throw aborted();
      this.#settle();
      let stream;
      if (failure === undefined) {
        const live = value;
        if (live instanceof Stored) stream = { ...live };
        try {
          value = stream ? undefined : structuredClone(live);
          this.#kv.put(key, { ...record, done: true, value, stream });
          if (stream) value = this.#replayStream(key, stepName, stream);
        } catch (e) {
          // Storage takes up to 8 MiB a value; the message is production's.
          if (String(e?.message).includes('SQLITE_TOOBIG')) {
            failure = error(
              'WorkflowInternalError',
              `Step ${stepName} output is too large. Maximum allowed size ` +
                'is 1MiB.'
            );
          } else {
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
        } finally {
          dispose(live);
        }
      }
      if (failure === undefined) {
        if (undo) Object.assign(undo, stream ? { stream } : { output: value });
        this.#log('attempt_completed', { stepName, attempt });
        const sensitive = cfg.sensitive === 'output';
        const shown = stream ? stream.preview : value;
        const output = sensitive ? '[REDACTED]' : shown;
        const fields = output === undefined ? {} : { output };
        // A subscriber gets the stream itself.
        if (stream && !sensitive) fields.stream = key;
        this.#log('step_completed', { stepName, ...fields });
        return value;
      }
      // A stream that fails to read ends the instance.
      if (failure?.name === 'InvalidStepReadableStreamError') {
        const failed = plain(fatalError(failure.message));
        this.#kv.put(key, { ...record, error: failed });
        this.#log('attempt_errored', { stepName, attempt, error: failed });
        this.#log('step_errored', { stepName });
        this.#finish('errored', {
          error: plain(
            fatalError(
              'The execution of the Workflow instance was terminated, as the ' +
                `step "${name}" returned an invalid ReadableStream output. ` +
                failure.message
            )
          ),
        });
        throw aborted();
      }
      const failed = plain(failure);
      // The step fails with `thrown`, its attempt with `failure`.
      const fail = (thrown) => {
        this.#kv.put(key, { ...record, error: plain(thrown) });
        const logged = nonRetryable(failure)
          ? nonRetryableError(failure, true)
          : failed;
        this.#log('attempt_errored', { stepName, attempt, error: logged });
        this.#log('step_errored', { stepName });
        return thrown;
      };
      if (nonRetryable(failure) || attempt > cfg.retries.limit) {
        throw fail(failure);
      }
      let retryDelayMs;
      try {
        retryDelayMs = await this.#delay(cfg, failure, ctx(attempt), attempt);
      } catch (e) {
        if (!this.#owns(run)) throw aborted();
        throw fail(e);
      }
      if (!this.#owns(run)) throw aborted();
      const skip = this.#x('delay', stepName);
      record.retryAt = clock() + (skip ? 0 : retryDelayMs);
      this.#kv.put(key, record);
      this.#log('attempt_errored', {
        stepName,
        attempt,
        retryDelayMs,
        error: failed,
      });
    }
  }

  // The delay before the retry after `attempt`. A delay function has
  // `DELAY_FN_MS` to return a duration.
  async #delay(cfg, failure, ctx, attempt) {
    const { delay, backoff } = cfg.retries;
    if (typeof delay === 'function') {
      const step = `${ctx.step.name}-${ctx.step.count}`;
      const fail = (message) =>
        error(
          'NonRetryableDelayError',
          `The delay function for step "${step}" ${message}`
        );
      const timeout = this.#wait(clock() + DELAY_FN_MS);
      let d;
      try {
        d = await Promise.race([delay({ ctx, error: failure }), timeout]);
      } catch (e) {
        throw fail(`threw an error: ${e.message}`);
      } finally {
        timeout.cancel();
      }
      if (d === TIMEOUT) throw fail('did not return within 5 seconds');
      const value = typeof d === 'string' ? duration(d) : d;
      if (!Number.isFinite(value) || value < 0) {
        throw fail(
          'returned an invalid delay value (expected a number of ms or a ' +
            'duration string like "30 seconds")'
        );
      }
      return value;
    }
    const base = ms(delay);
    if (backoff === 'linear') return base * attempt;
    if (backoff === 'exponential') return base * 2 ** (attempt - 1);
    return base;
  }

  async #sleep(run, name, duration, until) {
    if (!(await this.#enter(run))) throw aborted();
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
    if (!this.#x('sleep', stepName)) {
      if (this.#rest(run, record.deadline)) throw aborted();
      await this.#wait(record.deadline);
      if (!(await this.#enter(run))) throw aborted();
    }
    this.#kv.put(key, { ...record, done: true });
    this.#log('sleep_completed', { stepName });
  }

  async #waitForEvent(run, name, { type, timeout = '24 hours' } = {}) {
    if (!(await this.#enter(run))) throw aborted();
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
      if (!(await this.#enter(run))) throw aborted();
      // Taken and recorded in one turn, so a crash keeps one or the other.
      const event = !this.#x('eventTimeout', stepName) && this.#take(type);
      if (event) {
        this.#kv.put(key, { ...record, done: true, value: event });
        this.#log('wait_completed', { stepName });
        return event;
      }
      if (!this.#x('eventTimeout', stepName)) {
        if (this.#rest(run, record.deadline, type)) throw aborted();
        if ((await this.#wait(record.deadline, type)) !== TIMEOUT) continue;
      }
      if (!(await this.#enter(run))) throw aborted();
      const e = error(
        'WorkflowTimeoutError',
        `Execution timed out after ${ms(timeout)}ms`
      );
      this.#kv.put(key, { ...record, error: plain(e) });
      this.#log('wait_timed_out', { stepName });
      throw e;
    }
  }

  // Ends `run` rather than wait in it until `at`, when that is more than
  // `REST_MS` away and nothing else of it runs or waits: `run()` runs again
  // at `at`, or when an event of `type` comes.
  #rest(run, at, type) {
    if (at - clock() <= REST_MS || this.#busy > 0 || this.#waiters.size > 0)
      return false;
    this.#drive = null;
    const rest = { dormant: at, wakesOn: type };
    if (this.#meta.status === 'waiting')
      this.#kv.put('meta', { ...this.#meta, ...rest });
    else this.#set('waiting', rest);
    this.#arm();
    return true;
  }

  // The oldest event of `type` sent and not yet taken.
  #take(type) {
    for (const [key, event] of this.#kv.list({ prefix: 'e:' })) {
      if (event.type !== type) continue;
      this.#kv.delete(key);
      return event;
    }
  }

  // The step records, in the order the steps were first reached.
  #history() {
    const records = [...this.#kv.list({ prefix: 's:' })];
    return records.sort((a, b) => a[1].ord - b[1].ord);
  }

  // The rollbacks of the steps the instance reached that registered one:
  // `run()` again, with steps that only read their records, up to the first
  // it did not reach. One that replay did not register again is missing.
  async #replay(m) {
    const counts = new Map();
    const rollbacks = new Map();
    const read = (type, name) => {
      const k = `${type}:${name}`;
      const count = (counts.get(k) ?? 0) + 1;
      counts.set(k, count);
      const key = `s:${type}:${count}:${name}`;
      return { key, count, record: this.#kv.get(key) };
    };
    const end = () => error('WorkflowReplayEnd', 'Replayed every step reached');
    const result = (record) => {
      if (record?.done) return record.value;
      if (record?.error) throw error(record.error.name, record.error.message);
      throw end();
    };
    const step = new Step({
      do: (name, config, callback, rollback) => {
        if (typeof config === 'function') [config, rollback] = [{}, callback];
        const { key, count, record } = read('do', name);
        if (record?.attempts === undefined) throw end();
        if (rollback) {
          const ctx = {
            step: { name, count },
            attempt: record.attempts || 1,
            config: shownConfig(stepConfig(config)),
          };
          rollbacks.set(key, undoOf(rollback, name, ctx, record, key));
        }
        return result(record);
      },
      sleep: (name) => result(read('sleep', name).record),
      waitForEvent: (name) => result(read('waitForEvent', name).record),
    });
    await this.#call(m, step).catch(() => {});
    return this.#history()
      .filter(([, r]) => r.hasRollback)
      .map(([key, { name, count }]) => {
        const missing = { name, ctx: { step: { name, count } }, missing: true };
        return rollbacks.get(key) ?? missing;
      });
  }

  // Rolls back the steps that registered a rollback, then finishes as
  // `status`. Those are the steps this `run()` reached: after a crash, the
  // ones it reached again.
  async #undo(status, fields, failure) {
    if (this.#rollbacks.length > 0) {
      this.#set('rollingBack', { ...fields, after: status });
      this.#release();
      this.#undoing = this.#meta.generation;
      let ok;
      try {
        ok = await this.#rollback(failure);
      } finally {
        this.#undoing = null;
      }
      if (this.#meta?.status !== 'rollingBack') return;
      this.#log(ok ? 'rollback_completed' : 'rollback_errored');
    }
    this.#finish(status, fields);
  }

  // Runs the rollbacks newest first: whether all of them succeeded.
  async #rollback(failure) {
    for (const undo of this.#rollbacks.toReversed()) {
      const { name, count } = undo.ctx.step;
      const stepName = `${name}-${count}`;
      const cfg = stepConfig(undo.config);
      this.#log('rollback_step_started', {
        stepName,
        config: shownConfig(cfg),
      });
      if (undo.missing) {
        const error = {
          name: 'RollbackMissing',
          message: 'Rollback function not available in registry',
        };
        this.#log('rollback_step_errored', { stepName, error });
        return false;
      }
      for (let attempt = 1; ; attempt++) {
        if (this.#meta?.status !== 'rollingBack') return false;
        this.#log('rollback_attempt_started', { stepName, attempt });
        try {
          const { ctx, stream, key } = undo;
          const output = stream
            ? this.#replayStream(key, stepName, stream)
            : undo.output;
          await undo.fn({ ctx, error: failure, output, stepName });
          this.#log('rollback_attempt_completed', { stepName, attempt });
          this.#log('rollback_step_completed', { stepName });
          break;
        } catch (e) {
          const error = plain(e);
          if (nonRetryable(e) || attempt > cfg.retries.limit) {
            this.#log('rollback_attempt_errored', { stepName, attempt, error });
            this.#log('rollback_step_errored', { stepName, error });
            return false;
          }
          let retryDelayMs;
          try {
            retryDelayMs = await this.#delay(cfg, e, undo.ctx, attempt);
          } catch (d) {
            this.#log('rollback_attempt_errored', { stepName, attempt, error });
            this.#log('rollback_step_errored', { stepName, error: plain(d) });
            return false;
          }
          const fields = { stepName, attempt, retryDelayMs, error };
          this.#log('rollback_attempt_errored', fields);
          await this.#wait(clock() + retryDelayMs);
        }
      }
    }
    return true;
  }

  #destroy() {
    this.#dropRollbacks();
    this.ctx.storage.deleteAll();
    this.#table = false;
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
    const keep = this.#config.retention ?? {};
    this.#kv.put('meta', {
      id,
      params,
      created: clock(),
      status: 'queued',
      generation: crypto.randomUUID(),
      retention: {
        success: ms(
          retention?.successRetention ?? keep.success ?? RETENTION_MS
        ),
        error: ms(retention?.errorRetention ?? keep.error ?? RETENTION_MS),
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
    const dev = this.env.LOCAL_DEV && {
      __LOCAL_DEV_STEP_OUTPUTS: this.#outputs(),
    };
    return {
      status: m.status,
      ...dev,
      output: m.output ?? null,
      error: m.error,
    };
  }

  // Each step's output and each event's payload, in the order they came.
  #outputs() {
    const outputs = [];
    for (const [, e] of this.#kv.list({ prefix: 'l:' })) {
      if (e.type === 'step_completed') outputs.push(e.output);
      if (e.type !== 'wait_completed') continue;
      const i = e.stepName.lastIndexOf('-');
      const [name, count] = [e.stepName.slice(0, i), e.stepName.slice(i + 1)];
      outputs.push(
        this.#kv.get(`s:waitForEvent:${count}:${name}`)?.value.payload
      );
    }
    return outputs;
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
      const status =
        this.#drive?.generation === m.generation
          ? 'running'
          : m.dormant !== undefined
            ? 'waiting'
            : 'queued';
      this.#set(status, m.dormant && { dormant: m.dormant + shift });
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
      // The `run()` in memory, if any, may not have reached them all yet.
      const rollbacks = await this.#replay(m);
      if (!this.#live(m.generation)) {
        for (const undo of rollbacks) dispose(undo.fn);
        return;
      }
      this.#dropRollbacks(rollbacks);
      const e = error('Terminated', 'Instance terminated during rollback');
      return this.#undo('terminated', {}, e);
    }
    this.#finish('terminated');
  }

  restart({ from } = {}) {
    const m = this.#meta;
    if (!m) throw notFound();
    const steps = this.#history();
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
    // From a step, the history keeps the start and the steps before it.
    const kept = steps.slice(0, keep).map(([, r]) => `${r.name}-${r.count}`);
    const keeps = (e) =>
      from && (START.has(e.type) || (e.stepName && kept.includes(e.stepName)));
    for (const [key] of steps.slice(keep)) {
      this.#kv.delete(key);
      // A mocked stream, attempt 0, stays.
      this.#sql.exec('DELETE FROM chunks WHERE key = ? AND attempt > 0', key);
    }
    for (const [key] of [...this.#kv.list({ prefix: 'e:' })]) {
      this.#kv.delete(key);
    }
    for (const [key, event] of [...this.#kv.list({ prefix: 'l:' })]) {
      if (!keeps(event)) this.#kv.delete(key);
    }
    const { id, params, created, retention, started } = m;
    this.#kv.put('meta', {
      id,
      params,
      created,
      retention,
      started: from ? started : undefined,
      status: 'queued',
      generation: crypto.randomUUID(),
    });
    if (!from) this.#log('workflow_queued');
    this.#drive = null;
    this.#release();
    this.#wake();
  }

  sendEvent(event) {
    if (!this.#meta) throw notFound();
    // A mocked event type takes no others.
    if (!this.#kv.get(`x:event:${event.type}`)) this.#send(event);
  }

  #send({ type, payload }) {
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
    if (this.#meta?.dormant !== undefined && this.#meta.wakesOn === type)
      this.#kv.put('meta', { ...this.#meta, dormant: undefined });
    this.#wake();
  }

  // The testing API.

  modifier() {
    return new Modifier((...args) => this.#modify(...args));
  }

  // What the modifier set for `kind`, of all steps or of `stepName`.
  #x(kind, stepName) {
    return this.#kv.get(`x:${kind}`) ?? this.#kv.get(`x:${kind}:${stepName}`);
  }

  // What `attempt` of `stepName` does instead of its callback, if mocked:
  // fail, time out or return `{value}`.
  #mocked(stepName, attempt) {
    const e =
      this.#x('error', stepName) ??
      this.#kv.get(`x:error:${stepName}:${attempt}`);
    if (e) throw error(e.name, e.message);
    if (
      this.#x('timeout', stepName) ??
      this.#kv.get(`x:timeout:${stepName}:${attempt}`)
    )
      throw error('WorkflowTimeoutError', 'Execution timed out after 0ms');
    const r = this.#kv.get(`x:result:${stepName}`);
    if (r) return { value: r.stream ? new Stored(r.stream) : r.value };
  }

  async #modify(op, step, arg, times) {
    const at = (s) => `${s.name}-${s.index ?? 1}`;
    const put = (k, v = true) => this.#kv.put(`x:${k}`, v);
    const each = (kind, steps) =>
      steps ? steps.forEach((s) => put(`${kind}:${at(s)}`)) : put(kind);
    // Persistent, or for the next `n` attempts, counted across mocks.
    const attempts = (kind, s, n, v) => {
      if (!n) return put(`${kind}:${at(s)}`, v);
      const first = this.#kv.get(`x:count:${at(s)}`) ?? 1;
      put(`count:${at(s)}`, first + n);
      for (let i = first; i < first + n; i++) put(`${kind}:${at(s)}:${i}`, v);
    };
    const unmocked = (what) => {
      if (this.#kv.get(`x:result:${at(step)}`) === undefined) return;
      throw new Error(
        `[WorkflowIntrospector] Trying to ${what} on step '${step.name}' ` +
          'after mocking its result!'
      );
    };
    switch (op) {
      case 'disableSleeps':
        return each('sleep', step);
      case 'disableRetryDelays':
        return each('delay', step);
      case 'mockStepResult': {
        if (this.#kv.get(`x:result:${at(step)}`) !== undefined) {
          throw new Error(
            `[WorkflowIntrospector] Trying to mock step '${step.name}' ` +
              'multiple times!'
          );
        }
        // Stored as attempt 0 of the step, which no run writes.
        const key = `s:do:${step.index ?? 1}:${step.name}`;
        const stream = arg instanceof ReadableStream && {
          ...(await this.#store(key, 0, arg, {})),
        };
        return put(`result:${at(step)}`, stream ? { stream } : { value: arg });
      }
      case 'mockStepError':
        unmocked('mock error');
        return attempts('error', step, times, {
          name: arg.name,
          message: arg.message,
        });
      case 'forceStepTimeout':
        unmocked('force timeout');
        return attempts('timeout', step, arg, true);
      case 'mockEvent':
        put(`event:${step.type}`);
        return this.#send(step);
      case 'forceEventTimeout':
        return put(`eventTimeout:${at(step)}`);
    }
  }

  // The output of step `name` of `index`, once it has one, or its error.
  async waitForStepResult(name, index = 1) {
    const stepName = `${name}-${index}`;
    const ends = new Set(['step_completed', 'step_errored']);
    for (;;) {
      const end = [...this.#kv.list({ prefix: 'l:' })]
        .map(([, e]) => e)
        .findLast((e) => e.stepName === stepName && ends.has(e.type));
      const record = this.#kv.get(`s:do:${index}:${name}`);
      if (end?.type === 'step_errored') {
        throw error(record.error.name, record.error.message);
      }
      if (end && record?.stream && end.output !== '[REDACTED]') {
        return this.#replayStream(
          `s:do:${index}:${name}`,
          stepName,
          record.stream
        );
      }
      if (end) return end.output;
      await new Promise((wake) => this.#watchers.add(wake));
    }
  }

  async waitForStatus(status) {
    this.#wake();
    for (;;) {
      const now = this.#meta?.status;
      if (now === status) return;
      if (TERMINAL.has(now) && TERMINAL.has(status)) {
        throw new Error(
          `[WorkflowIntrospector] The Workflow instance ${this.ctx.id.name} ` +
            `has reached status '${now}'. This is a finite status that ` +
            'prevents it from ever reaching the expected status of ' +
            `'${status}'.`
        );
      }
      await new Promise((wake) => this.#watchers.add(wake));
    }
  }

  outputOrError(isOutput) {
    const { status, output, error } = this.#meta ?? {};
    if (isOutput && status !== 'complete') {
      throw new Error(
        `Cannot retrieve output: Workflow instance is in status ` +
          `"${status}" but must be "complete" to have an output available`
      );
    }
    if (!isOutput && status !== 'errored') {
      throw new Error(
        `Cannot retrieve error: Workflow instance is in status ` +
          `"${status}" but must be "errored" to have error information ` +
          'available'
      );
    }
    return isOutput ? output : error;
  }

  // For Miniflare's local explorer, which finds instances by object ID.
  getInstanceMetadata() {
    const m = this.#meta;
    if (!m) throw new Error('Engine was never started');
    const createdOn = new Date(m.created).toISOString();
    return { instanceId: m.id, status: STATUS_CODES[m.status], createdOn };
  }

  // Deletes the instance and aborts the object, as the end of a test does.
  async abort(reason = DELETED) {
    this.#destroy();
    await this.ctx.storage.sync();
    this.ctx.abort(reason);
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
            const { stream: key, ...value } = event;
            const stored = key && this.#kv.get(key)?.stream;
            if (stored) {
              value.output = this.#replayStream(key, event.stepName, stored);
            }
            return { done: false, value };
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

  // Ends the `run()` in memory too, whose call this may be: the object
  // aborts, and the binding takes `DELETED` as done.
  async destroy() {
    if (!this.#meta) throw notFound();
    const live = this.#drive;
    this.#destroy();
    if (!live) return;
    await this.ctx.storage.sync();
    this.ctx.abort(DELETED);
  }
}

const body = (message) => workflowError('body', message);
const LOCATIONS = [
  'wnam',
  'weur',
  'enam',
  'eeur',
  'apac',
  'apac-ne',
  'apac-se',
  'oc',
  'sam',
  'afr',
  'me',
];
const positive = (d) =>
  (Number.isInteger(d) && d > 0) || (typeof d === 'string' && duration(d) > 0);

// Checks an entry of an object-form batch.
function checkEntry(e) {
  if (typeof e !== 'object' || e === null || Array.isArray(e)) {
    const type = Array.isArray(e) ? 'array' : e === null ? 'null' : typeof e;
    throw body(`Invalid input: expected object, received ${type}`);
  }
  const { successRetention: ok, errorRetention: failed } = e.retention ?? {};
  if ([ok, failed].some((d) => d !== undefined && !positive(d))) {
    throw body(
      "Duration must be a number or a string in format '{{number}} " +
        "{{unit}}' where unit is second(s), minute(s), etc."
    );
  }
  if (e.locationHint !== undefined && !LOCATIONS.includes(e.locationHint)) {
    const names = LOCATIONS.map((l) => `"${l}"`).join('|');
    throw body(`Invalid option: expected one of ${names}`);
  }
}

// Whether `o` are `subscribe()` options.
function validSubscription(o) {
  if (typeof o !== 'object' || o === null || Array.isArray(o)) return false;
  const { cursor, filter, ...rest } = o;
  return (
    Object.keys(rest).length === 0 &&
    (cursor === undefined || (Number.isSafeInteger(cursor) && cursor >= 0)) &&
    (filter === undefined ||
      (Array.isArray(filter) && filter.every((t) => EVENT_TYPES.has(t))))
  );
}
const invalidId = () =>
  workflowError('instance.invalid_id', 'Instance ID is invalid');
const valid = (id) => typeof id === 'string' && id.length <= 100 && ID.test(id);
const reserved = (id) =>
  ['batch', 'terminate', 'terminateAll'].includes(id) ||
  /^cf_[0-9a-f]{64}$/.test(id);

// A Workflow's API, `ctx.props.workflow` naming it.
export class WorkflowBinding extends WorkerEntrypoint {
  // The Workflow's instances, bound in the engine worker as
  // `workflow:<name>`, apart from the engine's own bindings.
  get #namespace() {
    return this.env[`workflow:${this.ctx.props.workflow}`];
  }

  #instance(id) {
    return this.#namespace.getByName(id);
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
    const session = sessions.get(this.ctx.props.workflow);
    if (session) {
      session.instanceIds.push(id);
      const modifier = this.#instance(id).modifier();
      for (const op of session.operations) await apply(modifier, op);
    }
    await this.#instance(id).create(options.params ?? {}, options.retention);
    return { id };
  }

  // The testing API, for a Workflows engine with a `LOCAL_DEV` binding. The
  // binding's props name it as `binding`.

  #test() {
    if (!this.env.LOCAL_DEV) throw new Error('The testing API is not enabled');
    return this.ctx.props.workflow;
  }

  #session(id) {
    const workflow = this.#test();
    const session = sessions.get(workflow);
    if (session?.id === id) return session;
    throw new Error(
      `Workflow ${JSON.stringify(workflow)} does not have an active ` +
        'introspection session for this introspector.'
    );
  }

  async unsafeGetBindingName() {
    this.#test();
    return this.ctx.props.binding;
  }

  async unsafeStartIntrospection() {
    const workflow = this.#test();
    if (sessions.has(workflow)) {
      throw new Error(
        `Workflow ${JSON.stringify(workflow)} already has an active ` +
          'introspection session for binding ' +
          `${JSON.stringify(this.ctx.props.binding)}.`
      );
    }
    const id = crypto.randomUUID();
    sessions.set(workflow, { id, operations: [], instanceIds: [] });
    return id;
  }

  async unsafeStopIntrospection(id) {
    const workflow = this.#test();
    if (sessions.get(workflow)?.id === id) sessions.delete(workflow);
  }

  async unsafeSetIntrospectionOperations(id, operations) {
    this.#session(id).operations = operations;
  }

  async unsafeGetIntrospectionInstances(id) {
    return this.#session(id).instanceIds;
  }

  async unsafeGetInstanceModifier(id) {
    this.#test();
    return this.#instance(id).modifier();
  }

  async unsafeWaitForStepResult(id, name, index) {
    this.#test();
    return this.#instance(id).waitForStepResult(name, index);
  }

  async unsafeWaitForStatus(id, status) {
    this.#test();
    return this.#instance(id).waitForStatus(status);
  }

  async unsafeGetOutputOrError(id, isOutput) {
    this.#test();
    return this.#instance(id).outputOrError(isOutput);
  }

  // Deletes an instance by its object's ID, as hex: whether it had one.
  async unsafeDeletePersistedInstance(hex) {
    this.#test();
    const ns = this.#namespace;
    const instance = ns.get(ns.idFromString(hex));
    if (!(await instance.exists())) return false;
    await instance.abort().catch(() => {});
    return true;
  }

  async unsafeAbort(id, reason) {
    this.#test();
    await this.#instance(id)
      .abort(reason)
      .catch(() => {});
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
      // As Miniflare does, this form ignores retention and location hints.
      const entries = options.map(({ retention, locationHint, ...o }) => o);
      const { created } = await this.#createAll(entries);
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
      checkEntry({ retention });
      instances = Array.from({ length: count }, () => ({ params, retention }));
    } else if (!Array.isArray(instances)) {
      throw body('Provided argument is invalid');
    } else if (instances.length === 0 || instances.length > MAX_BATCH) {
      throw body('Batch size exceeds maximum allowed');
    }
    for (const e of instances) checkEntry(e);
    return this.#createAll(instances, true);
  }

  // Creates `instances` in order. An id given twice, or that exists, is an
  // error entry: reported with `errors`, else skipped. Any other failure
  // throws.
  async #createAll(instances, errors = false) {
    const out = { created: [], errors: [] };
    const seen = new Set();
    const ids = instances.map((o) => o.id ?? crypto.randomUUID());
    for (const id of ids) {
      if (!valid(id) || (errors && reserved(id))) {
        throw errors
          ? invalidId()
          : error('WorkflowError', 'Workflow instance has invalid id');
      }
    }
    for (const [index, options] of instances.entries()) {
      const id = ids[index];
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
    await this.#destroy(id);
  }

  #destroy(id) {
    return this.#instance(id)
      .destroy()
      .catch((e) => {
        if (!String(e?.message).includes(DELETED)) throw e;
      });
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
      unique.map((id) => this.#destroy(id))
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
    const instance = await this.#get(id);
    if (!validSubscription(options)) {
      throw new Error('Invalid Workflow subscription options');
    }
    return instance.subscribe(options);
  }
}

export default {
  async fetch(request) {
    const url = new URL(request.url);
    if (env.TEST_CLOCK && url.pathname === '/clock') {
      const advance = Number(url.searchParams.get('advance'));
      if (!Number.isFinite(advance)) return new Response(null, { status: 400 });
      skew += advance;
      return Response.json({ now: clock() });
    }
    return new Response('Not Found', { status: 404 });
  },
};
