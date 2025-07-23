// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { connect, convertSocketToFetcher } from 'cloudflare:sockets';
import { strict as assert } from 'node:assert';
import { setTimeout as sleep } from 'node:timers/promises';

// Basic connectivity and GET test
export const oneRequest = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);

    const response = await httpClient.fetch('https://example.com/ping');
    assert.equal(response.status, 200);
    const text = await response.text();
    assert.equal(text, 'pong');
  },
};

export const jsonResponse = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);

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
    const httpClient = convertSocketToFetcher(socket);

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
  },
};

export const response404 = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);

    const response = await httpClient.fetch('https://example.com/status/404');
    assert.equal(response.status, 404);
    const text = await response.text();
    assert.equal(text, 'Not Found');
  },
};

export const response500 = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);

    const response = await httpClient.fetch('https://example.com/status/500');
    assert.equal(response.status, 500);
    const text = await response.text();
    assert.equal(text, 'Internal Server Error');
  },
};

export const multipleRequests = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);

    // First request
    const response1 = await httpClient.fetch('https://example.com/ping');
    assert.equal(response1.status, 200);
    const text1 = await response1.text();
    assert.equal(text1, 'pong');

    // Second request on same connection
    // TODO(cleanup) we want this to fail during this initial implementation but later we should
    // support multiple fetches (perhaps behind a compat flag)
    await assert.rejects(httpClient.fetch('https://example.com/json'), {
      name: 'Error',
      message:
        'Fetcher created from convertSocketToFetcher can only be used once',
    });
    /*
     * assert.equal(response2.status, 200);
     * const data = await response2.json();
     * assert.deepEqual(data, { message: 'Hello from HTTP socket server' });
     */
  },
};

export const multipleConcurrentRequests = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);

    // TODO(cleanup) when multiple fetches are enabled make sure this message is changed
    await assert.rejects(
      Promise.all([
        httpClient.fetch('https://example.com/ping'),
        httpClient.fetch('https://example.com/json'),
      ]),
      {
        name: 'Error',
        message:
          'Fetcher created from convertSocketToFetcher can only be used once',
      }
    );
  },
};

// Test that demonstrates socket is unusable after creating HTTP client
export const socketFetcherUnusable = {
  async test(ctrl, env, ctx) {
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
  },
};

// Test that closing the socket does not affect the HTTP client
export const socketCloseThenFetch = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);
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
    assert.throws(
      () => {
        convertSocketToFetcher(socket);
      },
      {
        name: 'TypeError', //TODO(immediately) Do we want this to be a TypeError.
        message: 'The ReadableStream has been locked to a reader.',
      }
    );
  },
};

// Test that locking a writer before creating the stream has the right error.
export const lockWriterFail = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const writer = socket.writable.getWriter();
    assert.throws(
      () => {
        convertSocketToFetcher(socket);
      },
      {
        name: 'TypeError', //TODO(immediately) Do we want this to be a TypeError.
        message: 'This WritableStream is currently locked to a writer.',
        //TODO(immediately) Why are the errors different?
      }
    );
  },
};

// try startTls and see if it fails
/*
export const startTlsFail = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`, {
      secureTransport: 'on',
    });

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
  },
};
*/

// Test remote end drops socket.
/*
export const dropRemote = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);
    try {
      await httpClient.fetch('https://example.com/drop');
    } catch(error) {
      console.log(error.name);
      console.log(error.message);
    }
    await assert.rejects(httpClient.fetch('https://example.com/drop'), {
      name: 'Error',
      message: 'Network connection lost.',
    });
  }
}
*/

async function tryPromise(promise) {
  try {
    await promise;
  } catch (error) {
    console.log(error.name);
    console.log(error.message);
  }
}

// Test WebSocket connection over the converted Socket
export const websockets = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);

    // Create a WebSocket connection using the HTTP client for fetch
    const ws = new WebSocket(`ws://localhost:${env.HTTP_SOCKET_SERVER_PORT}/`);

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
  },
};

// Test remote end destroys socket.
export const destroyRemote = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);
    await assert.rejects(httpClient.fetch('https://example.com/destroy'), {
      name: 'Error',
      message: 'Network connection lost.',
    });
  },
};

// Test remote end drops socket
export const dropRemote = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.HTTP_SOCKET_SERVER_PORT}`);
    const httpClient = convertSocketToFetcher(socket);
    await assert.rejects(
      httpClient.fetch('https://example.com/drop', {
        signal: AbortSignal.timeout(500),
      }),
      {
        name: 'TimeoutError',
        message: 'The operation was aborted due to timeout',
      }
    );
  },
};

export const errorRemoteWrites = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.SOCKET_PARTIALLY_WRITTEN}`);
    await sleep(2000);
    const httpClient = convertSocketToFetcher(socket);
    await assert.rejects(
      httpClient.fetch('https://example.com/ping', {
        signal: AbortSignal.timeout(500),
      }),
      {
        name: 'Error',
        // TODO(immenently) Right now we are getting a proctocol Error do we instead want to error
        // when converting the Socket to the fetcher and see if there are already unconsumed writes?
        // If we intend to do fetcher re-use I don't think this is possible because connection
        // attempts really only happen on fetch not on convertSocketToFetcher
        message: /internal error/,
      }
    );
  },
};

/*
export const startTlsSuccess = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.START_TLS_SOCKET}`, { secureTransport: 'starttls'});

    const writer = socket.writable.getWriter();
    const encoder = new TextEncoder();
    await writer.write(encoder.encode("starttls"));
    await writer.releaseLock();

    const tlsSocket = socket.startTls();
    console.log("Awaiting startTls");
    await tlsSocket.opened;

    const httpClient = convertSocketToFetcher(tlsSocket);
    const response = await httpClient.fetch('https://example.com/ping');
    assert.equal(response.status, 200);
    const text = await response.text();
    assert.equal(text, 'pong');
  }
};
export const errorRemoteReads = {
  async test(ctrl, env, ctx) {
    const socket = connect(`localhost:${env.SOCKET_PARTIALLY_WRITTEN}`);
    const httpClient = convertSocketToFetcher(socket);
    await assert.rejects(httpClient.fetch('https://example.com/ping', {signal: AbortSignal.timeout(500)}), {
      name: 'TimeoutError',
      message: 'The operation was aborted due to timeout',
    });
  }
};
*/
