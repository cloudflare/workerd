// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkerEntrypoint } from 'cloudflare:workers';
import * as assert from 'node:assert';

let responseArr = [];

export default class TailWorker extends WorkerEntrypoint {
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  async tail(events, env, ctx) {
    events.map((event) => {
      responseArr.push(JSON.stringify(event));
    });
  }
}

export const test = {
  async test() {
    // HACK: The prior tests terminates once the scheduled() invocation has returned a response, but
    // propagating the outcome of the invocation may take longer. Wait briefly so this can go ahead.
    await scheduler.wait(50);

    let expected = [
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"http://placeholder/body-length","method":"POST","headers":{}},"response":{"status":200}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"cron":"* * * * 30"}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"cron":""}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"http://placeholder/body-length","method":"POST","headers":{}},"response":{"status":200}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"http://placeholder/not-found","method":"GET","headers":{}},"response":{"status":404}}}',
      `{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[{"name":"Error","message":"The Workers runtime canceled this request because it detected that your Worker's code had hung and would never generate a response. Refer to: https://developers.cloudflare.com/workers/observability/errors/","timestamp":0}],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"http://placeholder/web-socket","method":"GET","headers":{"upgrade":"websocket"}}}}`,
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"https://fake-host/message","method":"POST","headers":{"content-type":"application/octet-stream","x-msg-delay-secs":"2","x-msg-fmt":"text"}},"response":{"status":200}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"https://fake-host/message","method":"POST","headers":{"content-type":"application/octet-stream","x-msg-fmt":"bytes"}},"response":{"status":200}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"https://fake-host/message","method":"POST","headers":{"content-type":"application/octet-stream","x-msg-fmt":"json"}},"response":{"status":200}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"https://fake-host/message","method":"POST","headers":{"content-type":"application/octet-stream","x-msg-fmt":"v8"}},"response":{"status":200}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"batchSize":5,"queue":"test-queue"}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"stateless","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"https://fake-host/batch","method":"POST","headers":{"cf-queue-batch-bytes":"31","cf-queue-batch-count":"4","cf-queue-largest-msg":"13","content-type":"application/json","x-msg-delay-secs":"2"}},"response":{"status":200}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"durableObject","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"durableObject","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"http://foo/test","method":"GET","headers":{}},"response":{"status":200}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"durableObject","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"http://example.com/","method":"GET","headers":{"upgrade":"websocket"}}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"durableObject","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"getWebSocketEvent":{"webSocketEventType":"message"}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"durableObject","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"request":{"url":"http://example.com/hibernation","method":"GET","headers":{"upgrade":"websocket"}}}}',
      '{"wallTime":0,"cpuTime":0,"truncated":false,"executionModel":"durableObject","outcome":"ok","scriptName":null,"diagnosticsChannelEvents":[],"exceptions":[],"logs":[],"eventTimestamp":0,"event":{"getWebSocketEvent":{"wasClean":true,"code":1000,"webSocketEventType":"close"}}}',
    ];

    assert.strictEqual(expected.length, responseArr.length);
    for (let i = 0; i < expected.length; ++i) {
      // Parse JSON to compare, ignoring scheduledTime which is not deterministic
      let expectedObj = JSON.parse(expected[i]);
      let actualObj = JSON.parse(responseArr[i]);

      // Remove scheduledTime from comparison if it exists in scheduled events
      if (actualObj.event && 'scheduledTime' in actualObj.event) {
        delete actualObj.event.scheduledTime;
      }

      assert.deepStrictEqual(actualObj, expectedObj);
    }
  },
};
