// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Streaming tail worker that captures timing information for JSRPC invocations.
// This validates that Return events occur at the expected time.
//
// Expected timeline:
//   T=0:      Onset (invocation starts)
//   T=~500ms: Return (handler returns the stream)
//   T=~950ms: Outcome (stream fully consumed)

import * as assert from 'node:assert';

// Store events by invocation ID so we can analyze timing per-invocation
let invocationEvents = new Map();

export default {
  tailStream(onsetEvent, env, ctx) {
    const invocationId = onsetEvent.invocationId;
    const baseTimestamp = onsetEvent.timestamp.getTime();

    // Initialize event list for this invocation
    const events = [
      {
        type: onsetEvent.event.type,
        relativeTimeMs: 0,
        info: onsetEvent.event.info?.type,
        entrypoint: onsetEvent.event.entrypoint,
      },
    ];
    invocationEvents.set(invocationId, { baseTimestamp, events });

    return (event) => {
      const relativeTimeMs = event.timestamp.getTime() - baseTimestamp;

      const eventRecord = {
        type: event.event.type,
        relativeTimeMs,
      };

      // Capture additional info for specific event types
      if (event.event.type === 'return') {
        eventRecord.returnInfo = event.event.info;
      } else if (event.event.type === 'outcome') {
        eventRecord.outcome = event.event.outcome;
        eventRecord.cpuTime = event.event.cpuTime;
        eventRecord.wallTime = event.event.wallTime;
      } else if (event.event.type === 'attributes') {
        eventRecord.attributes = event.event.info;
      }

      events.push(eventRecord);
    };
  },
};

export const test = {
  async test() {
    // Wait briefly for all tail events to be processed
    await scheduler.wait(200);

    // Find the StreamingService invocation (the JSRPC call)
    let streamingServiceEvents = null;
    for (const [invocationId, data] of invocationEvents.entries()) {
      const onset = data.events[0];
      if (onset.entrypoint === 'StreamingService' && onset.info === 'jsrpc') {
        streamingServiceEvents = data.events;
        break;
      }
    }

    if (!streamingServiceEvents) {
      console.log('Captured invocations:');
      for (const [invocationId, data] of invocationEvents.entries()) {
        console.log(`  ${invocationId}: ${JSON.stringify(data.events[0])}`);
      }
      throw new Error(
        'Could not find StreamingService JSRPC invocation events'
      );
    }

    console.log(
      'StreamingService events:',
      JSON.stringify(streamingServiceEvents, null, 2)
    );

    // Find the key events
    const onset = streamingServiceEvents.find((e) => e.type === 'onset');
    const returnEvent = streamingServiceEvents.find((e) => e.type === 'return');
    const outcome = streamingServiceEvents.find((e) => e.type === 'outcome');

    // Validate all events are present
    assert.ok(onset, 'Missing onset event');
    assert.ok(outcome, 'Missing outcome event');

    if (!returnEvent) {
      console.log('WARNING: No return event found for JSRPC invocation!');
      console.log(
        'This may indicate setReturn() is not being called for JSRPC.'
      );
      throw new Error('Return event not captured for JSRPC invocation');
    }

    const timeToReturn = returnEvent.relativeTimeMs;
    const timeToOutcome = outcome.relativeTimeMs;
    const gap = timeToOutcome - timeToReturn;

    console.log(`timeToReturn: ${timeToReturn}ms`);
    console.log(`timeToOutcome: ${timeToOutcome}ms`);
    console.log(`Gap between Return and Outcome: ${gap}ms`);

    // Key validation: Return must happen BEFORE Outcome
    if (timeToReturn >= timeToOutcome) {
      throw new Error(
        `Return event (${timeToReturn}ms) should happen before Outcome (${timeToOutcome}ms). ` +
          `This indicates Return is fired when stream ends, not when handler returns.`
      );
    }

    // Return should happen at least 50ms after onset (handler sleeps for 500ms)
    // NOTE: Using a lower threshold (50ms instead of 200ms) because Windows has
    // significant timer jitter and scheduler.wait() timing assumptions don't hold
    // reliably under load.
    if (timeToReturn < 50) {
      throw new Error(
        `Return event (${timeToReturn}ms) happened too quickly. ` +
          `Expected at least 50ms from onset (handler sleeps for 500ms before returning).`
      );
    }

    // Return should be significantly before Outcome (at least 200ms gap for stream drain)
    if (gap < 200) {
      throw new Error(
        `Gap between Return (${timeToReturn}ms) and Outcome (${timeToOutcome}ms) is only ${gap}ms. ` +
          `Expected at least 200ms for stream consumption.`
      );
    }

    console.log(`PASS: Return event occurs ${gap}ms before Outcome`);
  },
};
