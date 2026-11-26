// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The tail worker and the streaming tail worker of the Workers that scheduled-test.js sends
// scheduled events to. Its test checks what each received for each event.
//
// The test waits until both have received every event, however long that takes, rather than for a
// fixed time: an event can still be reaching them after scheduled-test's test has finished. It
// polls, because a request that only waits for another request to settle a promise looks hung,
// and is cancelled. While it waits, it logs what has arrived every few seconds, so that a test
// that times out shows what is missing.

import assert from 'node:assert';

function summarize(event) {
  switch (event.type) {
    case 'onset':
      return { type: event.type, info: event.info };
    case 'log':
      return { type: event.type, level: event.level, message: event.message };
    case 'exception':
      return { type: event.type, name: event.name, message: event.message };
    case 'outcome':
      return { type: event.type, outcome: event.outcome };
    default:
      return { type: event.type };
  }
}

function onset(cron) {
  // Tests run in predictable mode, which hides the scheduled time from streaming tail workers.
  return {
    type: 'onset',
    info: { type: 'scheduled', scheduledTime: new Date(0), cron },
  };
}

function log(message, level = 'log') {
  return { type: 'log', level, message: [message] };
}

const RETURN = { type: 'return' };
const OK = { type: 'outcome', outcome: 'ok' };
const EXCEPTION = { type: 'outcome', outcome: 'exception' };

// What the streaming tail worker receives for each event, in the order scheduled-test sends them.
const EXPECTED_STREAMS = [
  // scheduled-test-handler
  [
    onset('reject'),
    log('scheduled reject 1000 object object'),
    { type: 'exception', name: 'Error', message: 'rejected' },
    EXCEPTION,
  ],
  [
    onset('wait-until'),
    log('scheduled wait-until 1000 object object'),
    RETURN,
    log('waitUntil finished'),
    OK,
  ],
  // scheduled-test-handler's noHandler entrypoint
  [
    onset('ok'),
    log(
      'Received a ScheduledEvent but we lack a handler for ScheduledEvents (a.k.a. Cron ' +
        'Triggers). Did you remember to export a scheduled() function?',
      'warn'
    ),
    {
      type: 'exception',
      name: 'Error',
      message: 'Handler does not export a scheduled() function',
    },
    EXCEPTION,
  ],
  // scheduled-test-listener
  [onset('ok'), log('listener ok 1000'), OK],
  [onset('no-retry'), log('listener no-retry 1000'), OK],
  [
    onset('throw'),
    log('listener throw 1000'),
    { type: 'exception', name: 'Error', message: 'thrown by the listener' },
    EXCEPTION,
  ],
  [
    onset('wait-until'),
    log('listener wait-until 1000'),
    log('waitUntil finished'),
    OK,
  ],
  // scheduled-test-no-listener
  [
    onset('ok'),
    log(
      'Received a ScheduledEvent but we lack an event listener for scheduled events (a.k.a. ' +
        'Cron Triggers). Did you remember to call addEventListener("scheduled", ...)?',
      'warn'
    ),
    {
      type: 'exception',
      name: 'Error',
      message: 'No event listener registered for scheduled events.',
    },
    EXCEPTION,
  ],
];

// The event of each trace that the tail worker receives. Unlike a streaming tail worker, it sees
// the scheduled time in predictable mode.
function trace(cron, outcome) {
  return { scheduledTime: 1000, cron, outcome };
}

// Sorted, since nothing orders the delivery of one event's trace before the next one's.
const EXPECTED_TRACES = sortTraces([
  trace('reject', 'exception'),
  trace('wait-until', 'ok'),
  trace('ok', 'exception'),
  trace('ok', 'ok'),
  trace('no-retry', 'ok'),
  trace('throw', 'exception'),
  trace('wait-until', 'ok'),
  trace('ok', 'exception'),
]);

function sortTraces(traces) {
  const key = (trace) => JSON.stringify(trace);
  return traces.toSorted((a, b) => key(a).localeCompare(key(b)));
}

// Each stream received so far, as the list of its events, summarized.
const streams = [];
let finishedStreams = 0;
const traces = [];

export default {
  tailStream(onsetEvent) {
    const events = [summarize(onsetEvent.event)];
    streams.push(events);
    return (event) => {
      events.push(summarize(event.event));
      if (event.event.type === 'outcome') finishedStreams++;
    };
  },

  tail(received) {
    for (const trace of received) {
      traces.push({
        scheduledTime: trace.event.scheduledTime,
        cron: trace.event.cron,
        outcome: trace.outcome,
      });
    }
  },
};

export const test = {
  async test() {
    for (
      let polls = 1;
      finishedStreams < EXPECTED_STREAMS.length ||
      traces.length < EXPECTED_TRACES.length;
      polls++
    ) {
      await scheduler.wait(10);
      if (polls % 500 === 0) {
        console.log(
          `still waiting: ${finishedStreams} of ${EXPECTED_STREAMS.length} streams finished, ` +
            `${traces.length} of ${EXPECTED_TRACES.length} traces received`,
          JSON.stringify({ streams, traces })
        );
      }
    }
    assert.deepStrictEqual(streams, EXPECTED_STREAMS);
    assert.deepStrictEqual(sortTraces(traces), EXPECTED_TRACES);
  },
};
