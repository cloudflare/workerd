// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The cron module's parser, against fixtures of Cloudflare's dialect
// (celld's tests/dst/src/cron_units.rs), and its plan.

import assert from 'node:assert';
import { next, parse, plan } from 'cron.js';

const at = (y, mo, d, h = 0, mi = 0) => Date.UTC(y, mo - 1, d, h, mi);
const after = (expression, from) => next(parse(expression), from);
const test = (f) => ({ test: f });

const NOON = at(2026, 10, 15, 12); // a Thursday

export const arming = test(() => {
  const p = plan([parse('0 * * * *')], undefined, NOON + 1, 0, false);
  assert.deepStrictEqual(p, {
    crons: [],
    armAt: NOON + 3_600_000,
    armIsRetry: false,
  });
});

export const aFailureRetriesBeforeTheNextOccurrence = test(() => {
  const crons = [parse('0 * * * *'), parse('0 12 * * *')];
  assert.deepStrictEqual(plan(crons, NOON, NOON + 10, 0, true), {
    crons: ['0 * * * *', '0 12 * * *'],
    armAt: NOON + 10 + 2_000,
    armIsRetry: true,
  });
});

export const aRetryNeverDelaysTheNextOccurrence = test(() => {
  const p = plan([parse('* * * * *')], NOON, NOON + 59_000, 0, true);
  assert.strictEqual(p.armAt, NOON + 60_000);
  assert.strictEqual(p.armIsRetry, false);
});

export const retriesStopAtTheCeiling = test(() => {
  const p = plan([parse('0 * * * *')], NOON, NOON + 10, 6, true);
  assert.strictEqual(p.armAt, NOON + 3_600_000);
  assert.strictEqual(p.armIsRetry, false);
});

export const expressionsShareOneDeadline = test(() => {
  const crons = ['0 12 * * *', '*/5 * * * *', '0 13 * * *'].map(parse);
  const noon = at(2026, 8, 15, 12);
  assert.strictEqual(
    plan(crons, undefined, noon - 120_000, 0, false).armAt,
    noon
  );
  const due = (t) => plan(crons, t, t, 0, false).crons;
  assert.deepStrictEqual(due(noon), ['0 12 * * *', '*/5 * * * *']);
  assert.deepStrictEqual(due(noon + 300_000), ['*/5 * * * *']);
  assert.deepStrictEqual(due(noon + 360_000), []);
  assert.strictEqual(plan([], undefined, noon, 0, false).armAt, null);
});

// Strictly after, even from inside the minute.
export const anOccurrenceNeverReturnsItself = test(() => {
  const noon = at(2026, 8, 15, 12);
  assert.strictEqual(after('0 12 * * *', noon), at(2026, 8, 16, 12));
  assert.strictEqual(after('0 12 * * *', noon + 30_000), at(2026, 8, 16, 12));
  assert.strictEqual(after('* * * * *', noon), noon + 60_000);
});

export const stepsListsAndRanges = test(() => {
  const from = at(2026, 8, 15, 12, 46);
  assert.strictEqual(after('*/15 * * * *', from), at(2026, 8, 15, 13));
  // `30/15` runs from 30 to the end of the field, and `5/1` from 5.
  assert.strictEqual(after('30/15 * * * *', from), at(2026, 8, 15, 13, 30));
  const half = at(2026, 8, 15, 12, 31);
  assert.strictEqual(after('30/15 * * * *', half), at(2026, 8, 15, 12, 45));
  assert.strictEqual(after('5/1 * * * *', from), at(2026, 8, 15, 12, 47));
  assert.strictEqual(
    after('0 9-11,17 * * *', at(2026, 8, 15, 11)),
    at(2026, 8, 15, 17)
  );
  assert.strictEqual(parse('  */5 * * * *  ').text, '*/5 * * * *');
});

// Cloudflare numbers the weekdays 1 to 7 from Sunday and rejects 0.
export const weekdaysCountFromSunday = test(() => {
  const from = at(2026, 8, 15); // a Saturday
  assert.strictEqual(after('0 0 * * 1', from), at(2026, 8, 16));
  assert.strictEqual(after('0 0 * * 2', from), at(2026, 8, 17));
  assert.strictEqual(after('0 0 * * 7', from), at(2026, 8, 22));
  assert.strictEqual(after('0 0 * JAN mon', from), after('0 0 * 1 2', from));
  assert.strictEqual(after('0 0 * * 2-6', from), at(2026, 8, 17));
});

// With both day fields restricted, a day matches when either does.
export const twoRestrictedDayFieldsAreAUnion = test(() => {
  const from = at(2026, 8, 20);
  assert.strictEqual(after('0 0 1 9 mon', from), at(2026, 9, 1));
  assert.strictEqual(after('0 0 1 9 mon', at(2026, 9, 1)), at(2026, 9, 7));
  assert.strictEqual(after('0 0 * 9 mon', from), at(2026, 9, 7));
});

export const aLeapDayWaitsEightYearsAcrossASkippedCentury = test(() => {
  assert.strictEqual(after('0 0 29 2 *', at(2026, 8, 15)), at(2028, 2, 29));
  assert.strictEqual(after('0 0 29 2 *', at(2096, 3, 1)), at(2104, 2, 29));
  assert.strictEqual(
    after('0 0 1 * *', at(2026, 8, 15, 23, 59)),
    at(2026, 9, 1)
  );
});

export const theLWAndHashForms = test(() => {
  // August 2026 ends on Monday the 31st; February 2027 has 28 days.
  assert.strictEqual(after('0 0 L * *', at(2026, 8, 15)), at(2026, 8, 31));
  assert.strictEqual(after('0 0 L-3 * *', at(2026, 8, 15)), at(2026, 8, 28));
  assert.strictEqual(after('0 0 L * *', at(2027, 2, 1)), at(2027, 2, 28));
  assert.strictEqual(after('0 0 L-3W * *', at(2026, 8, 1)), at(2026, 8, 28));
  // October 2026 ends on Saturday the 31st.
  assert.strictEqual(after('0 0 LW * *', at(2026, 10, 1)), at(2026, 10, 30));
  // `L-7W` is Saturday the 24th, so Friday the 23rd and not the Monday too.
  assert.strictEqual(after('0 0 L-7W * *', at(2026, 10, 1)), at(2026, 10, 23));
  assert.strictEqual(after('0 0 L-7W * *', at(2026, 10, 23)), at(2026, 11, 23));
  // April 2027 ends on Friday the 30th but has no 31st, so `31W` waits for
  // May's, a Monday; January 2027's 31st is a Sunday, so it runs on Friday.
  assert.strictEqual(after('0 0 31W * *', at(2027, 4, 1)), at(2027, 5, 31));
  assert.strictEqual(after('0 0 31W * *', at(2027, 1, 1)), at(2027, 1, 29));
  // `L-1W` is Sunday the 29th of November 2026, so Monday the 30th; `L-30W`
  // is Saturday the 1st of August 2026, so Monday the 3rd.
  assert.strictEqual(after('0 0 L-1W * *', at(2026, 11, 1)), at(2026, 11, 30));
  assert.strictEqual(after('0 0 L-30W * *', at(2026, 7, 20)), at(2026, 8, 3));
  // A target before the 1st matches nothing: February 2028 has a 29th.
  assert.strictEqual(after('0 0 L-28W 2 *', at(2027, 1, 1)), at(2028, 2, 1));
  // The 15th of November 2026 is a Sunday.
  assert.strictEqual(after('0 0 15W * *', at(2026, 11, 1)), at(2026, 11, 16));
  // The 1st of August 2026 is a Saturday: `1W` stays in August.
  assert.strictEqual(after('0 0 1W * *', at(2026, 7, 20)), at(2026, 8, 3));
  // September 2026: Mondays are the 7th, 14th, 21st and 28th.
  assert.strictEqual(after('0 0 * * MONL', at(2026, 9, 1)), at(2026, 9, 28));
  // August 2026 ends on a Monday, a week after the one before.
  assert.strictEqual(after('0 0 * * MONL', at(2026, 8, 1)), at(2026, 8, 31));
  assert.strictEqual(after('0 0 * * MON#2', at(2026, 9, 1)), at(2026, 9, 14));
  assert.strictEqual(after('0 0 * * MON#5', at(2026, 9, 1)), at(2026, 11, 30));
  // A bare `L` in the weekday field is Saturday.
  assert.strictEqual(after('0 0 * * L', at(2026, 8, 16)), at(2026, 8, 22));
});

export const whatCloudflareRefusesIsRefused = test(() => {
  for (const expression of [
    '',
    '* * * *',
    '* * * * * *',
    '0 0 ? * *',
    '0 0 * * 0',
    '60 * * * *',
    '* 24 * * *',
    '* * 32 * *',
    '* * * 13 *',
    '* * * 0 *',
    '* * * * 8',
    '* * * * mno',
    '*/0 * * * *',
    '*/60 * * * *',
    '0 0 * * +2',
    '0 0 1W,15 * *',
    '0 0 * * 3,L',
    '0 0 * * MON#6',
    '0 0 * * 1#2#3',
    '0 0 L-31 * *',
    '0 0 L-29W 2 *',
    '0 0 30 2 *',
    // Saffron accepts these, with readings refused here.
    '30-10 * * * *',
    '0,30-10 * * * *',
    '5,* * * * *',
  ]) {
    assert.throws(
      () => parse(expression),
      /^Error: cron trigger "/,
      expression
    );
  }
});
