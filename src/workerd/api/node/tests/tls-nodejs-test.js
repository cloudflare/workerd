// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.
import tls from 'node:tls';
import { strictEqual, ok, rejects, throws } from 'node:assert';
import { once } from 'node:events';
import net from 'node:net';

// Tests are taken from
// https://github.com/nodejs/node/blob/304743655d5236c2edc39094336ee2667600b684/test/parallel/test-tls-connect-abort-controller.js
export const tlsConnectAbortController = {
  async test() {
    // Our tests differ from Node.js
    // We don't check for abortSignal listener count because it's not supported.
    const connectOptions = (signal) => ({
      port: 8888,
      host: 'localhost',
      signal,
    });

    const assertAbort = (socket, testName) => {
      return rejects(
        () => once(socket, 'close'),
        {
          name: 'AbortError',
        },
        `AbortError should have been thrown on ${testName}`
      );
    };

    async function postAbort() {
      const ac = new AbortController();
      const { signal } = ac;
      const socket = tls.connect(connectOptions(signal));
      ac.abort();
      await assertAbort(socket, 'postAbort');
    }

    async function preAbort() {
      const ac = new AbortController();
      const { signal } = ac;
      ac.abort();
      const socket = tls.connect(connectOptions(signal));
      await assertAbort(socket, 'preAbort');
    }

    async function tickAbort() {
      const ac = new AbortController();
      const { signal } = ac;
      const socket = tls.connect(connectOptions(signal));
      setImmediate(() => ac.abort());
      await assertAbort(socket, 'tickAbort');
    }

    async function testConstructor() {
      const ac = new AbortController();
      const { signal } = ac;
      ac.abort();
      const socket = new tls.TLSSocket(undefined, connectOptions(signal));
      await assertAbort(socket, 'testConstructor');
    }

    async function testConstructorPost() {
      const ac = new AbortController();
      const { signal } = ac;
      const socket = new tls.TLSSocket(undefined, connectOptions(signal));
      ac.abort();
      await assertAbort(socket, 'testConstructorPost');
    }

    async function testConstructorPostTick() {
      const ac = new AbortController();
      const { signal } = ac;
      const socket = new tls.TLSSocket(undefined, connectOptions(signal));
      setImmediate(() => ac.abort());
      await assertAbort(socket, 'testConstructorPostTick');
    }

    await postAbort();
    await preAbort();
    await tickAbort();
    await testConstructor();
    await testConstructorPost();
    await testConstructorPostTick();
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/304743655d5236c2edc39094336ee2667600b684/test/parallel/test-tls-connect-allow-half-open-option.js
export const connectAllowHalfOpenOption = {
  async test() {
    {
      const socket = tls.connect({ port: 42, lookup() {} });
      strictEqual(socket.allowHalfOpen, false);
    }

    {
      const socket = tls.connect({
        port: 42,
        allowHalfOpen: false,
        lookup() {},
      });
      strictEqual(socket.allowHalfOpen, false);
    }

    {
      const { promise, resolve } = Promise.withResolvers();
      const socket = tls.connect(
        {
          port: 8888,
          allowHalfOpen: true,
        },
        () => {
          let message = '';

          socket.on('data', (chunk) => {
            message += chunk;
          });

          socket.on('end', () => {
            strictEqual(message, 'Hello');
            resolve();
          });

          socket.write('Hello');
          socket.end();
        }
      );

      socket.setEncoding('utf8');

      await promise;
    }
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/755e4603fd1679de72d250514ea5096b272ae8d6/test/parallel/test-tls-connect-simple.js
export const tlsConnectSimple = {
  async test() {
    const promise1 = Promise.withResolvers();
    const promise2 = Promise.withResolvers();
    const options = { port: 8888 };
    const client1 = tls.connect(options, function () {
      client1.end();
      promise1.resolve();
    });
    const client2 = tls.connect(options);
    client2.on('secureConnect', function () {
      client2.end();
      promise2.resolve();
    });
    await Promise.all([promise1.promise, promise2.promise]);
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/755e4603fd1679de72d250514ea5096b272ae8d6/test/parallel/test-tls-connect-timeout-option.js
export const tlsConnectTimeoutOption = {
  async test() {
    const socket = tls.connect({
      port: 8888,
      lookup: () => {},
      timeout: 1000,
    });

    strictEqual(socket.timeout, 1000);
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/755e4603fd1679de72d250514ea5096b272ae8d6/test/parallel/test-tls-connect-no-host.js
export const tlsConnectNoHost = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const socket = tls.connect(
      {
        port: 8888,
        // No host set here. 'localhost' is the default,
        // but tls.checkServerIdentity() breaks before the fix with:
        // Error: Hostname/IP doesn't match certificate's altnames:
        //   "Host: undefined. is not cert's CN: localhost"
      },
      function () {
        ok(socket.authorized);
        resolve();
      }
    );
    await promise;
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/755e4603fd1679de72d250514ea5096b272ae8d6/test/parallel/test-tls-connect-given-socket.js
export const tlsConnectGivenSocket = {
  async test() {
    const promises = [];
    let waiting = 2;
    function establish(socket, shouldNotCallCallback = false) {
      const { promise, resolve } = Promise.withResolvers();
      promises.push(promise);
      const client = tls.connect(
        {
          socket: socket,
        },
        () => {
          if (shouldNotCallCallback) {
            reject(new Error('should not have called tls.connect() callback'));
            return;
          }
          let data = '';
          client
            .on('data', (chunk) => {
              data += chunk.toString();
            })
            .on('end', () => {
              strictEqual(data, 'Hello');
              if (--waiting === 0) {
                resolve();
              }
            });
        }
      );

      if (shouldNotCallCallback) {
        queueMicrotask(() => resolve());
      }

      return client;
    }

    const port = 8887;
    // Immediate death socket
    const immediateDeath = net.connect(port);
    establish(immediateDeath, true).destroy();

    // Outliving
    const outlivingTCPPromise = Promise.withResolvers();
    const outlivingTCP = net.connect(port, () => {
      outlivingTLS.destroy();
      next();
      outlivingTCPPromise.resolve();
    });
    promises.push(outlivingTCPPromise.promise);
    const outlivingTLS = establish(outlivingTCP, true);

    function next() {
      // Already connected socket
      const { promise, resolve } = Promise.withResolvers();
      const connected = net.connect(port, () => {
        establish(connected);
        resolve();
      });
      promises.push(promise);

      // Connecting socket
      const connecting = net.connect(port);
      establish(connecting);
    }

    await Promise.all(promises);
  },
};
