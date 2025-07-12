// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { connect, convertSocketToFetcher } from 'cloudflare:sockets';
import { strict as assert } from 'node:assert';

export default {
  async test(ctrl, env) {
    // Basic connectivity and GET test
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      const response = await httpClient.fetch('https://example.com/ping');
      assert.equal(response.status, 200);
      const text = await response.text();
      assert.equal(text, 'pong');
    }

    // Test JSON response
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      const response = await httpClient.fetch('https://example.com/json');
      assert.equal(response.status, 200);
      assert.equal(response.headers.get('content-type'), 'application/json');
      const data = await response.json();
      assert.deepEqual(data, { message: 'Hello from HTTP socket server' });
    }

    // Test POST request with body
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      const postData = 'Hello, world!';
      const response = await httpClient.fetch('https://example.com/echo', {
        method: 'POST',
        body: postData,
      });
      assert.equal(response.status, 200);
      const text = await response.text();
      assert.equal(text, postData);
    }

    // Test request with custom headers
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      const response = await httpClient.fetch('https://example.com/headers', {
        headers: {
          'X-Custom-Header': 'custom-value',
          'X-Another-Header': 'another-value',
        },
      });
      assert.equal(response.status, 200);
      const headers = await response.json();
      assert.equal(headers['x-custom-header'], 'custom-value');
      assert.equal(headers['x-another-header'], 'another-value');
    }

    // Test 404 response
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      const response = await httpClient.fetch('https://example.com/status/404');
      assert.equal(response.status, 404);
      const text = await response.text();
      assert.equal(text, 'Not Found');
    }

    // Test 500 response
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      const response = await httpClient.fetch('https://example.com/status/500');
      assert.equal(response.status, 500);
      const text = await response.text();
      assert.equal(text, 'Internal Server Error');
    }

    console.log('Multiple requests on the same connection');
    // Test multiple requests on same connection
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      // First request
      const response1 = await httpClient.fetch('https://example.com/ping');
      assert.equal(response1.status, 200);
      const text1 = await response1.text();
      assert.equal(text1, 'pong');
      // Second request on same connection
      const response2 = await httpClient.fetch('https://example.com/json');
      assert.equal(response2.status, 200);
      const data = await response2.json();
      assert.deepEqual(data, { message: 'Hello from HTTP socket server' });
    }

    console.log('Multiple concurrent requests on the socket');
    // Test multiple concurrent requests on the same connection
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      // TODO(immenently) Make this promise return the correct message
      assert.rejects(
        async () => {
          await Promise.all([
            httpClient.fetch('https://example.com/ping'),
            httpClient.fetch('https://example.com/json'),
          ]);
        },
        {
          name: 'Error',
          message: /internal error; reference/,
        }
      );
    }

    // Test that demonstrates the issue: socket is unusable after creating HTTP client
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      // First, successfully use the HTTP client
      const response = await httpClient.fetch('https://example.com/ping');
      assert.equal(response.status, 200);

      // Now try to use the socket directly - this should fail because the streams are detached
      assert.throws(
        () => {
          socket.writable.getWriter();
        },
        {
          name: 'TypeError',
          message: 'This WritableStream is currently locked to a writer.',
        }
      );

      assert.throws(
        () => {
          socket.readable.getReader();
        },
        {
          name: 'TypeError',
          message: 'This ReadableStream is currently locked to a reader.',
        }
      );
    }

    // Test that closing the socket does not affect the HTTP client
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);
      // Now close the socket
      await socket.close();

      // Try to send a request with the HTTP client after closing the socket
      // This should still work since the HTTP client should have its own stream
      const response1 = await httpClient.fetch('https://example.com/ping');
      assert.equal(response1.status, 200);
    }

    // Test WebSocket connection over the converted Socket
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      // Create a WebSocket connection using the HTTP client for fetch
      const ws = new WebSocket(
        `ws://localhost:${env.HTTP_SOCKET_SERVER_PORT}/`
      );

      // Test the WebSocket connection
      await new Promise((resolve, reject) => {
        // Set a timeout for the test
        const timeout = setTimeout(() => {
          reject(new Error('WebSocket test timed out after 5 seconds'));
        }, 5000);

        ws.addEventListener('open', () => {
          ws.send('Hello from test client');
        });

        ws.addEventListener('message', (event) => {
          // Verify we got the welcome message
          if (event.data === 'Welcome to WebSocket server') {
          } else if (event.data.startsWith('Echo:')) {
            clearTimeout(timeout);
            ws.close();
            resolve();
          }
        });

        ws.addEventListener('error', (error) => {
          clearTimeout(timeout);
          reject(
            new Error(`WebSocket error: ${error.message || 'Unknown error'}`)
          );
        });

        ws.addEventListener('close', (event) => {
          if (event && !event.wasClean) {
            clearTimeout(timeout);
            reject(
              new Error(
                `WebSocket closed abnormally with code ${event.code || 'unknown'}`
              )
            );
          }
        });
      });
    }
  },
};
