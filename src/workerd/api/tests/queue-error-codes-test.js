// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import assert from 'node:assert';
import { Buffer } from 'node:buffer';

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);

    if (pathname === '/message') {
      const body = await request.text();

      // Return error with headers defined
      if (body.includes('with-headers')) {
        return new Response('', {
          status: 503,
          headers: {
            'CF-Queues-Error-Code': '10503',
            'CF-Queues-Error-Cause': 'Service temporarily unavailable',
          },
        });
      }

      // Return error without headers
      if (body.includes('no-headers')) {
        return new Response('', { status: 503 });
      }

      // Default success
      return new Response('');
    }

    if (pathname === '/batch') {
      const body = await request.json();

      if (body.messages.length > 0) {
        const firstMessage = Buffer.from(
          body.messages[0].body,
          'base64'
        ).toString();

        // Return error with headers defined
        if (firstMessage.includes('with-headers')) {
          return new Response('', {
            status: 503,
            headers: {
              'CF-Queues-Error-Code': '10503',
              'CF-Queues-Error-Cause': 'Service temporarily unavailable',
            },
          });
        }

        // Return error without headers
        if (firstMessage.includes('no-headers')) {
          return new Response('', { status: 503 });
        }
      }

      // Default success
      return new Response('');
    }

    return new Response('Not Found', { status: 404 });
  },

  async test(ctrl, env, ctx) {
    const flagEnabled = env.ERROR_CODES_FLAG;

    // Test with error headers defined
    try {
      await env.QUEUE.send('with-headers', { contentType: 'text' });
      assert.fail('Expected send() to throw');
    } catch (error) {
      if (flagEnabled) {
        // Flag ON + Headers defined → detailed error with code
        assert.strictEqual(
          error.message,
          'Service temporarily unavailable (10503)'
        );
      } else {
        // Flag OFF + Headers defined → generic error (ignores headers)
        assert.strictEqual(
          error.message,
          'Queue send failed: Service Unavailable'
        );
      }
    }

    // Test without error headers
    try {
      await env.QUEUE.send('no-headers', { contentType: 'text' });
      assert.fail('Expected send() to throw');
    } catch (error) {
      if (flagEnabled) {
        // Flag ON + Headers missing → default error with code 15000
        assert.strictEqual(error.message, 'Unknown Internal Error (15000)');
      } else {
        // Flag OFF + Headers missing → generic error
        assert.strictEqual(
          error.message,
          'Queue send failed: Service Unavailable'
        );
      }
    }

    // Test sendBatch with headers defined
    try {
      await env.QUEUE.sendBatch([
        { body: 'with-headers', contentType: 'text' },
      ]);
      assert.fail('Expected sendBatch() to throw');
    } catch (error) {
      if (flagEnabled) {
        // Flag ON + Headers defined → detailed error with code
        assert.strictEqual(
          error.message,
          'Service temporarily unavailable (10503)'
        );
      } else {
        // Flag OFF + Headers defined → generic error (ignores headers)
        assert.strictEqual(
          error.message,
          'Queue sendBatch failed: Service Unavailable'
        );
      }
    }

    // Test sendBatch without headers
    try {
      await env.QUEUE.sendBatch([{ body: 'no-headers', contentType: 'text' }]);
      assert.fail('Expected sendBatch() to throw');
    } catch (error) {
      if (flagEnabled) {
        // Flag ON + Headers missing → default error with code 15000
        assert.strictEqual(error.message, 'Unknown Internal Error (15000)');
      } else {
        // Flag OFF + Headers missing → generic error
        assert.strictEqual(
          error.message,
          'Queue sendBatch failed: Service Unavailable'
        );
      }
    }
  },
};
