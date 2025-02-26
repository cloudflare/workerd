// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

let resposeMap = new Map();

export default {
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  tailStream(...args) {
    // Onset event, must be singleton
    resposeMap.set(args[0].traceId, JSON.stringify(args[0].event));
    return (...args) => {
      // TODO(streaming-tail-worker): Support several queued elements
      let cons = resposeMap.get(args[0].traceId);
      resposeMap.set(args[0].traceId, cons + JSON.stringify(args[0].event));
    };
  },
};

export const test = {
  async test() {
    // HACK: The prior tests terminates once the scheduled() invocation has returned a response, but
    // propagating the outcome of the invocation may take longer. Wait briefly so this can go ahead.
    await scheduler.wait(100);
    // This test verifies that we're able to receive tail stream events for various handlers.

    // Recorded streaming tail worker events, in insertion order.
    let response = Array.from(resposeMap.values());

    let expected = [
      // http-test.js: fetch and scheduled events get reported correctly.
      '{"type":"onset","info":{"type":"fetch","method":"POST","url":"http://placeholder/body-length","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"fetch","method":"POST","url":"http://placeholder/body-length","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"scheduled","scheduledTime":"1970-01-01T00:00:00.000Z","cron":""}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"scheduled","scheduledTime":"1970-01-01T00:00:00.000Z","cron":"* * * * 30"}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"fetch","method":"GET","url":"http://placeholder/not-found","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"fetch","method":"GET","url":"http://placeholder/web-socket","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"exception","cpuTime":0,"wallTime":0}',

      // queue-test.js: queue events
      '{"type":"onset","info":{"type":"fetch","method":"POST","url":"https://fake-host/message","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"fetch","method":"POST","url":"https://fake-host/message","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"fetch","method":"POST","url":"https://fake-host/message","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"fetch","method":"POST","url":"https://fake-host/message","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"fetch","method":"POST","url":"https://fake-host/batch","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"queue","queueName":"test-queue","batchSize":5}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

      // actor-alarms-test.js: alarm events
      '{"type":"onset","info":{"type":"fetch","method":"GET","url":"http://foo/test","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"alarm","scheduledTime":"1970-01-01T00:00:00.000Z"}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

      // legacy tail worker, triggered via alarm test. It would appear that these being recorded
      // after the onsets above is not guaranteed, but since the streaming tail worker is invoked
      // when the main invocation starts whereas the legacy tail worker is only invoked when it ends
      // this should be fine in practice.
      '{"type":"onset","info":{"type":"trace","traces":[""]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"trace","traces":[""]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"trace","traces":[""]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

      // tests/websocket-hibernation.js: hibernatableWebSocket events
      '{"type":"onset","info":{"type":"fetch","method":"GET","url":"http://example.com/","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"fetch","method":"GET","url":"http://example.com/hibernation","cfJson":"{}","headers":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"hibernatableWebSocket","info":{"type":"message"}}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","info":{"type":"hibernatableWebSocket","info":{"type":"close","code":1000,"wasClean":true}}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
    ];

    assert.deepStrictEqual(response, expected);
  },
};
