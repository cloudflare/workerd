// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as assert from 'node:assert';

// Track if we've received logs in the tail handler
let receivedLogsInTail = false;

export default {
  fetch(request, env) {
    // Log some messages that should be captured by the tail handler
    console.log('Self-logger test: Message 1');
    console.log('Self-logger test: Message 2');
    console.error(new Error('Self-logger test: Error message'));

    return new Response('Self-logger test completed');
  },

  // Tail handler in the same worker that generates logs
  tail(events) {
    console.log(`Self-logger received ${events.length} events`);

    // Check that we have the logs we expect
    const logs = [];
    for (const event of events) {
      if (event.logs) {
        for (const log of event.logs) {
          logs.push(log.message[0]);
        }
      }
    }

    // Verify we're seeing the logs from our fetch handler
    const foundMessage1 = logs.some((log) => log.includes('Message 1'));
    const foundMessage2 = logs.some((log) => log.includes('Message 2'));
    const foundError = logs.some((log) => log.includes('Error message'));

    // Assert that we found our logs (this will throw if the test fails)
    assert.ok(foundMessage1, 'Should have received "Message 1" in logs');
    assert.ok(foundMessage2, 'Should have received "Message 2" in logs');
    assert.ok(foundError, 'Should have received "Error message" in logs');
    // If we get here, it means the self-logging functionality is working
    receivedLogsInTail = true;
  },
};

export const test = {
  async test(ctrl, env, ctx) {
    // This test simply verifies that the self-logger worked
    // If the tail handler was called, receivedLogsInTail will be true
    const response = await env.SERVICE.fetch('http://example.com/');
    const body = await response.text();
    assert.strictEqual(body, 'Self-logger test completed');
    await scheduler.wait(100);
    assert.strictEqual(
      receivedLogsInTail,
      true,
      'Tail handler should have been called'
    );
  },
};
