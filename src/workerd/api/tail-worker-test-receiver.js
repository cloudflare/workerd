// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

let resposeMap = new Map();

export default {
  // https://developers.cloudflare.com/workers/observability/logs/tail-workers/
  tailStream(event, env, ctx) {
    // Onset event, must be singleton

    // Set inner spanId to zero to have deterministic output.
    event.event.spanId = '0000000000000000';

    resposeMap.set(event.spanContext.traceId, JSON.stringify(event.event));
    return (event) => {
      let cons = resposeMap.get(event.spanContext.traceId);
      resposeMap.set(
        event.spanContext.traceId,
        cons + JSON.stringify(event.event)
      );
    };
  },
};

export const test = {
  async test() {
    // HACK: The prior tests terminates once the scheduled() invocation has returned a response, but
    // propagating the outcome of the invocation may take longer. Wait briefly so this can go ahead.
    await scheduler.wait(50);

    // The shared tail worker we configured only produces onset and outcome events, so every trace is identical here.
    // Number of traces based on how often main tail worker is invoked from previous tests
    let numTraces = 26;
    let basicTrace =
      '{"type":"onset","executionModel":"stateless","spanId":"0000000000000000","scriptTags":[],"info":{"type":"trace","traces":[]}}{"type":"return"}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}';
    assert.deepStrictEqual(
      Array.from(resposeMap.values()),
      Array(numTraces).fill(basicTrace)
    );
  },
};
