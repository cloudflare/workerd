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

    // The shared tail worker we configured only produces onset and outcome events, so every trace is identical here.
    // Number of traces based on how often main tail worker is invoked from previous tests
    let numTraces = 21;
    let basicTrace =
      '{"type":"onset","info":{"type":"trace","traces":[]}}{"type":"outcome","outcome":"ok","cpuTime":0,"wallTime":0}';
    assert.deepStrictEqual(
      Array.from(resposeMap.values()),
      Array(numTraces).fill(basicTrace)
    );
  },
};
