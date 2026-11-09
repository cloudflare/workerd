// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Cron triggers: each schedule is a Durable Object whose alarm is the whole
// scheduler, so an occurrence runs once however many instances of a cluster
// run the config, and the alarm fails over with the object. An occurrence
// counts as run once its handler returns: at least once.
//
// The config that runs this module (`bundled = "/workerd/cron.js"`) is an
// interface: cli/wrangler.rs and hand-written configs write it, each for the
// workerd it runs on. Anything added to it later must be optional. It is:
//
//   - a Worker with RPC and the `service_binding_extra_handlers` flag;
//   - the Durable Object class `Schedule`, SQLite-backed (`enableSql`),
//     stored on a local disk;
//   - these bindings:
//
//   SCHEDULE  the `Schedule` class
//   CRONS     JSON: `{"<binding>": ["<expression>", ...]}`, where each
//             `<binding>` is also a service binding to the Worker whose
//             `scheduled()` handler the expressions run, and names its
//             schedule
//
// workerd sends a Worker using this module one scheduled event at startup,
// which arms every schedule: an object has no client of its own to wake it.
// The schedule is the config's, not the object's, so a changed expression
// takes effect at the next arm and migrates nothing.
//
// Expressions are Cloudflare's dialect, in UTC, as its parser (saffron)
// reads them: weekdays 1 to 7 from Sunday, so `1-5` is Sunday to Thursday,
// where Vixie cron reads Monday to Friday; `L`, `W` and `#` in the day
// fields; with both day fields restricted, a day matches when either does.
// Two of saffron's readings are refused instead of copied: a descending
// range, which it wraps one value too far (`SAT-SUN` matches Friday), and
// `*` inside a list. An expression that never matches is refused too.

import { DurableObject, env } from 'cloudflare:workers';

const MINUTE_MS = 60_000;
const DAY_MS = 86_400_000;
// One Gregorian cycle: the calendar repeats every 400 years, so a walk this
// long sees every (month, day, weekday) there is.
const LOOKAHEAD_DAYS = 146_097;
// A failed occurrence is retried up to RETRIES times, the nth retry
// BACKOFF_MS * 2 ** (n - 1) after the attempt before it failed.
const RETRIES = 6;
const BACKOFF_MS = 2_000;

const field = (name, min, max, names = []) => ({ name, min, max, names });
const MINUTE = field('minute', 0, 59);
const HOUR = field('hour', 0, 23);
const DAY_OF_MONTH = field('day of month', 1, 31);
const MONTH = field('month', 1, 12, [
  'jan',
  'feb',
  'mar',
  'apr',
  'may',
  'jun',
  'jul',
  'aug',
  'sep',
  'oct',
  'nov',
  'dec',
]);
const DAY_OF_WEEK = field('day of week', 1, 7, [
  'sun',
  'mon',
  'tue',
  'wed',
  'thu',
  'fri',
  'sat',
]);

const digits = (text) => (/^\d+$/.test(text) ? Number(text) : undefined);
const weekend = (weekday) => weekday === 0 || weekday === 6;

function value(text, f) {
  const name = f.names.indexOf(text.toLowerCase());
  const v = name >= 0 ? f.min + name : digits(text);
  if (v === undefined) throw new Error(`${f.name} field has ${text}`);
  if (v < f.min || v > f.max) {
    throw new Error(`${f.name} field has ${v} outside ${f.min}-${f.max}`);
  }
  return v;
}

// A list of values, ranges and steps, as the set of values it names.
function list(text, f) {
  const set = new Set();
  const items = text.split(',');
  for (const item of items) {
    if (item === '*' && items.length > 1) {
      throw new Error(`${f.name} field has * inside a list`);
    }
    const [spec, stepText, extra] = item.split('/');
    const step = stepText === undefined ? 1 : digits(stepText);
    if (extra !== undefined || !(step >= 1 && step <= f.max - f.min)) {
      throw new Error(`${f.name} field has a bad step in ${item}`);
    }
    let [first, last] = [f.min, f.max];
    if (spec !== '*') {
      const [from, to, more] = spec.split('-');
      if (more !== undefined) throw new Error(`${f.name} field has ${spec}`);
      first = value(from, f);
      last = to !== undefined ? value(to, f) : step > 1 ? f.max : first;
    }
    if (first > last) {
      throw new Error(`${f.name} field has a descending range in ${item}`);
    }
    for (let v = first; v <= last; v += step) set.add(v);
  }
  return set;
}

// The day fields, as predicates of (day, weekday with Sunday 0, last day of
// the month), or null for `*`. `W` moves off a weekend to the nearest
// weekday without leaving the month; each arm is saffron's.
function dayOfMonth(text) {
  if (text === '*') return null;
  if (text === 'L') return (d, w, last) => d === last;
  if (text === 'LW') {
    return (d, w, last) =>
      (!weekend(w) && d === last) || (w === 5 && last - d < 3);
  }
  const offset = /^L-(\d+)(W?)$/.exec(text);
  if (offset !== null) {
    const n = Number(offset[1]);
    if (n < 1 || n > 30) throw new Error(`day of month field has ${text}`);
    if (offset[2] === '') return (d, w, last) => d + n === last;
    return (d, w, last) =>
      (!weekend(w) && d + n === last) ||
      (w === 1 && d + n === last + 1) ||
      (w === 1 && d === 3 && last - n === 1) ||
      (w === 5 && d + n + 1 === last);
  }
  if (text.endsWith('W')) {
    const t = value(text.slice(0, -1), DAY_OF_MONTH);
    // A day past the month's end matches nothing in that month.
    return (d, w, last) =>
      t <= last &&
      ((!weekend(w) && d === t) ||
        (w === 1 && d - 1 === t) ||
        (w === 1 && d === 3 && t === 1) ||
        (w === 5 && d + 1 === t) ||
        (w === 5 && d + 2 === t && t === last));
  }
  const days = list(text, DAY_OF_MONTH);
  return (d) => days.has(d);
}

// A bare `L` is Saturday: saffron reads it as day 7.
function dayOfWeek(text) {
  if (text === '*') return null;
  if (text === 'L') return (d, w) => w === 6;
  if (text.endsWith('L')) {
    const want = value(text.slice(0, -1), DAY_OF_WEEK) - 1;
    return (d, w, last) => w === want && d + 7 > last;
  }
  if (text.includes('#')) {
    const [day, nth, more] = text.split('#');
    const want = value(day, DAY_OF_WEEK) - 1;
    const n = digits(nth);
    if (more !== undefined || !(n >= 1 && n <= 5)) {
      throw new Error(`day of week field has ${text}`);
    }
    return (d, w) => w === want && Math.floor((d - 1) / 7) + 1 === n;
  }
  const days = list(text, DAY_OF_WEEK);
  return (d, w) => days.has(w + 1);
}

export function parse(expression) {
  const text = expression.trim();
  try {
    const f = text.split(/\s+/);
    if (f.length !== 5) throw new Error(`${f.length} fields, not 5`);
    const cron = {
      text,
      minute: list(f[0], MINUTE),
      hour: list(f[1], HOUR),
      dom: dayOfMonth(f[2]),
      month: list(f[3], MONTH),
      dow: dayOfWeek(f[4]),
    };
    if (next(cron, 0) === null) throw new Error('it never matches');
    return cron;
  } catch (e) {
    throw new Error(`cron trigger "${text}": ${e.message}`);
  }
}

function dayMatches(c, day) {
  const date = new Date(day * DAY_MS);
  const [y, m] = [date.getUTCFullYear(), date.getUTCMonth()];
  if (!c.month.has(m + 1)) return false;
  const args = [
    date.getUTCDate(),
    date.getUTCDay(),
    new Date(Date.UTC(y, m + 1, 0)).getUTCDate(),
  ];
  if (c.dom && c.dow) return c.dom(...args) || c.dow(...args);
  return (c.dom ?? c.dow)?.(...args) ?? true;
}

const timeMatches = (c, t) =>
  c.hour.has(Math.floor(t / 60)) && c.minute.has(t % 60);

function matches(c, at) {
  const minute = Math.floor(at / MINUTE_MS);
  return (
    dayMatches(c, Math.floor(minute / 1440)) && timeMatches(c, minute % 1440)
  );
}

// The first occurrence strictly after `after`, or null: an occurrence that
// returned itself would arm an alarm already due.
export function next(c, after) {
  const start = Math.floor(after / MINUTE_MS) + 1;
  let day = Math.floor(start / 1440);
  for (let t = start % 1440, end = day + LOOKAHEAD_DAYS; day < end; day++) {
    if (dayMatches(c, day)) {
      for (; t < 1440; t++)
        if (timeMatches(c, t)) return (day * 1440 + t) * MINUTE_MS;
    }
    t = 0;
  }
  return null;
}

// What a schedule does after an occurrence (`fired`) or an arm: `crons`, the
// expressions the occurrence owes; `armAt`, or null when nothing matches
// again; and whether that is a retry of the expressions that failed. A
// retry and the next occurrence compete for one alarm, and the occurrence
// wins whenever it is sooner, so a failing cron never delays the next run.
export function plan(crons, fired, now, retry, failed) {
  const due = fired === undefined ? [] : crons.filter((c) => matches(c, fired));
  const nexts = crons.map((c) => next(c, now)).filter((t) => t !== null);
  const occurrence = nexts.length > 0 ? Math.min(...nexts) : null;
  const retryAt =
    failed && retry < RETRIES ? now + BACKOFF_MS * 2 ** retry : null;
  const armAt =
    occurrence === null || retryAt === null
      ? (occurrence ?? retryAt)
      : Math.min(occurrence, retryAt);
  return {
    crons: due.map((c) => c.text),
    armAt,
    armIsRetry: armAt !== null && armAt !== occurrence,
  };
}

// The schedules, parsed once: a bad expression, or a name with no service
// binding to run, fails the config when it loads.
const schedules = Object.fromEntries(
  Object.entries(env.CRONS ?? {}).map(([name, expressions]) => {
    if (typeof env[name]?.scheduled !== 'function') {
      throw new Error(
        `cron triggers of "${name}": no service binding "${name}"`
      );
    }
    return [name, expressions.map(parse)];
  })
);
const crons = (name) => schedules[name] ?? [];

export default {
  async scheduled(controller, env) {
    await Promise.all(
      Object.keys(schedules).map((name) =>
        env.SCHEDULE.get(env.SCHEDULE.idFromName(name)).arm(name)
      )
    );
  },
};

export class Schedule extends DurableObject {
  // What a retry owes, in memory: an eviction costs a retry, never a run.
  #owed = null;
  #retry = 0;

  // Arms the plan's deadline, or retires the object when nothing matches.
  async #arm(name, p) {
    const storage = this.ctx.storage;
    if (!p.armIsRetry) this.#owed = null;
    if (p.armAt === null) {
      await storage.deleteAlarm();
      await storage.delete(['name', 'at', 'retry']);
      return;
    }
    await storage.put({ name, at: p.armAt, retry: p.armIsRetry });
    await storage.setAlarm(p.armAt);
  }

  async arm(name) {
    const p = plan(crons(name), undefined, Date.now(), this.#retry, false);
    // An alarm no later than the plan's, a retry or a missed occurrence, is
    // kept.
    const alarm = await this.ctx.storage.getAlarm();
    if (alarm !== null && p.armAt !== null && alarm <= p.armAt) return;
    await this.#arm(name, p);
  }

  async alarm() {
    const stored = await this.ctx.storage.get(['name', 'at', 'retry']);
    const name = stored.get('name');
    if (name === undefined) return;
    const schedule = crons(name);
    let { occurrence, due } = this.#owed ?? {};
    if (!stored.get('retry')) {
      occurrence = stored.get('at');
      this.#retry = 0;
      due = plan(schedule, occurrence, Date.now(), 0, false).crons;
    }
    const failed = [];
    for (const cron of due ?? []) {
      try {
        const { outcome, noRetry } = await this.env[name].scheduled({
          scheduledTime: new Date(occurrence),
          cron,
        });
        if (outcome !== 'ok' && !noRetry) failed.push(cron);
      } catch (e) {
        console.error(`cron "${cron}" of "${name}": ${e}`);
        failed.push(cron);
      }
    }
    // A schedule no longer in the config has no expressions, and retires.
    const p = plan(
      schedule,
      undefined,
      Date.now(),
      this.#retry,
      failed.length > 0
    );
    if (failed.length > 0) {
      this.#owed = { occurrence, due: failed };
      this.#retry++;
    } else {
      this.#retry = 0;
    }
    await this.#arm(name, p);
  }
}
