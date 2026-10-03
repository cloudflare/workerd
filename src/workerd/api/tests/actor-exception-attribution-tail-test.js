// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests that an exception thrown by a Durable Object request is reported in
// that request's trace while a newer request to the same object is still in
// progress. All requests to a Durable Object share one IoContext, whose current
// request is the newest one, so the exception must not be attributed to the
// IoContext's current request.
import * as assert from 'node:assert';
import { DurableObject } from 'cloudflare:workers';

// Summaries of the fetch traces received by the tail handler, keyed by URL.
const traces = new Map();

export class OverlappingRequests extends DurableObject {
  #newerStarted = Promise.withResolvers();
  #newerReleased = Promise.withResolvers();

  async fetch(request) {
    switch (new URL(request.url).pathname) {
      case '/older':
        await this.#newerStarted.promise;
        throw new Error('older request failed');
      case '/newer':
        this.#newerStarted.resolve();
        await this.#newerReleased.promise;
        return new Response('newer request done');
      case '/release':
        this.#newerReleased.resolve();
        return new Response('released');
    }
    return new Response('not found', { status: 404 });
  }
}

async function waitForTraces(urls) {
  for (let attempt = 0; attempt < 100; ++attempt) {
    if (urls.every((url) => traces.has(url))) {
      return urls.map((url) => traces.get(url));
    }
    await scheduler.wait(10);
  }
  assert.fail(
    `missing traces for ${urls.filter((url) => !traces.has(url)).join(', ')}`
  );
}

export default {
  tail(events) {
    for (const event of events) {
      const url = event.event?.request?.url;
      if (url === undefined) continue;
      traces.set(url, {
        outcome: event.outcome,
        exceptions: event.exceptions.map(({ name, message }) => ({
          name,
          message,
        })),
      });
    }
  },
};

export const test = {
  async test(ctrl, env) {
    const stub = env.OVERLAPPING.get(env.OVERLAPPING.idFromName('overlap'));

    // The older request fails once the newer one has started, while the newer
    // one is still in progress and therefore the object's current request.
    const older = stub.fetch('http://example.com/older');
    const newer = stub.fetch('http://example.com/newer');
    await assert.rejects(older, { message: 'older request failed' });

    await stub.fetch('http://example.com/release');
    assert.strictEqual(await (await newer).text(), 'newer request done');

    const [olderTrace, newerTrace, releaseTrace] = await waitForTraces([
      'http://example.com/older',
      'http://example.com/newer',
      'http://example.com/release',
    ]);

    assert.deepStrictEqual(olderTrace, {
      outcome: 'exception',
      exceptions: [{ name: 'Error', message: 'older request failed' }],
    });
    assert.deepStrictEqual(newerTrace, { outcome: 'ok', exceptions: [] });
    assert.deepStrictEqual(releaseTrace, { outcome: 'ok', exceptions: [] });
  },
};
