// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { connect, internalNewHttpClient } from 'cloudflare:sockets';
import { strict as assert } from 'node:assert';
import { setTimeout as sleep } from 'node:timers/promises';

// Basic connectivity and GET test
export const oneRequest = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

    const response = await httpClient.fetch('https://example.com/ping');
    assert.equal(response.status, 200);
    const text = await response.text();
    assert.equal(text, 'pong');
  },
};

export const jsonResponse = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

    const response = await httpClient.fetch('https://example.com/json');
    assert.equal(response.status, 200);
    assert.equal(response.headers.get('content-type'), 'application/json');
    const data = await response.json();
    assert.deepEqual(data, { message: 'Hello from HTTP socket server' });
  },
};

export const postRequest = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

    const postData = 'Hello, world!';
    const response = await httpClient.fetch('https://example.com/echo', {
      method: 'POST',
      body: postData,
    });
    assert.equal(response.status, 200);
    const text = await response.text();
    assert.equal(text, postData);
  },
};

export const customHeaders = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

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
  },
};

export const response404 = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

    const response = await httpClient.fetch('https://example.com/status/404');
    assert.equal(response.status, 404);
    const text = await response.text();
    assert.equal(text, 'Not Found');
  },
};

export const response500 = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

    const response = await httpClient.fetch('https://example.com/status/500');
    assert.equal(response.status, 500);
    const text = await response.text();
    assert.equal(text, 'Internal Server Error');
  },
};

export const redirect301 = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

    // TODO(cleanup) when enabling multiple fetches we can only then do redirects.
    const response = await httpClient.fetch('https://example.com/redirect');
    assert.equal(response.status, 200);
    const text = await response.text();
    assert.equal(text, 'pong');
  },
};

export const multipleRequests = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

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
  },
};

export const multipleConcurrentRequests = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

    // TODO(cleanup) when multiple fetches are enabled make sure this message is changed
    await assert.rejects(
      Promise.all([
        httpClient.fetch('https://example.com/ping', {
          signal: AbortSignal.timeout(500),
        }),
        httpClient.fetch('https://example.com/json', {
          signal: AbortSignal.timeout(500),
        }),
      ]),
      {
        name: 'Error',
        message: /internal error/,
      }
    );
  },
};

// Test that demonstrates socket is unusable after creating HTTP client
export const socketFetcherUnusable = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

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
  },
};

// Test that closing the socket does not affect the HTTP client
export const socketCloseThenFetch = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);
    // Now close the socket
    await socket.close();

    // Try to send a request with the HTTP client after closing the socket
    // This should still work since the HTTP client should have its own stream
    const response1 = await httpClient.fetch('https://example.com/ping');
    assert.equal(response1.status, 200);
  },
};

// Test that locking a reader before creating the stream has the right error.
export const lockReaderFail = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const reader = socket.readable.getReader();
    await assert.rejects(internalNewHttpClient(socket), {
      name: 'TypeError',
      message: 'The ReadableStream has been locked to a reader.',
    });
  },
};

// Test that locking a writer before creating the stream has the right error.
export const lockWriterFail = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const writer = socket.writable.getWriter();
    await assert.rejects(internalNewHttpClient(socket), {
      name: 'TypeError',
      message: 'This WritableStream is currently locked to a writer.',
    });
  },
};

/* TODO(soon) this test does not test the behavior properly
// Test that queueing up writes before creating the stream throws an error.
export const writeThenConvertFlushes = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.FLUSH_HELLO_SOCKET}`);
    const writer = socket.writable.getWriter();
    const encoder = new TextEncoder();
    await writer.write(encoder.encode('Hello'));
    writer.releaseLock();
    const httpClient = await internalNewHttpClient(socket);

    const response = await httpClient.fetch('https://example.com/ping');
    assert.equal(response.status, 200);
    const text = await response.text();
    assert.equal(text, 'pong');
  },
};
*/

/* TODO(soon) whenever we support inspecting the readable stream for data.
export const errorRemoteWrites = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.SOCKET_PARTIALLY_WRITTEN}`);
    await sleep(2000);
    const httpClient = await internalNewHttpClient(socket);
    await assert.rejects(
      httpClient.fetch('https://example.com/ping', {
        signal: AbortSignal.timeout(500),
      }),
      {
        name: 'Error',
        message: "Readable stream in http protocol starts empty, remote side wrote data that was not consumed",
      }
    );
  },
};
*/

// Test WebSocket connection over the converted Socket
export const websockets = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

    const res = await fetch(
      `http://localhost:${env.HTTP_SOCKET_SERVER_PORT}/`,
      {
        headers: {
          Connection: 'Upgrade',
          Upgrade: 'websocket',
        },
      }
    );

    const ws = res.webSocket;

    if (!ws) {
      throw new Error('WebSocket upgrade failed');
    }

    ws.accept();
    ws.send('Hello from test client');

    // Test the WebSocket connection
    await new Promise((resolve, reject) => {
      // Set a timeout for the test
      const timeout = setTimeout(() => {
        reject(new Error('WebSocket test timed out after 5 seconds'));
      }, 5000);

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
  },
};

// Test remote end destroys socket.
export const destroyRemote = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);
    await assert.rejects(httpClient.fetch('https://example.com/destroy'), {
      name: 'Error',
      message: 'Network connection lost.',
    });
  },
};

// Remote destroy first then await internalNewHttpClient
// Check if connect freezes or if a socket doesn't

// Test remote end drops socket
export const dropRemote = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);
    await assert.rejects(
      httpClient.fetch('https://example.com/drop', {
        signal: AbortSignal.timeout(500),
      }),
      {
        name: 'TimeoutError',
        message: 'The operation was aborted due to timeout',
      }
    );
    /* TODO(cleanup) after supporting multiple fetches this should throw a recognizable error
    response = await httpClient.fetch('https://example.com/ping');
    */
  },
};

export const connectOnFetcherThrows = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = await internalNewHttpClient(socket);

    assert.throws(
      () => {
        httpClient.connect('localhost:8080');
      },
      {
        name: 'TypeError',
        message:
          'connect is not something that can be done on a fetcher converted from a socket',
      }
    );
  },
};

export const startTlsSuccess = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.STARTTLS_SOCKET}`, {
      secureTransport: 'starttls',
    });

    const writer = socket.writable.getWriter();
    const reader = socket.readable.getReader();
    const encoder = new TextEncoder();
    const decoder = new TextDecoder();

    try {
      // Read HELLO greeting
      const { value: greeting } = await reader.read();
      const greetingText = decoder.decode(greeting).trim();
      console.log('startTlsSuccess: Received greeting:', greetingText);

      if (greetingText === 'HELLO') {
        console.log('startTlsSuccess: Sending HELLO_BACK');
        await writer.write(encoder.encode('HELLO_BACK\n'));

        // Read START_TLS signal
        const { value: signal } = await reader.read();
        const signalText = decoder.decode(signal).trim();
        console.log('startTlsSuccess: Received signal:', signalText);

        if (signalText === 'START_TLS') {
          console.log('startTlsSuccess: Received START_TLS, upgrading to TLS');

          // Release the reader and writer before upgrading
          reader.releaseLock();
          writer.releaseLock();

          // Upgrade to TLS
          console.log('startTlsSuccess: About to start TLS');
          const tlsSocket = socket.startTls();
          console.log('startTlsSuccess: Started TLS');

          await tlsSocket.opened;
          console.log(
            'startTlsSuccess: TLS connection established successfully'
          );

          const httpClient = await internalNewHttpClient(tlsSocket);
          const response = await httpClient.fetch('https://example.com/ping');
          assert.equal(response.status, 200);
          const text = await response.text();
          assert.equal(text, 'pong');
        }
      }
    } catch (err) {
      console.log('startTlsSuccess: Error:', err.message);
      throw err;
    }
  },
};

export const startTlsEarlyConvert = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.STARTTLS_SOCKET}`, {
      secureTransport: 'starttls',
    });

    const writer = socket.writable.getWriter();
    const reader = socket.readable.getReader();
    const encoder = new TextEncoder();
    const decoder = new TextDecoder();

    const { value: greeting } = await reader.read();
    const greetingText = decoder.decode(greeting).trim();

    if (greetingText !== 'HELLO') throw 'Wrong Handshake';
    await writer.write(encoder.encode('HELLO_BACK\n'));

    // Read START_TLS signal
    const { value: signal } = await reader.read();
    const signalText = decoder.decode(signal).trim();
    if (signalText !== 'START_TLS') throw 'Cannot Start TLS';

    // Release the reader and writer before upgrading
    reader.releaseLock();
    writer.releaseLock();

    const tlsSocket = socket.startTls();
    await assert.rejects(internalNewHttpClient(socket), {
      name: 'TypeError',
      message: 'This WritableStream is currently locked to a writer.',
    });
    const httpClient = await internalNewHttpClient(tlsSocket);
    await tlsSocket.opened;
    const response = await httpClient.fetch('https://example.com/ping');
    assert.equal(response.status, 200);
    const text = await response.text();
    assert.equal(text, 'pong');
  },
};

export const startTlsEarlySend = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.STARTTLS_SOCKET}`, {
      secureTransport: 'starttls',
    });

    const writer = socket.writable.getWriter();
    const reader = socket.readable.getReader();
    const encoder = new TextEncoder();
    const decoder = new TextDecoder();

    const { value: greeting } = await reader.read();
    const greetingText = decoder.decode(greeting).trim();

    if (greetingText !== 'HELLO') throw 'Wrong Handshake';
    await writer.write(encoder.encode('HELLO_BACK\n'));

    // Read START_TLS signal
    const { value: signal } = await reader.read();
    const signalText = decoder.decode(signal).trim();
    if (signalText !== 'START_TLS') throw 'Cannot Start TLS';

    // Release the reader and writer before upgrading
    reader.releaseLock();
    writer.releaseLock();

    const tlsSocket = socket.startTls();
    const httpClient = await internalNewHttpClient(tlsSocket);
    const response = await httpClient.fetch('https://example.com/ping');
    assert.equal(response.status, 200);
    const text = await response.text();
    assert.equal(text, 'pong');
  },
};

export const manualProtocolThenFetcher = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const writer = socket.writable.getWriter();
    const reader = socket.readable.getReader();
    const encoder = new TextEncoder();
    const decoder = new TextDecoder();

    // Manually construct and send HTTP request
    const httpRequest =
      'GET /ping HTTP/1.1\r\n' +
      'Host: example.com\r\n' +
      'Connection: keep-alive\r\n' +
      '\r\n';

    await writer.write(encoder.encode(httpRequest));

    // Read the HTTP response manually
    let responseData = '';
    let contentLength = 0;
    let headersParsed = false;

    while (true) {
      const { value, done } = await reader.read();
      if (done) break;

      const chunk = decoder.decode(value, { stream: true });
      responseData += chunk;

      if (!headersParsed && responseData.includes('\r\n\r\n')) {
        const [headers, body] = responseData.split('\r\n\r\n', 2);
        const contentLengthMatch = headers.match(/content-length:\s*(\d+)/i);
        if (contentLengthMatch) {
          contentLength = parseInt(contentLengthMatch[1]);
        }
        headersParsed = true;

        // Check if we have all the content
        if (body.length >= contentLength) {
          break;
        }
      }
    }

    // Verify the manual HTTP response
    assert(responseData.includes('HTTP/1.1 200 OK'));
    assert(responseData.includes('pong'));

    // Release locks and convert to HTTP client
    reader.releaseLock();
    writer.releaseLock();

    const httpClient = await internalNewHttpClient(socket);
    const response = await httpClient.fetch('https://example.com/json');
    assert.equal(response.status, 200);
    const data = await response.json();
    assert.deepEqual(data, { message: 'Hello from HTTP socket server' });
  },
};
