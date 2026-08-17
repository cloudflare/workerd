// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Coverage for the Temporal API surface.
//
// Assertions about Temporal's own behaviour are derived from fixed literals
// rather than the current time, so they stay deterministic.
//
// Temporal.Now is the exception, and is covered separately below. Its results
// cannot be pinned to a literal, but they can be pinned to Date.now(): the two
// are required to read the same clock, so bracketing one with the other is
// exact without depending on what time it actually is.

import { deepStrictEqual, ok, strictEqual, throws } from 'node:assert';

// 2024-02-29T12:34:56.789012345Z. A leap day, so it also exercises
// `inLeapYear`, `daysInMonth` and February-overflow behaviour.
const REF_NS = 1709210096789012345n;
const REF_MS = 1709210096789;

// Midnight UTC on the same day, used as the rounding/startOfDay target.
const REF_DAY_MS = 1709164800000;

const isoDate = () => Temporal.PlainDate.from('2024-02-29');
const isoTime = () => Temporal.PlainTime.from('12:34:56.789012345');
const isoDateTime = () =>
  Temporal.PlainDateTime.from('2024-02-29T12:34:56.789012345');
const isoYearMonth = () => Temporal.PlainYearMonth.from('2024-02');
const isoMonthDay = () => Temporal.PlainMonthDay.from('02-29');
const utcInstant = () => Temporal.Instant.fromEpochNanoseconds(REF_NS);
const utcZoned = () => new Temporal.ZonedDateTime(REF_NS, 'UTC');

// `toLocaleString` output depends on the bundled ICU and CLDR versions, so
// assert only that each class produces a non-empty string rather than pinning
// text that shifts on every ICU bump.
// PlainYearMonth and PlainMonthDay must be passed an explicit
// `{ calendar: 'iso8601' }`; with no options they reject their own iso8601
// values against the locale's default calendar. Which combinations are
// rejected is Intl behaviour rather than anything workerd controls, so it is
// left to test262 and only the working combination is exercised here.
function assertLocaleString(value, options) {
  const formatted =
    options === undefined
      ? value.toLocaleString()
      : value.toLocaleString('en-US', options);
  strictEqual(typeof formatted, 'string');
  ok(formatted.length > 0);
}

// Every Temporal type is deliberately non-primitive-coercible so that `+` and
// `<` cannot silently produce nonsense.
function assertNotCoercible(value) {
  throws(() => value.valueOf(), TypeError);
  throws(() => value + 1, TypeError);
}

export const namespace = {
  test() {
    strictEqual(typeof Temporal, 'object');
    strictEqual(Temporal[Symbol.toStringTag], 'Temporal');

    // Temporal is a namespace, not a constructor.
    throws(() => new Temporal(), TypeError);

    for (const name of [
      'Duration',
      'Instant',
      'PlainDate',
      'PlainDateTime',
      'PlainMonthDay',
      'PlainTime',
      'PlainYearMonth',
      'ZonedDateTime',
    ]) {
      strictEqual(typeof Temporal[name], 'function', name);
    }
    strictEqual(typeof Temporal.Now, 'object');
    strictEqual(Temporal.Now[Symbol.toStringTag], 'Temporal.Now');
  },
};

export const now = {
  test() {
    ok(Temporal.Now.instant() instanceof Temporal.Instant);
    ok(Temporal.Now.plainDateTimeISO() instanceof Temporal.PlainDateTime);
    ok(Temporal.Now.plainDateISO() instanceof Temporal.PlainDate);
    ok(Temporal.Now.plainTimeISO() instanceof Temporal.PlainTime);
    ok(Temporal.Now.zonedDateTimeISO() instanceof Temporal.ZonedDateTime);

    const zone = Temporal.Now.timeZoneId();
    strictEqual(typeof zone, 'string');
    ok(zone.length > 0);

    strictEqual(Temporal.Now.plainDateTimeISO('UTC').calendarId, 'iso8601');
    strictEqual(Temporal.Now.zonedDateTimeISO('UTC').timeZoneId, 'UTC');
    strictEqual(
      Temporal.Now.zonedDateTimeISO('America/New_York').timeZoneId,
      'America/New_York'
    );
  },
};

// Reads a clock via `read`, bracketed by `Date.now()`, and asserts the result
// falls inside the bracket.
//
// The bracket is required rather than decorative. workerd's TimerChannel
// returns kj::systemPreciseCalendarClock().now() directly, so the clock keeps
// advancing and two consecutive reads can land in different milliseconds.
// Asserting equality against a single Date.now() would fail whenever a read
// straddled a millisecond boundary.
//
// Bracketing still pins the property that matters: both clocks must be the same
// clock. A Temporal reading sourced from anywhere else lands far outside a
// window this narrow — in cloudflare/workerd#6907 it reported 1970.
//
// Do not replace this with a single equality check. Under the clamped,
// Spectre-mitigated TimerChannel that production uses, time does hold still
// across a synchronous block and equality would appear to work; it is this
// unclamped test-time clock that makes it flaky.
function assertMatchesDateNow(read) {
  const before = Date.now();
  const observed = read();
  const after = Date.now();

  ok(
    observed >= before && observed <= after,
    `${observed} outside [${before}, ${after}]`
  );
  if (before === after) {
    strictEqual(observed, before);
  }
}

export const nowMatchesDateNow = {
  test() {
    // Temporal.Now must read the same clock as Date.now(). Every method is
    // reduced to an epoch-milliseconds reading so it can be bracketed
    // directly, which also avoids any midnight-rollover edge case.
    assertMatchesDateNow(() => Temporal.Now.instant().epochMilliseconds);

    assertMatchesDateNow(
      () => Temporal.Now.zonedDateTimeISO('UTC').epochMilliseconds
    );

    assertMatchesDateNow(
      () =>
        Temporal.Now.plainDateTimeISO('UTC').toZonedDateTime('UTC')
          .epochMilliseconds
    );

    // plainDateISO and plainTimeISO each carry only half the information, so
    // recombine them into a single instant and bracket that.
    assertMatchesDateNow(
      () =>
        Temporal.Now.plainDateISO('UTC').toZonedDateTime({
          timeZone: 'UTC',
          plainTime: Temporal.Now.plainTimeISO('UTC'),
        }).epochMilliseconds
    );

    // Date.prototype.toTemporalInstant is the documented bridge between the
    // two APIs, so it must agree too.
    assertMatchesDateNow(
      () => new Date().toTemporalInstant().epochMilliseconds
    );
  },
};

export const nowIsCoarse = {
  test() {
    // Date.now() has millisecond granularity, and Temporal must not expose
    // anything finer. Sub-millisecond timers are a Spectre timing-attack
    // vector, so this asserts the absence of precision rather than the
    // presence of a value: the nanosecond reading has to be a whole number of
    // milliseconds.
    strictEqual(Temporal.Now.instant().epochNanoseconds % 1_000_000n, 0n);
    strictEqual(
      Temporal.Now.zonedDateTimeISO('UTC').epochNanoseconds % 1_000_000n,
      0n
    );

    const t = Temporal.Now.plainTimeISO('UTC');
    strictEqual(t.microsecond, 0);
    strictEqual(t.nanosecond, 0);

    // The epoch-nanoseconds reading must be exactly Date.now() scaled up, with
    // no residue in the low digits.
    const before = Date.now();
    const observed = Temporal.Now.instant().epochNanoseconds;
    const after = Date.now();
    if (before === after) {
      strictEqual(observed, BigInt(before) * 1_000_000n);
    }
  },
};

export const nowIsNotTheEpoch = {
  test() {
    // Guards cloudflare/workerd#6907 directly: Temporal was shipped reading a
    // clock stuck at the Unix epoch, so Temporal.Now.instant() reported
    // 1970-01-01 while Date.now() reported real time. Applications that guard
    // a polyfill with `typeof Temporal === 'undefined'` silently began
    // computing against 1970 — in that incident, minting JWTs with iat=0.
    //
    // Any clock that is merely *stuck* rather than wrong-by-a-constant would
    // also be caught here, since Date.now() inside a request is always well
    // past this bound.
    const instant = Temporal.Now.instant();
    ok(instant.epochMilliseconds > 1_700_000_000_000, instant.toString());
    ok(Temporal.Now.plainDateISO('UTC').year >= 2023);
  },
};

export const instant = {
  test() {
    // Construction.
    strictEqual(new Temporal.Instant(REF_NS).epochNanoseconds, REF_NS);
    strictEqual(Temporal.Instant.from(utcInstant()).epochNanoseconds, REF_NS);
    strictEqual(
      Temporal.Instant.from('2024-02-29T12:34:56.789012345Z').epochNanoseconds,
      REF_NS
    );
    strictEqual(
      Temporal.Instant.fromEpochMilliseconds(REF_MS).epochMilliseconds,
      REF_MS
    );
    strictEqual(
      Temporal.Instant.fromEpochNanoseconds(REF_NS).epochNanoseconds,
      REF_NS
    );

    // Offsets are accepted and normalised to UTC.
    strictEqual(
      Temporal.Instant.from('2024-02-29T07:34:56.789012345-05:00')
        .epochNanoseconds,
      REF_NS
    );

    // Properties.
    const i = utcInstant();
    strictEqual(i.epochMilliseconds, REF_MS);
    strictEqual(i.epochNanoseconds, REF_NS);

    // Arithmetic.
    strictEqual(i.add({ hours: 1 }).epochNanoseconds, REF_NS + 3600000000000n);
    strictEqual(
      i.subtract({ hours: 1 }).epochNanoseconds,
      REF_NS - 3600000000000n
    );
    strictEqual(
      Temporal.Instant.fromEpochMilliseconds(0)
        .until(Temporal.Instant.fromEpochMilliseconds(1000))
        .toString(),
      'PT1S'
    );
    strictEqual(
      Temporal.Instant.fromEpochMilliseconds(1000)
        .since(Temporal.Instant.fromEpochMilliseconds(0))
        .toString(),
      'PT1S'
    );

    // Rounding.
    strictEqual(
      i.round({ smallestUnit: 'hour', roundingMode: 'floor' })
        .epochMilliseconds,
      REF_DAY_MS + 12 * 3600000
    );
    strictEqual(
      i.round({ smallestUnit: 'second', roundingMode: 'trunc' })
        .epochMilliseconds,
      1709210096000
    );

    // Comparison.
    ok(i.equals(utcInstant()));
    ok(!i.equals(Temporal.Instant.fromEpochMilliseconds(0)));
    strictEqual(
      Temporal.Instant.compare(
        Temporal.Instant.fromEpochMilliseconds(0),
        Temporal.Instant.fromEpochMilliseconds(1)
      ),
      -1
    );
    strictEqual(Temporal.Instant.compare(i, utcInstant()), 0);
    strictEqual(
      Temporal.Instant.compare(
        Temporal.Instant.fromEpochMilliseconds(1),
        Temporal.Instant.fromEpochMilliseconds(0)
      ),
      1
    );

    // Serialization.
    strictEqual(i.toString(), '2024-02-29T12:34:56.789012345Z');
    strictEqual(i.toJSON(), '2024-02-29T12:34:56.789012345Z');
    strictEqual(JSON.parse(JSON.stringify({ i })).i, i.toString());
    strictEqual(i.toString({ smallestUnit: 'minute' }), '2024-02-29T12:34Z');
    strictEqual(
      i.toString({ timeZone: 'America/New_York' }),
      '2024-02-29T07:34:56.789012345-05:00'
    );
    assertLocaleString(i);
    assertNotCoercible(i);

    // Conversion.
    const z = i.toZonedDateTimeISO('UTC');
    ok(z instanceof Temporal.ZonedDateTime);
    strictEqual(z.epochNanoseconds, REF_NS);
    strictEqual(z.timeZoneId, 'UTC');

    strictEqual(i[Symbol.toStringTag], 'Temporal.Instant');
  },
};

export const plainDate = {
  test() {
    // Construction.
    strictEqual(new Temporal.PlainDate(2024, 2, 29).toString(), '2024-02-29');
    strictEqual(Temporal.PlainDate.from('2024-02-29').toString(), '2024-02-29');
    strictEqual(
      Temporal.PlainDate.from({ year: 2024, month: 2, day: 29 }).toString(),
      '2024-02-29'
    );
    strictEqual(
      Temporal.PlainDate.from({
        year: 2024,
        monthCode: 'M02',
        day: 29,
      }).toString(),
      '2024-02-29'
    );

    // Overflow handling is part of the documented `from` contract.
    strictEqual(
      Temporal.PlainDate.from({ year: 2023, month: 2, day: 31 }).toString(),
      '2023-02-28'
    );
    throws(
      () =>
        Temporal.PlainDate.from(
          { year: 2023, month: 2, day: 31 },
          { overflow: 'reject' }
        ),
      RangeError
    );

    const d = isoDate();

    // Calendar.
    strictEqual(d.calendarId, 'iso8601');

    // Year-related. The ISO calendar has no eras.
    strictEqual(d.era, undefined);
    strictEqual(d.eraYear, undefined);
    strictEqual(d.year, 2024);
    strictEqual(d.inLeapYear, true);
    strictEqual(d.monthsInYear, 12);
    strictEqual(d.daysInYear, 366);

    // Month-related.
    strictEqual(d.month, 2);
    strictEqual(d.monthCode, 'M02');
    strictEqual(d.daysInMonth, 29);

    // Week-related. 2024-02-29 falls in ISO week 9.
    strictEqual(d.weekOfYear, 9);
    strictEqual(d.yearOfWeek, 2024);
    strictEqual(d.daysInWeek, 7);

    // Day-related. 2024-02-29 is a Thursday and the 60th day of the year.
    strictEqual(d.day, 29);
    strictEqual(d.dayOfWeek, 4);
    strictEqual(d.dayOfYear, 60);

    // Updaters.
    strictEqual(d.with({ day: 1 }).toString(), '2024-02-01');
    strictEqual(d.with({ year: 2023 }).toString(), '2023-02-28');
    const gregory = d.withCalendar('gregory');
    strictEqual(gregory.calendarId, 'gregory');
    strictEqual(gregory.era, 'ce');
    strictEqual(gregory.eraYear, 2024);

    // Arithmetic, including February clamping.
    strictEqual(d.add({ days: 1 }).toString(), '2024-03-01');
    strictEqual(d.subtract({ days: 1 }).toString(), '2024-02-28');
    strictEqual(
      Temporal.PlainDate.from('2024-01-31').add({ months: 1 }).toString(),
      '2024-02-29'
    );
    strictEqual(
      d.until(Temporal.PlainDate.from('2024-03-31')).toString(),
      'P31D'
    );
    strictEqual(
      d.since(Temporal.PlainDate.from('2024-02-28')).toString(),
      'P1D'
    );
    strictEqual(
      Temporal.PlainDate.from('2024-01-15')
        .until(Temporal.PlainDate.from('2025-03-20'), { largestUnit: 'year' })
        .toString(),
      'P1Y2M5D'
    );

    // Comparison.
    ok(d.equals(isoDate()));
    ok(!d.equals(Temporal.PlainDate.from('2024-02-28')));
    strictEqual(Temporal.PlainDate.compare('2024-02-28', '2024-02-29'), -1);
    strictEqual(Temporal.PlainDate.compare(d, isoDate()), 0);
    strictEqual(Temporal.PlainDate.compare('2024-03-01', '2024-02-29'), 1);

    // Serialization.
    strictEqual(d.toJSON(), '2024-02-29');
    strictEqual(
      gregory.toString({ calendarName: 'always' }),
      '2024-02-29[u-ca=gregory]'
    );
    assertLocaleString(d);
    // An iso8601 PlainDate formats against a non-ISO calendar as well as
    // against its own.
    assertLocaleString(d, { calendar: 'gregory' });
    assertNotCoercible(d);

    // Conversion.
    strictEqual(
      d.toPlainDateTime(isoTime()).toString(),
      '2024-02-29T12:34:56.789012345'
    );
    strictEqual(d.toPlainDateTime().toString(), '2024-02-29T00:00:00');
    strictEqual(d.toPlainYearMonth().toString(), '2024-02');
    strictEqual(d.toPlainMonthDay().toString(), '02-29');
    const zoned = d.toZonedDateTime({ timeZone: 'UTC', plainTime: isoTime() });
    strictEqual(zoned.epochNanoseconds, REF_NS);
    strictEqual(
      d.toZonedDateTime({ timeZone: 'UTC' }).epochMilliseconds,
      REF_DAY_MS
    );

    strictEqual(d[Symbol.toStringTag], 'Temporal.PlainDate');
  },
};

export const plainTime = {
  test() {
    // Construction.
    strictEqual(
      new Temporal.PlainTime(12, 34, 56, 789, 12, 345).toString(),
      '12:34:56.789012345'
    );
    strictEqual(new Temporal.PlainTime().toString(), '00:00:00');
    strictEqual(
      Temporal.PlainTime.from('12:34:56.789012345').toString(),
      '12:34:56.789012345'
    );
    strictEqual(
      Temporal.PlainTime.from({ hour: 12, minute: 34 }).toString(),
      '12:34:00'
    );
    throws(
      () => Temporal.PlainTime.from({ hour: 25 }, { overflow: 'reject' }),
      RangeError
    );

    const t = isoTime();

    // Properties.
    strictEqual(t.hour, 12);
    strictEqual(t.minute, 34);
    strictEqual(t.second, 56);
    strictEqual(t.millisecond, 789);
    strictEqual(t.microsecond, 12);
    strictEqual(t.nanosecond, 345);

    // Updater.
    strictEqual(t.with({ hour: 0 }).toString(), '00:34:56.789012345');

    // Arithmetic. Time arithmetic wraps around midnight rather than throwing.
    strictEqual(t.add({ hours: 1 }).toString(), '13:34:56.789012345');
    strictEqual(t.subtract({ hours: 13 }).toString(), '23:34:56.789012345');
    strictEqual(
      Temporal.PlainTime.from('00:00')
        .until(Temporal.PlainTime.from('01:30'))
        .toString(),
      'PT1H30M'
    );
    strictEqual(
      Temporal.PlainTime.from('01:30')
        .since(Temporal.PlainTime.from('00:00'))
        .toString(),
      'PT1H30M'
    );

    // Rounding: 56.789s rounds the minute up under the default halfExpand.
    strictEqual(t.round({ smallestUnit: 'minute' }).toString(), '12:35:00');
    strictEqual(
      t.round({ smallestUnit: 'minute', roundingMode: 'floor' }).toString(),
      '12:34:00'
    );
    strictEqual(
      t.round({ smallestUnit: 'millisecond' }).toString(),
      '12:34:56.789'
    );

    // Comparison.
    ok(t.equals(isoTime()));
    ok(!t.equals(Temporal.PlainTime.from('12:34')));
    strictEqual(Temporal.PlainTime.compare('01:00', '02:00'), -1);
    strictEqual(Temporal.PlainTime.compare(t, isoTime()), 0);
    strictEqual(Temporal.PlainTime.compare('02:00', '01:00'), 1);

    // Serialization.
    strictEqual(t.toJSON(), '12:34:56.789012345');
    strictEqual(t.toString({ smallestUnit: 'minute' }), '12:34');
    strictEqual(t.toString({ fractionalSecondDigits: 3 }), '12:34:56.789');
    assertLocaleString(t);
    assertNotCoercible(t);

    strictEqual(t[Symbol.toStringTag], 'Temporal.PlainTime');
  },
};

export const plainDateTime = {
  test() {
    // Construction.
    strictEqual(
      new Temporal.PlainDateTime(
        2024,
        2,
        29,
        12,
        34,
        56,
        789,
        12,
        345
      ).toString(),
      '2024-02-29T12:34:56.789012345'
    );
    strictEqual(
      Temporal.PlainDateTime.from('2024-02-29T12:34:56.789012345').toString(),
      '2024-02-29T12:34:56.789012345'
    );
    strictEqual(
      Temporal.PlainDateTime.from({
        year: 2024,
        month: 2,
        day: 29,
        hour: 12,
      }).toString(),
      '2024-02-29T12:00:00'
    );

    const dt = isoDateTime();

    // Date-side properties.
    strictEqual(dt.calendarId, 'iso8601');
    strictEqual(dt.era, undefined);
    strictEqual(dt.eraYear, undefined);
    strictEqual(dt.year, 2024);
    strictEqual(dt.inLeapYear, true);
    strictEqual(dt.monthsInYear, 12);
    strictEqual(dt.daysInYear, 366);
    strictEqual(dt.month, 2);
    strictEqual(dt.monthCode, 'M02');
    strictEqual(dt.daysInMonth, 29);
    strictEqual(dt.weekOfYear, 9);
    strictEqual(dt.yearOfWeek, 2024);
    strictEqual(dt.daysInWeek, 7);
    strictEqual(dt.day, 29);
    strictEqual(dt.dayOfWeek, 4);
    strictEqual(dt.dayOfYear, 60);

    // Time-side properties.
    strictEqual(dt.hour, 12);
    strictEqual(dt.minute, 34);
    strictEqual(dt.second, 56);
    strictEqual(dt.millisecond, 789);
    strictEqual(dt.microsecond, 12);
    strictEqual(dt.nanosecond, 345);

    // Updaters.
    strictEqual(
      dt.with({ hour: 0 }).toString(),
      '2024-02-29T00:34:56.789012345'
    );
    strictEqual(dt.withCalendar('gregory').calendarId, 'gregory');
    strictEqual(dt.withPlainTime('01:02:03').toString(), '2024-02-29T01:02:03');
    // An omitted argument resets the time to midnight.
    strictEqual(dt.withPlainTime().toString(), '2024-02-29T00:00:00');

    // Arithmetic.
    strictEqual(
      dt.add({ days: 1 }).toString(),
      '2024-03-01T12:34:56.789012345'
    );
    strictEqual(
      dt.subtract({ days: 1 }).toString(),
      '2024-02-28T12:34:56.789012345'
    );
    strictEqual(
      Temporal.PlainDateTime.from('2024-02-29T00:00')
        .until(Temporal.PlainDateTime.from('2024-02-29T01:30'))
        .toString(),
      'PT1H30M'
    );
    strictEqual(
      Temporal.PlainDateTime.from('2024-02-29T01:30')
        .since(Temporal.PlainDateTime.from('2024-02-29T00:00'))
        .toString(),
      'PT1H30M'
    );

    // Rounding.
    strictEqual(
      dt.round({ smallestUnit: 'day' }).toString(),
      '2024-03-01T00:00:00'
    );
    strictEqual(
      dt.round({ smallestUnit: 'day', roundingMode: 'floor' }).toString(),
      '2024-02-29T00:00:00'
    );

    // Comparison.
    ok(dt.equals(isoDateTime()));
    ok(!dt.equals(Temporal.PlainDateTime.from('2024-02-29T00:00')));
    strictEqual(
      Temporal.PlainDateTime.compare('2024-02-29T00:00', '2024-02-29T01:00'),
      -1
    );
    strictEqual(Temporal.PlainDateTime.compare(dt, isoDateTime()), 0);
    strictEqual(
      Temporal.PlainDateTime.compare('2024-02-29T01:00', '2024-02-29T00:00'),
      1
    );

    // Serialization.
    strictEqual(dt.toJSON(), '2024-02-29T12:34:56.789012345');
    strictEqual(dt.toString({ smallestUnit: 'minute' }), '2024-02-29T12:34');
    assertLocaleString(dt);
    assertNotCoercible(dt);

    // Conversion.
    strictEqual(dt.toPlainDate().toString(), '2024-02-29');
    strictEqual(dt.toPlainTime().toString(), '12:34:56.789012345');
    strictEqual(dt.toZonedDateTime('UTC').epochNanoseconds, REF_NS);

    strictEqual(dt[Symbol.toStringTag], 'Temporal.PlainDateTime');
  },
};

export const plainYearMonth = {
  test() {
    // Construction.
    strictEqual(new Temporal.PlainYearMonth(2024, 2).toString(), '2024-02');
    strictEqual(Temporal.PlainYearMonth.from('2024-02').toString(), '2024-02');
    strictEqual(
      Temporal.PlainYearMonth.from({ year: 2024, month: 2 }).toString(),
      '2024-02'
    );

    const ym = isoYearMonth();

    // Properties.
    strictEqual(ym.calendarId, 'iso8601');
    strictEqual(ym.era, undefined);
    strictEqual(ym.eraYear, undefined);
    strictEqual(ym.year, 2024);
    strictEqual(ym.month, 2);
    strictEqual(ym.monthCode, 'M02');
    strictEqual(ym.inLeapYear, true);
    strictEqual(ym.monthsInYear, 12);
    strictEqual(ym.daysInYear, 366);
    strictEqual(ym.daysInMonth, 29);

    // Updater.
    strictEqual(ym.with({ month: 3 }).toString(), '2024-03');

    // Arithmetic.
    strictEqual(ym.add({ months: 1 }).toString(), '2024-03');
    strictEqual(ym.subtract({ months: 1 }).toString(), '2024-01');
    strictEqual(
      ym.until(Temporal.PlainYearMonth.from('2024-05')).toString(),
      'P3M'
    );
    // largestUnit defaults to auto, which is year here, so a 12-month gap comes
    // back as P1Y rather than P12M.
    strictEqual(
      ym.since(Temporal.PlainYearMonth.from('2023-02')).toString(),
      'P1Y'
    );
    strictEqual(
      ym
        .since(Temporal.PlainYearMonth.from('2023-02'), {
          largestUnit: 'month',
        })
        .toString(),
      'P12M'
    );

    // Comparison.
    ok(ym.equals(isoYearMonth()));
    ok(!ym.equals(Temporal.PlainYearMonth.from('2024-03')));
    strictEqual(Temporal.PlainYearMonth.compare('2024-01', '2024-02'), -1);
    strictEqual(Temporal.PlainYearMonth.compare(ym, isoYearMonth()), 0);
    strictEqual(Temporal.PlainYearMonth.compare('2024-03', '2024-02'), 1);

    // Serialization.
    strictEqual(ym.toJSON(), '2024-02');
    assertLocaleString(ym, { calendar: 'iso8601' });
    assertNotCoercible(ym);

    // Conversion.
    strictEqual(ym.toPlainDate({ day: 29 }).toString(), '2024-02-29');

    strictEqual(ym[Symbol.toStringTag], 'Temporal.PlainYearMonth');
  },
};

export const plainMonthDay = {
  test() {
    // Construction.
    strictEqual(new Temporal.PlainMonthDay(2, 29).toString(), '02-29');
    strictEqual(Temporal.PlainMonthDay.from('02-29').toString(), '02-29');
    strictEqual(
      Temporal.PlainMonthDay.from({ month: 2, day: 29 }).toString(),
      '02-29'
    );
    strictEqual(
      Temporal.PlainMonthDay.from({ monthCode: 'M02', day: 29 }).toString(),
      '02-29'
    );

    const md = isoMonthDay();

    // Properties. PlainMonthDay deliberately exposes no `month`, only
    // `monthCode`, because month numbers are not stable across calendars.
    strictEqual(md.calendarId, 'iso8601');
    strictEqual(md.monthCode, 'M02');
    strictEqual(md.day, 29);

    // Updater.
    strictEqual(md.with({ day: 28 }).toString(), '02-28');

    // Comparison. PlainMonthDay has `equals` but no `compare`, since
    // month-days have no total order without a year.
    ok(md.equals(isoMonthDay()));
    ok(!md.equals(Temporal.PlainMonthDay.from('02-28')));
    strictEqual(Temporal.PlainMonthDay.compare, undefined);

    // Serialization.
    strictEqual(md.toJSON(), '02-29');
    assertLocaleString(md, { calendar: 'iso8601' });
    assertNotCoercible(md);

    // Conversion. `toPlainDate` takes no options and always resolves fields
    // with `constrain`, so 02-29 lands on the 29th in a leap year and clamps
    // to the 28th in a common year rather than throwing.
    strictEqual(md.toPlainDate({ year: 2024 }).toString(), '2024-02-29');
    strictEqual(md.toPlainDate({ year: 2023 }).toString(), '2023-02-28');

    strictEqual(md[Symbol.toStringTag], 'Temporal.PlainMonthDay');
  },
};

export const zonedDateTime = {
  test() {
    // Construction.
    strictEqual(
      new Temporal.ZonedDateTime(REF_NS, 'UTC').epochNanoseconds,
      REF_NS
    );
    strictEqual(
      new Temporal.ZonedDateTime(REF_NS, 'UTC', 'gregory').calendarId,
      'gregory'
    );
    strictEqual(
      Temporal.ZonedDateTime.from('2024-02-29T12:34:56.789012345+00:00[UTC]')
        .epochNanoseconds,
      REF_NS
    );
    strictEqual(
      Temporal.ZonedDateTime.from({
        year: 2024,
        month: 2,
        day: 29,
        hour: 12,
        timeZone: 'UTC',
      }).toString(),
      '2024-02-29T12:00:00+00:00[UTC]'
    );

    const z = utcZoned();

    // Date-side properties.
    strictEqual(z.calendarId, 'iso8601');
    strictEqual(z.era, undefined);
    strictEqual(z.eraYear, undefined);
    strictEqual(z.year, 2024);
    strictEqual(z.inLeapYear, true);
    strictEqual(z.monthsInYear, 12);
    strictEqual(z.daysInYear, 366);
    strictEqual(z.month, 2);
    strictEqual(z.monthCode, 'M02');
    strictEqual(z.daysInMonth, 29);
    strictEqual(z.weekOfYear, 9);
    strictEqual(z.yearOfWeek, 2024);
    strictEqual(z.daysInWeek, 7);
    strictEqual(z.day, 29);
    strictEqual(z.dayOfWeek, 4);
    strictEqual(z.dayOfYear, 60);

    // Time-side properties.
    strictEqual(z.hour, 12);
    strictEqual(z.minute, 34);
    strictEqual(z.second, 56);
    strictEqual(z.millisecond, 789);
    strictEqual(z.microsecond, 12);
    strictEqual(z.nanosecond, 345);

    // Time-zone and epoch properties.
    strictEqual(z.timeZoneId, 'UTC');
    strictEqual(z.offset, '+00:00');
    strictEqual(z.offsetNanoseconds, 0);
    strictEqual(z.hoursInDay, 24);
    strictEqual(z.epochMilliseconds, REF_MS);
    strictEqual(z.epochNanoseconds, REF_NS);

    // Updaters.
    strictEqual(
      z.with({ hour: 0 }).toString(),
      '2024-02-29T00:34:56.789012345+00:00[UTC]'
    );
    strictEqual(z.withCalendar('gregory').era, 'ce');
    strictEqual(
      z.withPlainTime('01:02:03').toString(),
      '2024-02-29T01:02:03+00:00[UTC]'
    );
    strictEqual(z.withPlainTime().toString(), '2024-02-29T00:00:00+00:00[UTC]');
    const ny = z.withTimeZone('America/New_York');
    strictEqual(ny.timeZoneId, 'America/New_York');
    // Same instant, different wall clock.
    strictEqual(ny.epochNanoseconds, REF_NS);
    strictEqual(ny.hour, 7);
    strictEqual(ny.offset, '-05:00');
    strictEqual(ny.offsetNanoseconds, -5 * 3600000000000);

    // Arithmetic.
    strictEqual(z.add({ hours: 1 }).epochNanoseconds, REF_NS + 3600000000000n);
    strictEqual(
      z.subtract({ hours: 1 }).epochNanoseconds,
      REF_NS - 3600000000000n
    );
    strictEqual(
      Temporal.ZonedDateTime.from('2024-02-29T00:00+00:00[UTC]')
        .until(Temporal.ZonedDateTime.from('2024-02-29T01:30+00:00[UTC]'))
        .toString(),
      'PT1H30M'
    );
    strictEqual(
      Temporal.ZonedDateTime.from('2024-02-29T01:30+00:00[UTC]')
        .since(Temporal.ZonedDateTime.from('2024-02-29T00:00+00:00[UTC]'))
        .toString(),
      'PT1H30M'
    );

    // Rounding.
    strictEqual(
      z.round({ smallestUnit: 'day', roundingMode: 'floor' }).epochMilliseconds,
      REF_DAY_MS
    );

    // Comparison.
    ok(z.equals(utcZoned()));
    // `equals` compares instant *and* zone, unlike `compare`.
    ok(!z.equals(ny));
    strictEqual(Temporal.ZonedDateTime.compare(z, ny), 0);
    strictEqual(
      Temporal.ZonedDateTime.compare(
        Temporal.ZonedDateTime.from('2024-02-29T00:00+00:00[UTC]'),
        Temporal.ZonedDateTime.from('2024-02-29T01:00+00:00[UTC]')
      ),
      -1
    );
    strictEqual(
      Temporal.ZonedDateTime.compare(
        Temporal.ZonedDateTime.from('2024-02-29T01:00+00:00[UTC]'),
        Temporal.ZonedDateTime.from('2024-02-29T00:00+00:00[UTC]')
      ),
      1
    );

    // Serialization.
    strictEqual(z.toString(), '2024-02-29T12:34:56.789012345+00:00[UTC]');
    strictEqual(z.toJSON(), '2024-02-29T12:34:56.789012345+00:00[UTC]');
    strictEqual(
      z.toString({ timeZoneName: 'never' }),
      '2024-02-29T12:34:56.789012345+00:00'
    );
    assertLocaleString(z);
    assertNotCoercible(z);

    // Day boundaries and transitions.
    strictEqual(z.startOfDay().epochMilliseconds, REF_DAY_MS);
    // UTC never transitions.
    strictEqual(z.getTimeZoneTransition('next'), null);
    strictEqual(z.getTimeZoneTransition('previous'), null);

    // Conversion.
    strictEqual(z.toInstant().epochNanoseconds, REF_NS);
    strictEqual(z.toPlainDate().toString(), '2024-02-29');
    strictEqual(z.toPlainTime().toString(), '12:34:56.789012345');
    strictEqual(
      z.toPlainDateTime().toString(),
      '2024-02-29T12:34:56.789012345'
    );

    strictEqual(z[Symbol.toStringTag], 'Temporal.ZonedDateTime');
  },
};

export const zonedDateTimeDst = {
  test() {
    // US DST began 2024-03-10T07:00:00Z. These are historical rules, so the
    // values are stable even though they come from the bundled tzdata.
    const dstDay = Temporal.ZonedDateTime.from(
      '2024-03-10T00:00:00-05:00[America/New_York]'
    );
    strictEqual(dstDay.hoursInDay, 23);

    const transition = Temporal.ZonedDateTime.from(
      '2024-01-01T00:00:00-05:00[America/New_York]'
    ).getTimeZoneTransition('next');
    ok(transition instanceof Temporal.ZonedDateTime);
    strictEqual(transition.toInstant().epochMilliseconds, 1710054000000);
    strictEqual(transition.offset, '-04:00');

    // Going backwards from the same point lands on the previous November.
    const previous = Temporal.ZonedDateTime.from(
      '2024-01-01T00:00:00-05:00[America/New_York]'
    ).getTimeZoneTransition('previous');
    ok(previous instanceof Temporal.ZonedDateTime);
    strictEqual(previous.offset, '-05:00');

    // The skipped local hour is disambiguated rather than rejected.
    strictEqual(
      Temporal.ZonedDateTime.from(
        '2024-03-10T02:30:00[America/New_York]'
      ).toInstant().epochMilliseconds,
      1710055800000
    );
    throws(
      () =>
        Temporal.ZonedDateTime.from('2024-03-10T02:30:00[America/New_York]', {
          disambiguation: 'reject',
        }),
      RangeError
    );
  },
};

export const duration = {
  test() {
    // Construction.
    const full = new Temporal.Duration(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    strictEqual(full.years, 1);
    strictEqual(full.months, 2);
    strictEqual(full.weeks, 3);
    strictEqual(full.days, 4);
    strictEqual(full.hours, 5);
    strictEqual(full.minutes, 6);
    strictEqual(full.seconds, 7);
    strictEqual(full.milliseconds, 8);
    strictEqual(full.microseconds, 9);
    strictEqual(full.nanoseconds, 10);

    strictEqual(Temporal.Duration.from('PT1H30M').toString(), 'PT1H30M');
    strictEqual(
      Temporal.Duration.from({ hours: 1, minutes: 30 }).toString(),
      'PT1H30M'
    );
    strictEqual(Temporal.Duration.from(full).years, 1);

    // Sign and emptiness.
    strictEqual(full.sign, 1);
    strictEqual(full.blank, false);
    strictEqual(new Temporal.Duration().sign, 0);
    strictEqual(new Temporal.Duration().blank, true);
    strictEqual(new Temporal.Duration().toString(), 'PT0S');

    // Updater.
    strictEqual(full.with({ years: 2 }).years, 2);

    // Negation and absolute value.
    strictEqual(full.negated().sign, -1);
    strictEqual(full.negated().years, -1);
    strictEqual(full.negated().abs().sign, 1);
    strictEqual(full.negated().abs().years, 1);

    // Arithmetic.
    strictEqual(
      Temporal.Duration.from({ hours: 1 }).add({ minutes: 30 }).toString(),
      'PT1H30M'
    );
    strictEqual(
      Temporal.Duration.from({ hours: 1 }).subtract({ minutes: 30 }).toString(),
      'PT30M'
    );

    // Rounding and totalling. Calendar units need a `relativeTo` anchor;
    // pure time units do not.
    strictEqual(
      Temporal.Duration.from({ seconds: 90 })
        .round({ largestUnit: 'minute' })
        .toString(),
      'PT1M30S'
    );
    strictEqual(
      Temporal.Duration.from({ minutes: 90 })
        .round({ smallestUnit: 'hour' })
        .toString(),
      'PT2H'
    );
    strictEqual(
      Temporal.Duration.from({ months: 1 })
        .round({
          largestUnit: 'day',
          relativeTo: Temporal.PlainDate.from('2024-02-01'),
        })
        .toString(),
      'P29D'
    );
    strictEqual(
      Temporal.Duration.from({ minutes: 90 }).total({ unit: 'hour' }),
      1.5
    );
    strictEqual(
      Temporal.Duration.from({ months: 1 }).total({
        unit: 'day',
        relativeTo: Temporal.PlainDate.from('2024-02-01'),
      }),
      29
    );

    // Comparison.
    strictEqual(
      Temporal.Duration.compare(
        Temporal.Duration.from({ minutes: 1 }),
        Temporal.Duration.from({ seconds: 90 })
      ),
      -1
    );
    strictEqual(
      Temporal.Duration.compare(
        Temporal.Duration.from({ seconds: 90 }),
        Temporal.Duration.from({ minutes: 1, seconds: 30 })
      ),
      0
    );
    strictEqual(
      Temporal.Duration.compare(
        Temporal.Duration.from({ seconds: 90 }),
        Temporal.Duration.from({ minutes: 1 })
      ),
      1
    );

    // Serialization.
    strictEqual(Temporal.Duration.from({ hours: 1 }).toJSON(), 'PT1H');
    strictEqual(
      Temporal.Duration.from({ seconds: 1, milliseconds: 500 }).toString({
        fractionalSecondDigits: 1,
      }),
      'PT1.5S'
    );
    // Duration is the one class whose toString rejects hour and minute as
    // smallestUnit; the others accept both.
    throws(
      () =>
        Temporal.Duration.from({ minutes: 90 }).toString({
          smallestUnit: 'minute',
        }),
      RangeError
    );
    throws(
      () =>
        Temporal.Duration.from({ hours: 1 }).toString({
          smallestUnit: 'hour',
        }),
      RangeError
    );
    assertLocaleString(Temporal.Duration.from({ hours: 1 }));
    assertNotCoercible(Temporal.Duration.from({ hours: 1 }));

    strictEqual(full[Symbol.toStringTag], 'Temporal.Duration');
  },
};

export const dateInterop = {
  test() {
    // Built from a fixed epoch value, not the current time.
    strictEqual(new Date(0).toTemporalInstant().epochNanoseconds, 0n);
    strictEqual(new Date(REF_MS).toTemporalInstant().epochMilliseconds, REF_MS);
    strictEqual(
      new Date(REF_MS).toTemporalInstant().toString(),
      '2024-02-29T12:34:56.789Z'
    );

    // Round-trip back through the legacy Date API.
    const instant = Temporal.Instant.fromEpochMilliseconds(REF_MS);
    strictEqual(new Date(instant.epochMilliseconds).getTime(), REF_MS);
  },
};

export const rangeLimits = {
  test() {
    // Temporal refuses to represent instants beyond +/-10^8 days from the
    // epoch, matching the valid range of Date. That is 10^8 * 86400 * 10^9
    // nanoseconds.
    const maxNs = 8640000000000000000000n;
    strictEqual(maxNs, BigInt(10 ** 8) * 86400n * 1_000_000_000n);
    strictEqual(
      Temporal.Instant.fromEpochNanoseconds(maxNs).epochNanoseconds,
      maxNs
    );
    strictEqual(
      Temporal.Instant.fromEpochNanoseconds(-maxNs).epochNanoseconds,
      -maxNs
    );
    throws(() => Temporal.Instant.fromEpochNanoseconds(maxNs + 1n), RangeError);
    throws(
      () => Temporal.Instant.fromEpochNanoseconds(-maxNs - 1n),
      RangeError
    );
    throws(() => Temporal.PlainDate.from('+275760-09-14'), RangeError);
  },
};
