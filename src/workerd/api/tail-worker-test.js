// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

let responseMap = new Map();

// JSON.stringify() does not support BigInt by default - manually add that here.
BigInt.prototype.toJSON = function () {
  return this.toString();
};

export default {
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  tailStream(event, env, ctx) {
    // Onset event, must be singleton

    // For scheduled and alarm tests, override scheduledTime to make this test deterministic.
    if (
      event.event.info.type == 'scheduled' ||
      event.event.info.type == 'alarm'
    ) {
      event.event.info.scheduledTime = '1970-01-01T00:00:00.000Z';
    }

    responseMap.set(event.traceId, JSON.stringify(event.event));
    return (event) => {
      let cons = responseMap.get(event.traceId);
      responseMap.set(event.traceId, cons + JSON.stringify(event.event));
    };
  },
};

export const test = {
  async test() {
    // HACK: The prior tests terminates once the scheduled() invocation has returned a response, but
    // propagating the outcome of the invocation may take longer. Wait briefly so this can go ahead.
    await scheduler.wait(50);
    // This test verifies that we're able to receive tail stream events for various handlers.

    // Recorded streaming tail worker events, in insertion order.
    let response = Array.from(responseMap.values());

    let expected = [
      // http-test.js: fetch and scheduled events get reported correctly.
      // First event is emitted by the test() event, which allows us to get some coverage for span tracing.
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"custom"}}{"type":"spanOpen","name":"fetch","parentSpanId":"0"}{"type":"attributes","info":[{"name":"network.protocol.name","value":"http"},{"name":"network.protocol.version","value":"HTTP/1.1"},{"name":"http.request.method","value":"POST"},{"name":"url.full","value":"http://placeholder/body-length"},{"name":"http.request.body.size","value":"3"},{"name":"http.response.status_code","value":"200"},{"name":"http.response.body.size","value":"22"}]}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"fetch","parentSpanId":"0"}{"type":"attributes","info":[{"name":"network.protocol.name","value":"http"},{"name":"network.protocol.version","value":"HTTP/1.1"},{"name":"http.request.method","value":"POST"},{"name":"url.full","value":"http://placeholder/body-length"},{"name":"http.response.status_code","value":"200"},{"name":"http.response.body.size","value":"31"}]}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"scheduled","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"scheduled","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"spanOpen","name":"worker","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"http://placeholder/body-length","headers":[]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"spanOpen","name":"worker","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"http://placeholder/body-length","headers":[]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"spanOpen","name":"worker","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"scheduled","scheduledTime":"1970-01-01T00:00:00.000Z","cron":""}}{"type":"spanOpen","name":"worker","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"scheduled","scheduledTime":"1970-01-01T00:00:00.000Z","cron":"* * * * 30"}}{"type":"spanOpen","name":"worker","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"custom"}}{"type":"spanOpen","name":"worker","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://placeholder/not-found","headers":[]}}{"type":"return","info":{"type":"fetch","statusCode":404}}{"type":"spanOpen","name":"worker","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://placeholder/web-socket","headers":[{"name":"upgrade","value":"websocket"}]}}{"type":"exception","name":"Error","message":"The Workers runtime canceled this request because it detected that your Worker\'s code had hung and would never generate a response. Refer to: https://developers.cloudflare.com/workers/observability/errors/"}{"type":"spanOpen","name":"worker","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      // Test that when the onset event would be larger than MAX_TRACE_BYTES, we still send the event but with variable size fields counted to the size being empty.
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"","headers":[]}}{"type":"return","info":{"type":"fetch","statusCode":404}}{"type":"spanOpen","name":"worker","parentSpanId":"0"}{"type":"spanClose","outcome":"ok"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

      // queue-test.js: queue events
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"custom"}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"https://fake-host/message","headers":[{"name":"content-type","value":"application/octet-stream"},{"name":"x-msg-delay-secs","value":"2"},{"name":"x-msg-fmt","value":"text"}]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"https://fake-host/message","headers":[{"name":"content-type","value":"application/octet-stream"},{"name":"x-msg-fmt","value":"bytes"}]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"https://fake-host/message","headers":[{"name":"content-type","value":"application/octet-stream"},{"name":"x-msg-fmt","value":"json"}]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"https://fake-host/message","headers":[{"name":"content-type","value":"application/octet-stream"},{"name":"x-msg-fmt","value":"v8"}]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"fetch","method":"POST","url":"https://fake-host/batch","headers":[{"name":"cf-queue-batch-bytes","value":"31"},{"name":"cf-queue-batch-count","value":"4"},{"name":"cf-queue-largest-msg","value":"13"},{"name":"content-type","value":"application/json"},{"name":"x-msg-delay-secs","value":"2"}]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"queue","queueName":"test-queue","batchSize":5}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

      // actor-alarms-test.js: alarm events
      '{"type":"onset","executionModel":"durableObject","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://foo/test","headers":[]}}{"type":"return","info":{"type":"fetch","statusCode":200}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"durableObject","scriptTags":[],"info":{"type":"alarm","scheduledTime":"1970-01-01T00:00:00.000Z"}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

      // legacy tail worker, triggered via alarm test. It would appear that these being recorded
      // after the onsets above is not guaranteed, but since the streaming tail worker is invoked
      // when the main invocation starts whereas the legacy tail worker is only invoked when it ends
      // this should be fine in practice.
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"trace","traces":[""]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"trace","traces":[""]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

      // tests/websocket-hibernation.js: hibernatableWebSocket events
      '{"type":"onset","executionModel":"durableObject","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://example.com/","headers":[{"name":"upgrade","value":"websocket"}]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"durableObject","scriptTags":[],"info":{"type":"fetch","method":"GET","url":"http://example.com/hibernation","headers":[{"name":"upgrade","value":"websocket"}]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"durableObject","scriptTags":[],"info":{"type":"hibernatableWebSocket","info":{"type":"message"}}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"durableObject","scriptTags":[],"info":{"type":"hibernatableWebSocket","info":{"type":"close","code":1000,"wasClean":true}}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',

      // tail-worker-test-jsrpc: Regression test for EW-9282 (missing onset event with
      // JsRpcSessionCustomEventImpl). This is derived from tests/js-rpc-test.js.
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"custom"}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
      '{"type":"onset","executionModel":"stateless","scriptTags":[],"info":{"type":"jsrpc","methodName":"nonFunctionProperty"}}{"type":"log","level":"log","message":["foo"]}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}',
    ];

    assert.deepStrictEqual(response, expected);
  },
};
