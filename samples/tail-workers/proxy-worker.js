// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';

// Global variables to store performance stats
let cppSerializationTime = 0;
let jsonStringifyTime = 0;
let sampleCount = 0;

export default class RPCProxyWorker extends WorkerEntrypoint {

  async fetch(request) {
    const url = new URL(request.url);

    if (url.pathname === '/echo') {
      // Proxy /echo requests to the tail worker via RPC
      console.log('Proxying /echo request to tail worker');
      return this.env.SERVICE.fetch(request);
    } else if (url.pathname === '/stats') {
      // Return performance statistics
      if (sampleCount === 0) {
        return new Response('No performance data available yet. Send some tail events first.', {
          status: 200,
          headers: { 'Content-Type': 'text/plain' }
        });
      }

      const avgCppTime = cppSerializationTime / sampleCount;
      const avgJsonTime = jsonStringifyTime / sampleCount;

      const stats = `Performance Statistics (${sampleCount} samples):
C++ Serialization: ${avgCppTime.toFixed(4)}ms average
JSON.stringify: ${avgJsonTime.toFixed(4)}ms average
Speedup factor: ${(avgJsonTime / avgCppTime).toFixed(2)}x
`;

      console.log('Returning performance stats:', stats);
      return new Response(stats, {
        status: 200,
        headers: { 'Content-Type': 'text/plain' }
      });
    } else {
      // Default behavior for other paths
      return this.env.SERVICE.fetch(request);
    }
  }

  async tail(events) {
    console.log('Tail event received in Proxy Worker - starting benchmark');

    // Measure JSON.stringify serialization time
    const jsonStart = performance.now();
    let jsonEvents = events.map((event) => structuredClone(event.toJSON()));
    // let tmp1 = structuredClone(JSON.parse(JSON.stringify(events)));
    const jsonEnd = performance.now();
    const jsonTime = jsonEnd - jsonStart;

    // Measure C++ serialization time (direct object passing)
    const cppStart = performance.now();
    let tmp2 = structuredClone(events);
    const cppEnd = performance.now();
    const cppTime = cppEnd - cppStart;

    // Update global statistics
    cppSerializationTime += cppTime;
    jsonStringifyTime += jsonTime;
    sampleCount++;

    console.log(`Sample ${sampleCount} - C++: ${cppTime.toFixed(4)}ms, JSON: ${jsonTime.toFixed(4)}ms`);
  }
}
