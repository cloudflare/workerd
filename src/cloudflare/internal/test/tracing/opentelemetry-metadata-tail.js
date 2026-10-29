// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
const opens = [];
const received = [];
const spanEvents = [];
const links = [];
const markSpanIds = [];
const windows = new Map();
let outcomes = 0;
export default {
  tailStream(onset) {
    windows.set(onset.invocationId, {
      start: onset.timestamp.getTime(),
      end: Infinity,
    });
    return (event) => {
      received.push(event);
      if (event.event.type === 'spanEvent') spanEvents.push(event.event);
      if (event.event.type === 'spanLinks') links.push(...event.event.links);
      if (['spanEvent', 'spanLinks'].includes(event.event.type))
        markSpanIds.push(event.spanContext.spanId);
      if (event.event.type === 'spanOpen') opens.push(event.event);
      else {
        assert.strictEqual(Object.hasOwn(event.event, 'kind'), false);
        assert.strictEqual(Object.hasOwn(event.event, 'scope'), false);
      }
      if (event.event.type === 'outcome') {
        windows.get(onset.invocationId).end = event.timestamp.getTime();
        outcomes++;
      }
    };
  },
};

export const metadata = {
  async test() {
    for (let i = 0; outcomes < 2 && i < 100; i++) await scheduler.wait(1);
    assert.strictEqual(outcomes, 2);
    const open = (name) => opens.find((event) => event.name === name);
    assert.strictEqual(open('must-not-export'), undefined);
    const scope = {
      name: 'metadata-test',
      version: '1.2.3',
      schemaUrl: 'https://example.com/schema',
    };
    for (const [kind, name] of [
      'internal',
      'server',
      'client',
      'producer',
      'consumer',
    ].entries()) {
      assert.strictEqual(open(`kind-${kind}`).kind, name);
      assert.deepStrictEqual(open(`kind-${kind}`).scope, scope);
    }
    assert.strictEqual(open('default-kind').kind, 'internal');
    assert.deepStrictEqual(open('default-kind').scope, scope);
    assert.deepStrictEqual(open('name-only').scope, { name: 'name-only' });
    assert.deepStrictEqual(open('other-version').scope, {
      name: 'metadata-test',
      version: '2.0',
    });
    assert.deepStrictEqual(open('empty-optionals').scope, {
      name: 'empty',
      version: '',
      schemaUrl: '',
    });
    for (const enabled of [false, true]) {
      assert.deepStrictEqual(Object.keys(open(`native-${enabled}`)), [
        'type',
        'name',
        'spanId',
      ]);
    }
    assert.deepStrictEqual(open('scope-limit').scope, {
      name: 'a'.repeat(255),
      version: 'v'.repeat(255),
      schemaUrl: 's'.repeat(2047),
    });
    assert.deepStrictEqual(Object.keys(open('runtime-child')), [
      'type',
      'name',
      'spanId',
    ]);
    const entries = (name, type) =>
      received.filter(
        (e) =>
          e.spanContext.spanId === open(name).spanId && e.event.type === type
      );
    const counts = entries('count-limits', 'spanEvent');
    assert.strictEqual(counts.length, 128);
    assert.strictEqual(counts[0].event.attributes.length, 128);
    assert(counts.slice(1).every((e) => e.event.attributes.length === 0));
    const countLinks = entries('count-limits', 'spanLinks');
    assert.strictEqual(countLinks.length, 1);
    for (const entry of countLinks) {
      assert.strictEqual(entry.event.links.length, 128);
      assert.strictEqual(entry.event.links[0].attributes.length, 128);
      assert(
        entry.event.links.slice(1).every((link) => link.attributes.length === 0)
      );
    }
    for (const name of ['event-budget', 'link-budget']) {
      assert.strictEqual(entries(name, 'spanEvent').length, 0);
      assert.strictEqual(entries(name, 'spanLinks').length, 0);
      const attrs = entries(name, 'attributes').flatMap((e) => e.event.info);
      assert(
        attrs.some(
          (a) =>
            a.name === 'cloudflare.warning.type' &&
            a.value === 'span_data_limit_exceeded'
        )
      );
      assert(!attrs.some((a) => a.name === 'starved'));
    }
    const validated = entries('time-validation', 'spanEvent').map(
      (e) => e.event
    );
    assert.deepStrictEqual(
      validated.slice(0, 5).map((e) => e.time),
      [
        [1700000000, 2],
        [1700000000, 123000000],
        [1700000000, 456000000],
        [1700000001, 500000001],
        [1700000000, 999999999],
      ]
    );
    assert.strictEqual(validated[5].time, undefined);
    assert.strictEqual(validated[6].time, undefined);
    assert.deepStrictEqual(validated[7].time, [9223372036, 854775807]);
    assert.strictEqual(validated[8].name, 'a'.repeat(63));
    assert.deepStrictEqual(validated[9].attributes, []);
    const shortLink = entries('time-validation', 'spanLinks')[0].event.links[0];
    assert.strictEqual(
      shortLink.context.traceId,
      '00000000000000001111111111111111'
    );
    assert.strictEqual(shortLink.context.traceState, undefined);
    assert.strictEqual(open('time-validation').startTime, undefined);
    assert.strictEqual(
      entries('time-validation', 'spanClose')[0].event.endTime,
      undefined
    );
    assert.deepStrictEqual(
      entries('future-start', 'spanClose')[0].event.endTime,
      [9223372036, 854775807]
    );
    const originalEvents = entries('kind-0', 'spanEvent').map((e) => e.event);
    assert.deepStrictEqual(
      originalEvents.map((event) => event.name),
      ['step', 'dated', 'millis', 'implicit', 'implicit']
    );
    assert.deepStrictEqual(spanEvents[0].time, [1700000000, 123456789]);
    assert.deepStrictEqual(spanEvents[0].attributes, [
      { name: 'text', value: 'hello' },
      { name: 'count', value: 2 },
      { name: 'flags', value: [true, false] },
    ]);
    assert.deepStrictEqual(spanEvents[1].time, [1700000000, 123000000]);
    assert.deepStrictEqual(spanEvents[2].time, [1700000000, 456000000]);
    assert.strictEqual(Object.hasOwn(spanEvents[3], 'time'), false);
    assert.deepStrictEqual(spanEvents[4].attributes, []);
    const originalLinks = entries('kind-0', 'spanLinks').flatMap(
      (e) => e.event.links
    );
    assert.strictEqual(originalLinks.length, 4);
    assert.deepStrictEqual(
      entries('kind-0', 'spanLinks').map((e) => e.event.links.length),
      [1, 1, 2]
    );
    assert.deepStrictEqual(links[0], {
      context: {
        traceId: '11111111111111111111111111111111',
        spanId: '2222222222222222',
        traceFlags: 255,
        isRemote: true,
        traceState: 'vendor=value',
      },
      attributes: [{ name: 'source', value: 'options' }],
    });
    assert.deepStrictEqual(links[1].attributes, []);
    assert.strictEqual(links[2].context.isRemote, false);
    assert.deepStrictEqual(links[2].attributes, [
      { name: 'source', value: 'addLinks' },
    ]);
    assert.deepStrictEqual(links[3], links[1]);
    const starts = [
      [1700000000, 123456789],
      [1700000000, 123000000],
      [1700000000, 456000000],
      undefined,
      [1700000001, 0],
    ];
    const ends = [
      [1700000001, 987654321],
      [1700000001, 123000000],
      [1700000001, 456000000],
      undefined,
      [1700000001, 0],
    ];
    for (let kind = 0; kind <= 4; kind++) {
      const opening = open(`kind-${kind}`);
      const closing = received.find(
        (entry) =>
          entry.event.type === 'spanClose' &&
          entry.spanContext.spanId === opening.spanId
      );
      assert.deepStrictEqual(opening.startTime, starts[kind]);
      assert.deepStrictEqual(closing.event.endTime, ends[kind]);
      if (kind === 3) {
        assert.strictEqual(Object.hasOwn(opening, 'startTime'), false);
        assert.strictEqual(Object.hasOwn(closing.event, 'endTime'), false);
      }
    }
    const exceptions = received.filter(
      (entry) => entry.event.type === 'exception'
    );
    assert.strictEqual(exceptions.length, 1);
    assert.deepStrictEqual(exceptions[0].event.time, [1700000000, 223456789]);
    assert.strictEqual(exceptions[0].event.message, 'timed exception');
    assert.strictEqual(exceptions[0].spanContext.spanId, open('kind-0').spanId);
    // Application timestamps do not replace the outer I/O observation time.
    const window = windows.get(exceptions[0].invocationId);
    // Predictable mode deliberately normalizes exception observation timestamps to UNIX_EPOCH.
    if (exceptions[0].timestamp.getTime() !== 0) {
      assert(exceptions[0].timestamp.getTime() >= window.start);
      assert(exceptions[0].timestamp.getTime() <= window.end);
    }
    for (const entry of entries('kind-0', 'spanEvent')) {
      const observed = windows.get(entry.invocationId);
      assert(entry.timestamp.getTime() >= observed.start);
      assert(entry.timestamp.getTime() <= observed.end);
    }
  },
};
