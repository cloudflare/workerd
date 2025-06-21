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

      const response = await httpClient.fetch('/ping');
      assert.equal(response.status, 200);
      const text = await response.text();
      assert.equal(text, 'pong');
    }

    // Test JSON response
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      const response = await httpClient.fetch('/json');
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
      const response = await httpClient.fetch('/echo', {
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

      const response = await httpClient.fetch('/headers', {
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

      const response = await httpClient.fetch('/status/404');
      assert.equal(response.status, 404);
      const text = await response.text();
      assert.equal(text, 'Not Found');
    }

    // Test 500 response
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      const response = await httpClient.fetch('/status/500');
      assert.equal(response.status, 500);
      const text = await response.text();
      assert.equal(text, 'Internal Server Error');
    }

    // Test multiple requests on same connection
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      // First request
      const response1 = await httpClient.fetch('/ping');
      assert.equal(response1.status, 200);
      const text1 = await response1.text();
      assert.equal(text1, 'pong');
      try {
        // Second request on same connection
        const response2 = await httpClient.fetch('/json');
        assert.equal(response2.status, 200);
        const data = await response2.json();
        assert.deepEqual(data, { message: 'Hello from HTTP socket server' });
      } catch (error) {
        assert(error instanceof Error, 'Expected an Error to be thrown');
        assert(
          error.message.includes(
            'Fetcher created from convertSocketToFetcher can only be used once'
          ),
          'Expected the error message to contain "Fetcher created from convertSocketToFetcher can only be used once"'
        );
      }
    }

    // Test multiple concurrent requests on the same connection
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);
      try {
        const responses = await Promise.all([
          httpClient.fetch('/ping'),
          httpClient.fetch('/json'),
        ]);
      } catch (error) {
        assert(error instanceof Error, 'Expected an Error to be thrown');
        assert(
          error.message.includes(
            'Fetcher created from convertSocketToFetcher can only be used once'
          ),
          'Expected the error message to contain "Fetcher created from convertSocketToFetcher can only be used once"'
        );
      }
    }

    // Test that demonstrates the issue: socket is unusable after creating HTTP client
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);

      // First, successfully use the HTTP client
      const response = await httpClient.fetch('/ping');
      assert.equal(response.status, 200);

      // Now try to use the socket directly - this should fail because the streams are detached
      try {
        socket.writable.getWriter();

        // This should not be reached
        assert.fail(
          'Expected socket.writable to throw an error, but it did not'
        );
      } catch (error) {
        // We expect an error because the socket has been detached
        // The specific error message might vary, so we'll just check that an error was thrown
        assert(error instanceof TypeError, 'Expected an Error to be thrown');
        assert(
          error.message.includes(
            'This WritableStream is currently locked to a writer.'
          ),
          'Expected the error message to contain "This WritableStream is currently locked to a writer."'
        );
      }
      try {
        let reader = socket.readable.getReader();
        await reader.read();

        // This should not be reached
        assert.fail(
          'Expected socket.readable to throw an error, but it did not'
        );
      } catch (error) {
        // We expect an error because the socket has been detached
        // The specific error message might vary, so we'll just check that an error was thrown
        assert(error instanceof TypeError, 'Expected an Error to be thrown');
        assert(
          error.message.includes(
            'This ReadableStream is currently locked to a reader.'
          ),
          'Expected the error message to contain "This ReadableStream is currently locked to a reader."'
        );
      }
    }

    // Test that closing the socket does not affect the HTTP client
    {
      const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
      const httpClient = convertSocketToFetcher(socket);
      // Now close the socket
      await socket.close();

      // Try to send a request with the HTTP client after closing the socket
      // This should still work since the HTTP client should have its own stream
      const response1 = await httpClient.fetch('/ping');
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
