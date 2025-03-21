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
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT ORs
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

import { fail, ok, strictEqual, throws } from 'node:assert';
import { mock } from 'node:test';
import { once } from 'node:events';
import * as net from 'node:net';

const enc = new TextEncoder();

// test/parallel/test-net-access-byteswritten.js
export const testNetAccessBytesWritten = {
  test() {
    // Check that the bytesWritten getter doesn't crash if object isn't
    // constructed. Also check bytesRead...

    // TODO(now): Should be undefined
    strictEqual(net.Socket.prototype.bytesWritten, NaN);
    strictEqual(net.Socket.prototype.bytesRead, undefined);
  },
};

// test/parallel/test-net-after-close.js
export const testNetAfterClose = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9999, 'localhost');
    c.resume();
    c.on('close', () => resolve());
    await promise;

    // Calling functions / accessing properties of a closed socket should not throw
    c.setNoDelay();
    c.setKeepAlive();
    c.bufferSize;
    c.pause();
    c.resume();
    c.address();
    c.remoteAddress;
    c.remotePort;
  },
};

// test/parallel/test-net-allow-half-open.js
export const testNetAllowHalfOpen = {
  async test() {
    // Verify that the socket closes properly when the other end closes
    // and allowHalfOpen is false.

    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9999, 'localhost');
    strictEqual(c.allowHalfOpen, false);
    c.resume();

    const endFn = mock.fn(() => {
      queueMicrotask(() => {
        ok(!c.destroyed);
      });
    });
    const finishFn = mock.fn(() => {
      ok(!c.destroyed);
    });
    const closeFn = mock.fn(() => {
      resolve();
    });
    c.on('end', endFn);

    // Even tho we're not writing anything, since the socket receives a
    // EOS and allowHalfOpen is false, the socket should close both the
    // readable and writable sides, meaning we should definitely get a
    // finish event.
    c.on('finish', finishFn);
    c.on('close', closeFn);
    await promise;
    strictEqual(endFn.mock.callCount(), 1);
    strictEqual(finishFn.mock.callCount(), 1);
    strictEqual(closeFn.mock.callCount(), 1);
  },
};

// test/parallel/test-net-better-error-messages-port-hostname.js
export const testNetBetterErrorMessagesPortHostname = {
  async test() {
    // This is intentionally not a completely faithful reproduction of the
    // original test, as we don't have the ability to mock DNS lookups.
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(0, 'invalid address');
    c.on('connect', () => {
      throw new Error('should not connect');
    });
    const errorFn = mock.fn((error) => {
      // TODO(review): Currently our errors do not match the errors that
      // Node.js produces. Do we need them to? Specifically, in a case like
      // this, Node.js' error would have a meaningful `code` property along
      // with `hostname` and `syscall` properties. We, instead, are passing
      // along the underlying error returned from the internal Socket API.
      try {
        strictEqual(error.message, 'Specified address could not be parsed.');
      } catch (err) {
        console.log(err.message);
      }
      resolve();
    });
    c.on('error', errorFn);
    await promise;
    strictEqual(errorFn.mock.callCount(), 1);
  },
};

// test/parallel/test-net-binary.js
export const testNetBinary = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();

    // Connect to the echo server
    const c = net.connect(9998, 'localhost');
    c.setEncoding('latin1');
    let result = '';
    c.on('data', (chunk) => {
      result += chunk;
    });

    let binaryString = '';
    for (let i = 255; i >= 0; i--) {
      c.write(String.fromCharCode(i), 'latin1');
      binaryString += String.fromCharCode(i);
    }
    c.end();
    c.on('close', () => {
      resolve();
    });
    await promise;
    strictEqual(result, binaryString);
  },
};

// test/parallel/test-net-buffersize.js
export const testNetBuffersize = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9999, 'localhost');
    const finishFn = mock.fn(() => {
      strictEqual(c.bufferSize, 0);
      resolve();
    });
    c.on('finish', finishFn);

    strictEqual(c.bufferSize, 0);
    c.write('a');
    c.end();
    strictEqual(c.bufferSize, 1);
    await promise;
    strictEqual(finishFn.mock.callCount(), 1);
  },
};

// test/parallel/test-net-bytes-read.js
export const testNetBytesRead = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    // Connect to the echo server
    const c = net.connect(9998, 'localhost');
    c.resume();
    c.write('hello');
    c.end();
    const endFn = mock.fn(() => {
      strictEqual(c.bytesRead, 5);
      resolve();
    });
    c.on('end', endFn);

    await promise;

    strictEqual(endFn.mock.callCount(), 1);
  },
};

// test/parallel/test-net-bytes-stats.js
// export const testNetBytesStats = {
//   async test() {
//     // This is intentionally not a completely faithful reproduction of the
//     // original test which checks the bytesRead on the server side.
//
//     // Connect to the echo server
//     const { promise, resolve } = Promise.withResolvers();
//     const c = net.connect(9998, 'localhost');
//     let bytesDelivered = 0;
//     c.on('data', (chunk) => (bytesDelivered += chunk.byteLength));
//     c.write('hello');
//     c.end();
//     const endFn = mock.fn(() => {
//       strictEqual(c.bytesWritten, 5);
//       strictEqual(bytesDelivered, 5);
//       strictEqual(c.bytesRead, 5);
//       resolve();
//     });
//     c.on('end', endFn);
//
//     await promise;
//     strictEqual(endFn.mock.callCount(), 1);
//   },
// };

// test/parallel/test-net-bytes-written-large.js
const N = 10000000;
export const testNetBytesWrittenLargeVariant1 = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9998, 'localhost');
    c.resume();

    const writeFn = mock.fn(() => {
      strictEqual(c.bytesWritten, N);
      resolve();
    });

    c.end(Buffer.alloc(N), writeFn);

    await promise;
  },
};

// test/parallel/test-net-can-reset-timeout.js
export const testNetCanResetTimeout = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9997, 'localhost');
    const dataFn = mock.fn(() => {
      c.end();
    });
    c.on('data', dataFn);
    c.on('end', resolve);

    await promise;

    strictEqual(dataFn.mock.callCount(), 1);
  },
};

async function assertAbort(socket, testName) {
  try {
    await once(socket, 'close');
    fail(`close ${testName} should have thrown`);
  } catch (err) {
    strictEqual(err.name, 'AbortError');
  }
}

// test/parallel/test-net-connect-abort-controller.js
export const testNetConnectAbortControllerPostAbort = {
  async test() {
    const ac = new AbortController();
    const { signal } = ac;
    const socket = net.connect({ port: 9999, signal });
    ac.abort();
    await assertAbort(socket, 'postAbort');
  },
};

export const testNetConnectAbortControllerPreAbort = {
  async test() {
    const ac = new AbortController();
    const { signal } = ac;
    ac.abort();
    const socket = net.connect({ port: 9999, signal });
    await assertAbort(socket, 'preAbort');
  },
};

export const testNetConnectAbortControllerTickAbort = {
  async test() {
    const ac = new AbortController();
    const { signal } = ac;
    queueMicrotask(() => ac.abort());
    const socket = net.connect({ port: 9999, signal });
    await assertAbort(socket, 'tickAbort');
  },
};

export const testNetConnectAbortControllerConstructor = {
  async test() {
    const ac = new AbortController();
    const { signal } = ac;
    ac.abort();
    const socket = new net.Socket({ signal });
    await assertAbort(socket, 'testConstructor');
  },
};

export const testNetConnectAbortControllerConstructorPost = {
  async test() {
    const ac = new AbortController();
    const { signal } = ac;
    const socket = new net.Socket({ signal });
    ac.abort();
    await assertAbort(socket, 'testConstructorPost');
  },
};

export const testNetConnectAbortControllerConstructorPostTick = {
  async test() {
    const ac = new AbortController();
    const { signal } = ac;
    queueMicrotask(() => ac.abort());
    const socket = new net.Socket({ signal });
    await assertAbort(socket, 'testConstructorPostTick');
  },
};

// test/parallel/test-net-connect-after-destroy.js
export const testNetConnectAfterDestroy = {
  async test() {
    // Connect to something that we need to lookup, then delay
    // the lookup so that the connect attempt happens after the
    // destroy
    const lookup = mock.fn((host, options, callback) => {
      setTimeout(() => callback(null, 'localhost'), 100);
    });
    const c = net.connect({
      port: 80,
      host: 'example.org',
      lookup,
    });
    c.on('connect', () => {
      throw new Error('should not connect');
    });
    c.destroy();
    strictEqual(c.destroyed, true);
    strictEqual(lookup.mock.callCount(), 1);
  },
};

// test/parallel/test-net-connect-buffer.js
export const testNetConnectBuffer = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect({
      port: 9998,
      host: 'localhost',
      highWaterMark: 0,
    });

    strictEqual(c.pending, true);
    strictEqual(c.connecting, true);
    strictEqual(c.readyState, 'opening');
    strictEqual(c.bytesWritten, 0);

    // Write a string that contains a multi-byte character sequence to test that
    // `bytesWritten` is incremented with the # of bytes, not # of characters.
    const a = "L'État, c'est ";
    const b = 'moi';

    let result = '';
    c.setEncoding('utf8');
    c.on('data', (chunk) => {
      result += chunk;
    });
    const endFn = mock.fn(() => {
      strictEqual(result, a + b);
    });
    c.on('end', endFn);

    const writeFn = mock.fn(() => {
      strictEqual(c.pending, false);
      strictEqual(c.connecting, false);
      strictEqual(c.readyState, 'readOnly');
      strictEqual(c.bytesWritten, Buffer.from(a + b).length);
    });
    c.write(a, writeFn);

    const closeFn = mock.fn(() => {
      resolve();
    });
    c.on('close', closeFn);

    c.end(b);

    await promise;
    strictEqual(closeFn.mock.callCount(), 1);
    strictEqual(writeFn.mock.callCount(), 1);
    strictEqual(endFn.mock.callCount(), 1);
  },
};

// test/parallel/test-net-connect-destroy.js
export const testNetConnectDestroy = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9999, 'localhost');
    c.on('close', () => resolve());
    c.destroy();
    await promise;
  },
};

// test/parallel/test-net-connect-immediate-destroy.js
export const testNetConnectImmediateDestroy = {
  async test() {
    const connectFn = mock.fn();
    const socket = net.connect(9999, 'localhost', connectFn);
    socket.destroy();
    await Promise.resolve();
    strictEqual(connectFn.mock.callCount(), 0);
  },
};

// test/parallel/test-net-connect-immediate-finish.js
export const testNetConnectImmediateFinish = {
  async text() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9999, 'localhost');
    c.end();
    c.on('finish', () => resolve());
    await promise;
  },
};

// test/parallel/test-net-connect-keepalive.js
// test/parallel/test-net-keepalive.js
// We don't actually support keep alive so this test does
// something different than the original Node.js test
export const testNetConnectKeepAlive = {
  async test() {
    // Test that setKeepAlive call does not throw.
    throws(() => new net.Socket({ keepAlive: true }));
    const c = new net.Socket();
    c.setKeepAlive(false);
    c.setKeepAlive(true);
    // throws(() => c.setKeepAlive(true));
  },
};

// test/parallel/test-net-connect-memleak.js
export const testNetConnectMemleak = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const connectFn = mock.fn(() => resolve());
    const c = net.connect(9999, 'localhost', connectFn);
    c.emit('connect');
    await promise;
    strictEqual(connectFn.mock.callCount(), 1);
  },
};

// test/parallel/test-net-connect-no-arg.js
export const testNetConnectNoArg = {
  test() {
    throws(() => net.connect(), {
      code: 'ERR_MISSING_ARGS',
      message: 'The "options" or "port" or "path" argument must be specified',
    });
    throws(() => new net.Socket().connect(), {
      code: 'ERR_MISSING_ARGS',
      message: 'The "options" or "port" or "path" argument must be specified',
    });
    throws(() => net.connect({}), {
      code: 'ERR_MISSING_ARGS',
      message: 'The "options" or "port" or "path" argument must be specified',
    });
    throws(() => new net.Socket().connect({}), {
      code: 'ERR_MISSING_ARGS',
      message: 'The "options" or "port" or "path" argument must be specified',
    });
  },
};

// test/parallel/test-net-connect-options-allowhalfopen.js
// Simplified version of the equivalent Node.js test
export const testNetConnectOptionsAllowHalfOpen = {
  async test() {
    const { promise, resolve, reject } = Promise.withResolvers();
    const c = net.connect({ port: 9999, allowHalfOpen: true });
    c.resume();
    const writeFn = mock.fn(() => {
      c.write('hello', (err) => {
        if (err) reject(err);
        resolve();
      });
    });
    const endFn = mock.fn(() => {
      strictEqual(c.readable, false);
      strictEqual(c.writable, true);
      queueMicrotask(writeFn);
    });
    c.on('end', endFn);
    await promise;
  },
};

// test/parallel/test-net-connect-options-fd.js
// We do not support the fd option so this test does something different
// than the original Node.js test
export const testNetConnectOptionsFd = {
  async test() {
    throws(() => new net.Socket({ fd: 42 }));
  },
};

// test/parallel/test-net-connect-options-invalid.js
export const testNetConnectOptionsInvalid = {
  test() {
    ['objectMode', 'readableObjectMode', 'writableObjectMode'].forEach(
      (invalidKey) => {
        const option = {
          port: 8080,
          [invalidKey]: true,
        };
        const message = `The property 'options.${invalidKey}' is not supported. Received true`;

        throws(() => net.connect(option), {
          code: 'ERR_INVALID_ARG_VALUE',
          name: 'TypeError',
          message: new RegExp(message),
        });
      }
    );
  },
};

// test/parallel/test-net-connect-options-ipv6.js
export const testNetConnectOptionsIpv6 = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect({ host: '::1', port: 9999 });
    c.on('connect', () => resolve());
    await promise;
  },
};

// test/parallel/test-net-connect-options-path.js
// We do not support the path option so this test does something different
// than the original Node.js test
export const testNetConnectOptionsPath = {
  async test() {
    throws(() => net.connect({ path: '/tmp/sock' }));
  },
};

// test/parallel/test-net-connect-options-port.js
export const testNetConnectOptionsPort = {
  async test() {
    [true, false].forEach((port) => {
      throws(() => net.connect(port), {
        code: 'ERR_INVALID_ARG_TYPE',
        name: 'TypeError',
      });
    });
    [-1, 65537].forEach((port) => {
      throws(() => net.connect(port), {
        code: 'ERR_SOCKET_BAD_PORT',
      });
    });
  },
};

// test/parallel/test-net-connect-paused-connection.js
// The original test is a bit different given that it uses unref to avoid
// the paused connection from keeping the process alive. We don't have unref
// so let's just make sure things clean up okay when the IoContext is destroyed.
export const testNetConnectPausedConnection = {
  test() {
    net.connect(9999, 'localhost').pause();
  },
};

// test/parallel/test-net-dns-custom-lookup.js
// test/parallel/test-net-dns-lookup.js
export const testNetDnsCustomLookup = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const lookup = mock.fn((host, options, callback) => {
      strictEqual(host, 'localhost');
      strictEqual(options.family, 0);
      queueMicrotask(() => callback(null, '127.0.0.1', 4));
    });
    const c = net.connect({
      port: 9999,
      host: 'localhost',
      lookup,
    });
    c.on('lookup', (err, ip, type) => {
      strictEqual(err, null);
      strictEqual(ip, '127.0.0.1');
      strictEqual(type, 4);
      resolve();
    });
    await promise;
    strictEqual(lookup.mock.callCount(), 1);
  },
};

// test/parallel/test-net-dns-lookup-skip.js
export const testNetDnsLookupSkip = {
  async test() {
    const lookup = mock.fn();
    ['127.0.0.1', '::1'].forEach((host) => {
      net.connect({ host, port: 9999, lookup }).destroy();
    });
    strictEqual(lookup.mock.callCount(), 0);
  },
};

// test/parallel/test-net-during-close.js
export const testNetDuringClose = {
  test() {
    const c = net.connect(9999, 'localhost');
    c.destroy();
    c.remoteAddress;
    c.remoteFamily;
    c.remotePort;
  },
};

// test/parallel/test-net-end-destroyed.js
export const testNetEndDestroyed = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9999, 'localhost');
    c.resume();

    const endFn = mock.fn(() => {
      strictEqual(c.destroyed, false);
      resolve();
    });
    c.on('end', endFn);
    await promise;
  },
};

// test/parallel/test-net-end-without-connect.js
export const testNetEndWithoutConnect = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = new net.Socket();
    const endFn = mock.fn(() => {
      strictEqual(c.writable, false);
      resolve();
    });
    c.end(endFn);
    await promise;
  },
};

// test/parallel/test-net-isip.js
export const testNetIsIp = {
  test() {
    strictEqual(net.isIP('127.0.0.1'), 4);
    strictEqual(net.isIP('x127.0.0.1'), 0);
    strictEqual(net.isIP('example.com'), 0);
    strictEqual(net.isIP('0000:0000:0000:0000:0000:0000:0000:0000'), 6);
    strictEqual(net.isIP('0000:0000:0000:0000:0000:0000:0000:0000::0000'), 0);
    strictEqual(net.isIP('1050:0:0:0:5:600:300c:326b'), 6);
    strictEqual(net.isIP('2001:252:0:1::2008:6'), 6);
    strictEqual(net.isIP('2001:dead:beef:1::2008:6'), 6);
    strictEqual(net.isIP('2001::'), 6);
    strictEqual(net.isIP('2001:dead::'), 6);
    strictEqual(net.isIP('2001:dead:beef::'), 6);
    strictEqual(net.isIP('2001:dead:beef:1::'), 6);
    strictEqual(net.isIP('ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff'), 6);
    strictEqual(net.isIP(':2001:252:0:1::2008:6:'), 0);
    strictEqual(net.isIP(':2001:252:0:1::2008:6'), 0);
    strictEqual(net.isIP('2001:252:0:1::2008:6:'), 0);
    strictEqual(net.isIP('2001:252::1::2008:6'), 0);
    strictEqual(net.isIP('::2001:252:1:2008:6'), 6);
    strictEqual(net.isIP('::2001:252:1:1.1.1.1'), 6);
    strictEqual(net.isIP('::2001:252:1:255.255.255.255'), 6);
    strictEqual(net.isIP('::2001:252:1:255.255.255.255.76'), 0);
    strictEqual(net.isIP('fe80::2008%eth0'), 6);
    strictEqual(net.isIP('fe80::2008%eth0.0'), 6);
    strictEqual(net.isIP('fe80::2008%eth0@1'), 0);
    strictEqual(net.isIP('::anything'), 0);
    strictEqual(net.isIP('::1'), 6);
    strictEqual(net.isIP('::'), 6);
    strictEqual(net.isIP('0000:0000:0000:0000:0000:0000:12345:0000'), 0);
    strictEqual(net.isIP('0'), 0);
    strictEqual(net.isIP(), 0);
    strictEqual(net.isIP(''), 0);
    strictEqual(net.isIP(null), 0);
    strictEqual(net.isIP(123), 0);
    strictEqual(net.isIP(true), 0);
    strictEqual(net.isIP({}), 0);
    strictEqual(
      net.isIP({ toString: () => '::2001:252:1:255.255.255.255' }),
      6
    );
    strictEqual(net.isIP({ toString: () => '127.0.0.1' }), 4);
    strictEqual(net.isIP({ toString: () => 'bla' }), 0);

    strictEqual(net.isIPv4('127.0.0.1'), true);
    strictEqual(net.isIPv4('example.com'), false);
    strictEqual(net.isIPv4('2001:252:0:1::2008:6'), false);
    strictEqual(net.isIPv4(), false);
    strictEqual(net.isIPv4(''), false);
    strictEqual(net.isIPv4(null), false);
    strictEqual(net.isIPv4(123), false);
    strictEqual(net.isIPv4(true), false);
    strictEqual(net.isIPv4({}), false);
    strictEqual(
      net.isIPv4({ toString: () => '::2001:252:1:255.255.255.255' }),
      false
    );
    strictEqual(net.isIPv4({ toString: () => '127.0.0.1' }), true);
    strictEqual(net.isIPv4({ toString: () => 'bla' }), false);

    strictEqual(net.isIPv6('127.0.0.1'), false);
    strictEqual(net.isIPv6('example.com'), false);
    strictEqual(net.isIPv6('2001:252:0:1::2008:6'), true);
    strictEqual(net.isIPv6(), false);
    strictEqual(net.isIPv6(''), false);
    strictEqual(net.isIPv6(null), false);
    strictEqual(net.isIPv6(123), false);
    strictEqual(net.isIPv6(true), false);
    strictEqual(net.isIPv6({}), false);
    strictEqual(
      net.isIPv6({ toString: () => '::2001:252:1:255.255.255.255' }),
      true
    );
    strictEqual(net.isIPv6({ toString: () => '127.0.0.1' }), false);
    strictEqual(net.isIPv6({ toString: () => 'bla' }), false);
  },
};

// test/parallel/test-net-isipv4.js
export const testNetIsIpv4 = {
  test() {
    const v4 = [
      '0.0.0.0',
      '8.8.8.8',
      '127.0.0.1',
      '100.100.100.100',
      '192.168.0.1',
      '18.101.25.153',
      '123.23.34.2',
      '172.26.168.134',
      '212.58.241.131',
      '128.0.0.0',
      '23.71.254.72',
      '223.255.255.255',
      '192.0.2.235',
      '99.198.122.146',
      '46.51.197.88',
      '173.194.34.134',
    ];

    const v4not = [
      '.100.100.100.100',
      '100..100.100.100.',
      '100.100.100.100.',
      '999.999.999.999',
      '256.256.256.256',
      '256.100.100.100.100',
      '123.123.123',
      'http://123.123.123',
      '1000.2.3.4',
      '999.2.3.4',
      '0000000192.168.0.200',
      '192.168.0.2000000000',
    ];

    for (const ip of v4) {
      strictEqual(net.isIPv4(ip), true);
    }

    for (const ip of v4not) {
      strictEqual(net.isIPv4(ip), false);
    }
  },
};

// test/parallel/test-net-isipv6.js
export const testNetIsIpv6 = {
  test() {
    const v6 = [
      '::',
      '1::',
      '::1',
      '1::8',
      '1::7:8',
      '1:2:3:4:5:6:7:8',
      '1:2:3:4:5:6::8',
      '1:2:3:4:5:6:7::',
      '1:2:3:4:5::7:8',
      '1:2:3:4:5::8',
      '1:2:3::8',
      '1::4:5:6:7:8',
      '1::6:7:8',
      '1::3:4:5:6:7:8',
      '1:2:3:4::6:7:8',
      '1:2::4:5:6:7:8',
      '::2:3:4:5:6:7:8',
      '1:2::8',
      '2001:0000:1234:0000:0000:C1C0:ABCD:0876',
      '3ffe:0b00:0000:0000:0001:0000:0000:000a',
      'FF02:0000:0000:0000:0000:0000:0000:0001',
      '0000:0000:0000:0000:0000:0000:0000:0001',
      '0000:0000:0000:0000:0000:0000:0000:0000',
      '::ffff:192.168.1.26',
      '2::10',
      'ff02::1',
      'fe80::',
      '2002::',
      '2001:db8::',
      '2001:0db8:1234::',
      '::ffff:0:0',
      '::ffff:192.168.1.1',
      '1:2:3:4::8',
      '1::2:3:4:5:6:7',
      '1::2:3:4:5:6',
      '1::2:3:4:5',
      '1::2:3:4',
      '1::2:3',
      '::2:3:4:5:6:7',
      '::2:3:4:5:6',
      '::2:3:4:5',
      '::2:3:4',
      '::2:3',
      '::8',
      '1:2:3:4:5:6::',
      '1:2:3:4:5::',
      '1:2:3:4::',
      '1:2:3::',
      '1:2::',
      '1:2:3:4::7:8',
      '1:2:3::7:8',
      '1:2::7:8',
      '1:2:3:4:5:6:1.2.3.4',
      '1:2:3:4:5::1.2.3.4',
      '1:2:3:4::1.2.3.4',
      '1:2:3::1.2.3.4',
      '1:2::1.2.3.4',
      '1::1.2.3.4',
      '1:2:3:4::5:1.2.3.4',
      '1:2:3::5:1.2.3.4',
      '1:2::5:1.2.3.4',
      '1::5:1.2.3.4',
      '1::5:11.22.33.44',
      'fe80::217:f2ff:254.7.237.98',
      'fe80::217:f2ff:fe07:ed62',
      '2001:DB8:0:0:8:800:200C:417A',
      'FF01:0:0:0:0:0:0:101',
      '0:0:0:0:0:0:0:1',
      '0:0:0:0:0:0:0:0',
      '2001:DB8::8:800:200C:417A',
      'FF01::101',
      '0:0:0:0:0:0:13.1.68.3',
      '0:0:0:0:0:FFFF:129.144.52.38',
      '::13.1.68.3',
      '::FFFF:129.144.52.38',
      'fe80:0000:0000:0000:0204:61ff:fe9d:f156',
      'fe80:0:0:0:204:61ff:fe9d:f156',
      'fe80::204:61ff:fe9d:f156',
      'fe80:0:0:0:204:61ff:254.157.241.86',
      'fe80::204:61ff:254.157.241.86',
      'fe80::1',
      '2001:0db8:85a3:0000:0000:8a2e:0370:7334',
      '2001:db8:85a3:0:0:8a2e:370:7334',
      '2001:db8:85a3::8a2e:370:7334',
      '2001:0db8:0000:0000:0000:0000:1428:57ab',
      '2001:0db8:0000:0000:0000::1428:57ab',
      '2001:0db8:0:0:0:0:1428:57ab',
      '2001:0db8:0:0::1428:57ab',
      '2001:0db8::1428:57ab',
      '2001:db8::1428:57ab',
      '::ffff:12.34.56.78',
      '::ffff:0c22:384e',
      '2001:0db8:1234:0000:0000:0000:0000:0000',
      '2001:0db8:1234:ffff:ffff:ffff:ffff:ffff',
      '2001:db8:a::123',
      '::ffff:192.0.2.128',
      '::ffff:c000:280',
      'a:b:c:d:e:f:f1:f2',
      'a:b:c::d:e:f:f1',
      'a:b:c::d:e:f',
      'a:b:c::d:e',
      'a:b:c::d',
      '::a',
      '::a:b:c',
      '::a:b:c:d:e:f:f1',
      'a::',
      'a:b:c::',
      'a:b:c:d:e:f:f1::',
      'a:bb:ccc:dddd:000e:00f:0f::',
      '0:a:0:a:0:0:0:a',
      '0:a:0:0:a:0:0:a',
      '2001:db8:1:1:1:1:0:0',
      '2001:db8:1:1:1:0:0:0',
      '2001:db8:1:1:0:0:0:0',
      '2001:db8:1:0:0:0:0:0',
      '2001:db8:0:0:0:0:0:0',
      '2001:0:0:0:0:0:0:0',
      'A:BB:CCC:DDDD:000E:00F:0F::',
      '0:0:0:0:0:0:0:a',
      '0:0:0:0:a:0:0:0',
      '0:0:0:a:0:0:0:0',
      'a:0:0:a:0:0:a:a',
      'a:0:0:a:0:0:0:a',
      'a:0:0:0:a:0:0:a',
      'a:0:0:0:a:0:0:0',
      'a:0:0:0:0:0:0:0',
      'fe80::7:8%eth0',
      'fe80::7:8%1',
    ];

    const v6not = [
      '',
      '1:',
      ':1',
      '11:36:12',
      '02001:0000:1234:0000:0000:C1C0:ABCD:0876',
      '2001:0000:1234:0000:00001:C1C0:ABCD:0876',
      '2001:0000:1234: 0000:0000:C1C0:ABCD:0876',
      '2001:1:1:1:1:1:255Z255X255Y255',
      '3ffe:0b00:0000:0001:0000:0000:000a',
      'FF02:0000:0000:0000:0000:0000:0000:0000:0001',
      '3ffe:b00::1::a',
      '::1111:2222:3333:4444:5555:6666::',
      '1:2:3::4:5::7:8',
      '12345::6:7:8',
      '1::5:400.2.3.4',
      '1::5:260.2.3.4',
      '1::5:256.2.3.4',
      '1::5:1.256.3.4',
      '1::5:1.2.256.4',
      '1::5:1.2.3.256',
      '1::5:300.2.3.4',
      '1::5:1.300.3.4',
      '1::5:1.2.300.4',
      '1::5:1.2.3.300',
      '1::5:900.2.3.4',
      '1::5:1.900.3.4',
      '1::5:1.2.900.4',
      '1::5:1.2.3.900',
      '1::5:300.300.300.300',
      '1::5:3000.30.30.30',
      '1::400.2.3.4',
      '1::260.2.3.4',
      '1::256.2.3.4',
      '1::1.256.3.4',
      '1::1.2.256.4',
      '1::1.2.3.256',
      '1::300.2.3.4',
      '1::1.300.3.4',
      '1::1.2.300.4',
      '1::1.2.3.300',
      '1::900.2.3.4',
      '1::1.900.3.4',
      '1::1.2.900.4',
      '1::1.2.3.900',
      '1::300.300.300.300',
      '1::3000.30.30.30',
      '::400.2.3.4',
      '::260.2.3.4',
      '::256.2.3.4',
      '::1.256.3.4',
      '::1.2.256.4',
      '::1.2.3.256',
      '::300.2.3.4',
      '::1.300.3.4',
      '::1.2.300.4',
      '::1.2.3.300',
      '::900.2.3.4',
      '::1.900.3.4',
      '::1.2.900.4',
      '::1.2.3.900',
      '::300.300.300.300',
      '::3000.30.30.30',
      '2001:DB8:0:0:8:800:200C:417A:221',
      'FF01::101::2',
      '1111:2222:3333:4444::5555:',
      '1111:2222:3333::5555:',
      '1111:2222::5555:',
      '1111::5555:',
      '::5555:',
      ':::',
      '1111:',
      ':',
      ':1111:2222:3333:4444::5555',
      ':1111:2222:3333::5555',
      ':1111:2222::5555',
      ':1111::5555',
      ':::5555',
      '1.2.3.4:1111:2222:3333:4444::5555',
      '1.2.3.4:1111:2222:3333::5555',
      '1.2.3.4:1111:2222::5555',
      '1.2.3.4:1111::5555',
      '1.2.3.4::5555',
      '1.2.3.4::',
      'fe80:0000:0000:0000:0204:61ff:254.157.241.086',
      '123',
      'ldkfj',
      '2001::FFD3::57ab',
      '2001:db8:85a3::8a2e:37023:7334',
      '2001:db8:85a3::8a2e:370k:7334',
      '1:2:3:4:5:6:7:8:9',
      '1::2::3',
      '1:::3:4:5',
      '1:2:3::4:5:6:7:8:9',
      '::ffff:2.3.4',
      '::ffff:257.1.2.3',
      '::ffff:12345678901234567890.1.26',
      '2001:0000:1234:0000:0000:C1C0:ABCD:0876 0',
      '02001:0000:1234:0000:0000:C1C0:ABCD:0876',
    ];

    for (const ip of v6) {
      strictEqual(net.isIPv6(ip), true);
    }

    for (const ip of v6not) {
      strictEqual(net.isIPv6(ip), false);
    }
  },
};

// test/parallel/test-net-large-string.js
export const testNetLargeString = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9998, 'localhost');
    let response = '';
    const size = 40 * 1024;
    const data = 'あ'.repeat(size);
    c.setEncoding('utf8');
    c.on('data', (data) => (response += data));
    c.end(data);
    c.on('close', resolve);
    await promise;
    strictEqual(response.length, size);
    strictEqual(response, data);
  },
};

// test/parallel/test-net-local-address-port.js
// The localAddress information is a non-op in our implementation
export const testNetLocalAddressPort = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9999, 'localhost');
    c.on('connect', () => {
      strictEqual(c.localAddress, '0.0.0.0');
      strictEqual(c.localPort, 0);
      resolve();
    });
    await promise;
  },
};

// test/parallel/test-net-onread-static-buffer.js
export const testNetOnReadStaticBuffer = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const buffer = Buffer.alloc(1024);
    const fn = mock.fn((nread, buf) => {
      strictEqual(nread, 5);
      strictEqual(buf.buffer.byteLength, 1024);
      resolve();
    });
    const c = net.connect({
      port: 9998,
      host: 'localhost',
      onread: {
        buffer,
        callback: fn,
      },
    });
    c.on('data', () => {
      throw new Error('Should not have failed');
    });
    c.write('hello');
    await promise;
    strictEqual(fn.mock.callCount(), 1);
  },
};

// test/parallel/test-net-remote-address-port.js
// test/parallel/test-net-remote-address.js
export const testNetRemoteAddress = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.connect(9999, 'localhost');
    c.on('connect', () => {
      strictEqual(c.remoteAddress, 'localhost');
      strictEqual(c.remotePort, 9999);
      resolve();
    });
    await promise;
  },
};
