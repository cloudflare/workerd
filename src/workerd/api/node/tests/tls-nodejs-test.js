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
import { connect } from 'cloudflare:sockets';
import tls from 'node:tls';
import {
  strictEqual,
  ok,
  rejects,
  throws,
  doesNotThrow,
  deepStrictEqual,
} from 'node:assert';
import { once } from 'node:events';
import { inspect } from 'node:util';
import net from 'node:net';
import { translatePeerCertificate } from '_tls_common';
import { mock } from 'node:test';
import stream from 'node:stream';

export const checkPortsSetCorrectly = {
  test(ctrl, env, ctx) {
    ok(env.ECHO_SERVER_PORT);
    ok(env.HELLO_SERVER_PORT);
    ok(env.JS_STREAM_SERVER_PORT);
    ok(env.STREAM_WRAP_SERVER_PORT);
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/304743655d5236c2edc39094336ee2667600b684/test/parallel/test-tls-connect-abort-controller.js
export const tlsConnectAbortController = {
  async test(ctrl, env, ctx) {
    // Our tests differ from Node.js
    // We don't check for abortSignal listener count because it's not supported.
    const connectOptions = (signal) => ({
      port: env.ECHO_SERVER_PORT,
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
  async test(ctrl, env, ctx) {
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
          port: env.ECHO_SERVER_PORT,
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
  async test(ctrl, env, ctx) {
    const promise1 = Promise.withResolvers();
    const promise2 = Promise.withResolvers();
    const options = { port: env.ECHO_SERVER_PORT };
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
  async test(ctrl, env, ctx) {
    const socket = tls.connect({
      port: env.ECHO_SERVER_PORT,
      lookup: () => {},
      timeout: 1000,
    });

    strictEqual(socket.timeout, 1000);
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/755e4603fd1679de72d250514ea5096b272ae8d6/test/parallel/test-tls-connect-no-host.js
export const tlsConnectNoHost = {
  async test(ctrl, env, ctx) {
    const { promise, resolve } = Promise.withResolvers();
    const socket = tls.connect(
      {
        port: env.ECHO_SERVER_PORT,
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
  async test(ctrl, env, ctx) {
    const promises = [];
    let waiting = 2;
    function establish(socket, calls) {
      const { promise, resolve, reject } = Promise.withResolvers();
      promises.push(promise);
      const onConnectFn = mock.fn(() => {
        if (calls === 0) {
          reject(new Error('Should not have called onConnect callback'));
          return;
        }
        let data = '';
        let dataFn = mock.fn((chunk) => {
          data += chunk.toString();
        });
        client.on('data', dataFn);
        client.on('end', () => {
          strictEqual(data, 'Hello');
          if (--waiting === 0) {
            ok(dataFn.mock.callCount());
            resolve();
          }
        });
      });
      const client = tls.connect({ socket }, onConnectFn);
      ok(client.readable);
      ok(client.writable);

      if (calls === 0) {
        queueMicrotask(resolve);
      }

      return client;
    }

    const port = env.HELLO_SERVER_PORT;
    // Immediate death socket
    const immediateDeath = net.connect(port);
    establish(immediateDeath, 0).destroy();

    // Outliving
    {
      const { promise, resolve } = Promise.withResolvers();
      promises.push(promise);
      const outlivingTCP = net.connect(port, () => {
        outlivingTLS.destroy();
        next();
        resolve();
      });
      const outlivingTLS = establish(outlivingTCP, 0);
    }

    function next() {
      // Already connected socket
      const { promise, resolve } = Promise.withResolvers();
      promises.push(promise);
      const connected = net.connect(port, () => {
        establish(connected);
        resolve();
      });

      // Connecting socket
      const connecting = net.connect(port);
      establish(connecting);
    }

    await Promise.all(promises);
  },
};

export const testSecureContext = {
  async test() {
    throws(() => tls.connect({ port: 42, secureContext: {} }), {
      code: 'ERR_TLS_INVALID_CONTEXT',
    });

    doesNotThrow(() => {
      const secureContext = tls.createSecureContext({});
      tls.connect({ port: 42, secureContext });
    });

    doesNotThrow(() => {
      const secureContext = tls.SecureContext();
      tls.connect({ port: 42, secureContext });
    });
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/98513884684bccf944d7834f4820b061af41fb36/test/parallel/test-tls-check-server-identity.js
export const testCheckServerIdentity = {
  async test() {
    const tests = [
      // False-y values.
      {
        host: false,
        cert: { subject: { CN: 'a.com' } },
        error: "Host: false. is not cert's CN: a.com",
      },
      {
        host: null,
        cert: { subject: { CN: 'a.com' } },
        error: "Host: null. is not cert's CN: a.com",
      },
      {
        host: undefined,
        cert: { subject: { CN: 'a.com' } },
        error: "Host: undefined. is not cert's CN: a.com",
      },

      // Basic CN handling
      { host: 'a.com', cert: { subject: { CN: 'a.com' } } },
      { host: 'a.com', cert: { subject: { CN: 'A.COM' } } },
      {
        host: 'a.com',
        cert: { subject: { CN: 'b.com' } },
        error: "Host: a.com. is not cert's CN: b.com",
      },
      { host: 'a.com', cert: { subject: { CN: 'a.com.' } } },
      {
        host: 'a.com',
        cert: { subject: { CN: '.a.com' } },
        error: "Host: a.com. is not cert's CN: .a.com",
      },

      // IP address in CN. Technically allowed but so rare that we reject
      // it anyway. If we ever do start allowing them, we should take care
      // to only allow public (non-internal, non-reserved) IP addresses,
      // because that's what the spec mandates.
      {
        host: '8.8.8.8',
        cert: { subject: { CN: '8.8.8.8' } },
        error: "IP: 8.8.8.8 is not in the cert's list: ",
      },

      // The spec suggests that a "DNS:" Subject Alternative Name containing an
      // IP address is valid but it seems so suspect that we currently reject it.
      {
        host: '8.8.8.8',
        cert: { subject: { CN: '8.8.8.8' }, subjectaltname: 'DNS:8.8.8.8' },
        error: "IP: 8.8.8.8 is not in the cert's list: ",
      },

      // Likewise for "URI:" Subject Alternative Names.
      // See also https://github.com/nodejs/node/issues/8108.
      {
        host: '8.8.8.8',
        cert: {
          subject: { CN: '8.8.8.8' },
          subjectaltname: 'URI:http://8.8.8.8/',
        },
        error: "IP: 8.8.8.8 is not in the cert's list: ",
      },

      // An "IP Address:" Subject Alternative Name however is acceptable.
      {
        host: '8.8.8.8',
        cert: {
          subject: { CN: '8.8.8.8' },
          subjectaltname: 'IP Address:8.8.8.8',
        },
      },

      // But not when it's a CIDR.
      {
        host: '8.8.8.8',
        cert: {
          subject: { CN: '8.8.8.8' },
          subjectaltname: 'IP Address:8.8.8.0/24',
        },
        error: "IP: 8.8.8.8 is not in the cert's list: ",
      },

      // Wildcards in CN
      { host: 'b.a.com', cert: { subject: { CN: '*.a.com' } } },
      {
        host: 'ba.com',
        cert: { subject: { CN: '*.a.com' } },
        error: "Host: ba.com. is not cert's CN: *.a.com",
      },
      {
        host: '\n.b.com',
        cert: { subject: { CN: '*n.b.com' } },
        error: "Host: \n.b.com. is not cert's CN: *n.b.com",
      },
      {
        host: 'b.a.com',
        cert: {
          subjectaltname: 'DNS:omg.com',
          subject: { CN: '*.a.com' },
        },
        error: "Host: b.a.com. is not in the cert's altnames: " + 'DNS:omg.com',
      },
      {
        host: 'b.a.com',
        cert: { subject: { CN: 'b*b.a.com' } },
        error: "Host: b.a.com. is not cert's CN: b*b.a.com",
      },

      // Empty Cert
      {
        host: 'a.com',
        cert: {},
        error: 'Cert does not contain a DNS name',
      },

      // Empty Subject w/DNS name
      {
        host: 'a.com',
        cert: {
          subjectaltname: 'DNS:a.com',
        },
      },

      // Empty Subject w/URI name
      {
        host: 'a.b.a.com',
        cert: {
          subjectaltname: 'URI:http://a.b.a.com/',
        },
        error: 'Cert does not contain a DNS name',
      },

      // Multiple CN fields
      {
        host: 'foo.com',
        cert: {
          subject: { CN: ['foo.com', 'bar.com'] }, // CN=foo.com; CN=bar.com;
        },
      },

      // DNS names and CN
      {
        host: 'a.com',
        cert: {
          subjectaltname: 'DNS:*',
          subject: { CN: 'b.com' },
        },
        error: "Host: a.com. is not in the cert's altnames: " + 'DNS:*',
      },
      {
        host: 'a.com',
        cert: {
          subjectaltname: 'DNS:*.com',
          subject: { CN: 'b.com' },
        },
        error: "Host: a.com. is not in the cert's altnames: " + 'DNS:*.com',
      },
      {
        host: 'a.co.uk',
        cert: {
          subjectaltname: 'DNS:*.co.uk',
          subject: { CN: 'b.com' },
        },
      },
      {
        host: 'a.com',
        cert: {
          subjectaltname: 'DNS:*.a.com',
          subject: { CN: 'a.com' },
        },
        error: "Host: a.com. is not in the cert's altnames: " + 'DNS:*.a.com',
      },
      {
        host: 'a.com',
        cert: {
          subjectaltname: 'DNS:*.a.com',
          subject: { CN: 'b.com' },
        },
        error: "Host: a.com. is not in the cert's altnames: " + 'DNS:*.a.com',
      },
      {
        host: 'a.com',
        cert: {
          subjectaltname: 'DNS:a.com',
          subject: { CN: 'b.com' },
        },
      },
      {
        host: 'a.com',
        cert: {
          subjectaltname: 'DNS:A.COM',
          subject: { CN: 'b.com' },
        },
      },

      // DNS names
      {
        host: 'a.com',
        cert: {
          subjectaltname: 'DNS:*.a.com',
          subject: {},
        },
        error: "Host: a.com. is not in the cert's altnames: " + 'DNS:*.a.com',
      },
      {
        host: 'b.a.com',
        cert: {
          subjectaltname: 'DNS:*.a.com',
          subject: {},
        },
      },
      {
        host: 'c.b.a.com',
        cert: {
          subjectaltname: 'DNS:*.a.com',
          subject: {},
        },
        error:
          "Host: c.b.a.com. is not in the cert's altnames: " + 'DNS:*.a.com',
      },
      {
        host: 'b.a.com',
        cert: {
          subjectaltname: 'DNS:*b.a.com',
          subject: {},
        },
      },
      {
        host: 'a-cb.a.com',
        cert: {
          subjectaltname: 'DNS:*b.a.com',
          subject: {},
        },
      },
      {
        host: 'a.b.a.com',
        cert: {
          subjectaltname: 'DNS:*b.a.com',
          subject: {},
        },
        error:
          "Host: a.b.a.com. is not in the cert's altnames: " + 'DNS:*b.a.com',
      },
      // Multiple DNS names
      {
        host: 'a.b.a.com',
        cert: {
          subjectaltname: 'DNS:*b.a.com, DNS:a.b.a.com',
          subject: {},
        },
      },
      // URI names
      {
        host: 'a.b.a.com',
        cert: {
          subjectaltname: 'URI:http://a.b.a.com/',
          subject: {},
        },
        error: 'Cert does not contain a DNS name',
      },
      {
        host: 'a.b.a.com',
        cert: {
          subjectaltname: 'URI:http://*.b.a.com/',
          subject: {},
        },
        error: 'Cert does not contain a DNS name',
      },
      // IP addresses
      {
        host: 'a.b.a.com',
        cert: {
          subjectaltname: 'IP Address:127.0.0.1',
          subject: {},
        },
        error: 'Cert does not contain a DNS name',
      },
      {
        host: '127.0.0.1',
        cert: {
          subjectaltname: 'IP Address:127.0.0.1',
          subject: {},
        },
      },
      {
        host: '127.0.0.2',
        cert: {
          subjectaltname: 'IP Address:127.0.0.1',
          subject: {},
        },
        error: "IP: 127.0.0.2 is not in the cert's list: " + '127.0.0.1',
      },
      {
        host: '127.0.0.1',
        cert: {
          subjectaltname: 'DNS:a.com',
          subject: {},
        },
        error: "IP: 127.0.0.1 is not in the cert's list: ",
      },
      {
        host: 'localhost',
        cert: {
          subjectaltname: 'DNS:a.com',
          subject: { CN: 'localhost' },
        },
        error: "Host: localhost. is not in the cert's altnames: " + 'DNS:a.com',
      },
      // IDNA
      {
        host: 'xn--bcher-kva.example.com',
        cert: { subject: { CN: '*.example.com' } },
      },
      // RFC 6125, section 6.4.3: "[...] the client SHOULD NOT attempt to match
      // a presented identifier where the wildcard character is embedded within
      // an A-label [...]"
      {
        host: 'xn--bcher-kva.example.com',
        cert: { subject: { CN: 'xn--*.example.com' } },
        error:
          "Host: xn--bcher-kva.example.com. is not cert's CN: " +
          'xn--*.example.com',
      },
    ];

    tests.forEach(function (test, i) {
      const err = tls.checkServerIdentity(test.host, test.cert);
      strictEqual(
        err?.reason,
        test.error,
        `Test# ${i} failed: ${inspect(test)} \n` +
          `${test.error} != ${err?.reason}`
      );
    });
  },
};

// Tests are taken from
// https://github.com/nodejs/node/blob/1b5b019de1be9259e4374ca1d6ee7b3b28c48856/test/parallel/test-tls-translate-peer-certificate.js
export const testTlsTranslatePeerCertificate = {
  async test() {
    const certString = '__proto__=42\nA=1\nB=2\nC=3';

    strictEqual(translatePeerCertificate(null), null);
    strictEqual(translatePeerCertificate(undefined), null);

    strictEqual(translatePeerCertificate(0), null);
    strictEqual(translatePeerCertificate(1), 1);

    deepStrictEqual(translatePeerCertificate({}), {});

    // Earlier versions of Node.js parsed the issuer property but did so
    // incorrectly. This behavior has now reached end-of-life and user-supplied
    // strings will not be parsed at all.
    deepStrictEqual(translatePeerCertificate({ issuer: '' }), { issuer: '' });
    deepStrictEqual(translatePeerCertificate({ issuer: null }), {
      issuer: null,
    });
    deepStrictEqual(translatePeerCertificate({ issuer: certString }), {
      issuer: certString,
    });

    // Earlier versions of Node.js parsed the issuer property but did so
    // incorrectly. This behavior has now reached end-of-life and user-supplied
    // strings will not be parsed at all.
    deepStrictEqual(translatePeerCertificate({ subject: '' }), { subject: '' });
    deepStrictEqual(translatePeerCertificate({ subject: null }), {
      subject: null,
    });
    deepStrictEqual(translatePeerCertificate({ subject: certString }), {
      subject: certString,
    });

    deepStrictEqual(translatePeerCertificate({ issuerCertificate: '' }), {
      issuerCertificate: null,
    });
    deepStrictEqual(translatePeerCertificate({ issuerCertificate: null }), {
      issuerCertificate: null,
    });
    deepStrictEqual(
      translatePeerCertificate({ issuerCertificate: { subject: certString } }),
      { issuerCertificate: { subject: certString } }
    );

    {
      const cert = {};
      cert.issuerCertificate = cert;
      deepStrictEqual(translatePeerCertificate(cert), {
        issuerCertificate: cert,
      });
    }

    deepStrictEqual(translatePeerCertificate({ infoAccess: '' }), {
      infoAccess: { __proto__: null },
    });
    deepStrictEqual(translatePeerCertificate({ infoAccess: null }), {
      infoAccess: null,
    });
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/b1402835a512f14fa9f8dd23d3e0cee8cfe888a2/test/parallel/test-tls-basic-validations.js
export const testConvertALPNProtocols = {
  async test() {
    {
      const buffer = Buffer.from('abcd');
      const out = {};
      tls.convertALPNProtocols(buffer, out);
      out.ALPNProtocols.write('efgh');
      ok(buffer.equals(Buffer.from('abcd')));
      ok(out.ALPNProtocols.equals(Buffer.from('efgh')));
    }

    {
      const protocols = [new String('a').repeat(500)];
      const out = {};
      throws(() => tls.convertALPNProtocols(protocols, out), {
        code: 'ERR_OUT_OF_RANGE',
        message:
          'The byte length of the protocol at index 0 exceeds the ' +
          'maximum length. It must be <= 255. Received 500',
      });
    }
  },
};

export const testStartTlsBehaviorOnUpgrade = {
  async test(ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const socket = connect(`localhost:${env.HELLO_SERVER_PORT}`, {
      secureTransport: 'starttls',
    });
    strictEqual(socket.secureTransport, 'starttls');
    strictEqual(socket.upgraded, false);
    await socket.opened;
    strictEqual(socket.upgraded, false);
    socket.closed
      .then(() => {
        strictEqual(socket.secureTransport, 'starttls');
        strictEqual(socket.upgraded, true);
        resolve();
      })
      .catch(reject);
    const secureSocket = socket.startTls();
    // The newly created socket instance is not upgraded.
    strictEqual(secureSocket.upgraded, false);
    strictEqual(secureSocket.secureTransport, 'on');
    await promise;
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/52d95f53e466016120048fb43b3732ff9089ecd7/test/parallel/test-tls-destroy-whilst-write.js
export const testTlsDestroyWhilstWrite = {
  async test() {
    const { promise, resolve } = Promise.withResolvers();
    const delay = new stream.Duplex({
      read: function read() {},
      write: function write(data, enc, cb) {
        queueMicrotask(cb);
      },
    });

    const secure = tls.connect({
      socket: delay,
    });
    queueMicrotask(function () {
      secure.destroy();
    });
    secure.on('close', resolve);
    await promise;
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/52d95f53e466016120048fb43b3732ff9089ecd7/test/parallel/test-tls-js-stream.js
export const testTlsJsStream = {
  async test(ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const raw = net.connect(env.JS_STREAM_SERVER_PORT);

    let pending = false;
    raw.on('readable', function () {
      if (pending) socket._read();
    });

    raw.on('end', function () {
      socket.push(null);
    });

    const socket = new stream.Duplex({
      read: function read() {
        pending = false;

        const chunk = raw.read();
        if (chunk) {
          this.push(chunk);
        } else {
          pending = true;
        }
      },
      write: function write(data, enc, cb) {
        raw.write(data, enc, cb);
      },
    });

    const onConnectFn = mock.fn(() => {
      socket.resume();
      socket.end('hello');
    });
    const conn = tls.connect({ socket }, onConnectFn);
    conn.once('error', reject);
    conn.once('close', resolve);

    await promise;
    strictEqual(onConnectFn.mock.callCount(), 1);
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/b1402835a512f14fa9f8dd23d3e0cee8cfe888a2/test/parallel/test-tls-junk-closes-server.js
export const testTlsJunkClosesServer = {
  async test(ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const c = net.createConnection(env.HELLO_SERVER_PORT);

    c.on('data', function () {
      // We must consume all data sent by the server. Otherwise the
      // end event will not be sent and the test will hang.
      // For example, when compiled with OpenSSL32 we see the
      // following response '15 03 03 00 02 02 16' which
      // decodes as a fatal (0x02) TLS error alert number 22 (0x16),
      // which corresponds to TLS1_AD_RECORD_OVERFLOW which matches
      // the error we see if NODE_DEBUG is turned on.
      // Some earlier OpenSSL versions did not seem to send a response
      // but the TLS spec seems to indicate there should be one
      // https://datatracker.ietf.org/doc/html/rfc8446#page-85
      // and error handling seems to have been re-written/improved
      // in OpenSSL32. Consuming the data allows the test to pass
      // either way.
    });

    const onConnectFn = mock.fn(() => {
      c.write('blah\nblah\nblah\n');
    });
    c.on('connect', onConnectFn);
    c.on('end', resolve);
    await promise;
    strictEqual(onConnectFn.mock.callCount(), 1);
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/91d8a524ada001103a2d1c6825ca17b8393c183f/test/parallel/test-tls-on-empty-socket.js
export const testTlsOnEmptySocket = {
  async test(ctrl, env) {
    const { promise, resolve, reject } = Promise.withResolvers();
    const socket = new net.Socket();
    let out = '';

    const s = tls.connect({ socket }, function () {
      s.on('error', reject);
      s.on('data', function (chunk) {
        out += chunk;
      });
      s.on('end', resolve);
    });

    const onConnectFn = mock.fn();
    socket.connect(env.HELLO_SERVER_PORT, onConnectFn);

    await promise;
    strictEqual(out, 'Hello');
    strictEqual(onConnectFn.mock.callCount(), 1);
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/91d8a524ada001103a2d1c6825ca17b8393c183f/test/parallel/test-tls-pause.js
export const testTlsPause = {
  async test(ctrl, env) {
    const { promise, resolve } = Promise.withResolvers();
    const bufSize = 1024 * 1024;
    let sent = 0;
    let received = 0;
    let resumed = false;
    const client = tls.connect(
      {
        port: env.ECHO_SERVER_PORT,
      },
      () => {
        client.pause();
        const send = (() => {
          const ret = client.write(Buffer.allocUnsafe(bufSize));
          if (ret !== false) {
            sent += bufSize;
            ok(sent < 100 * 1024 * 1024); // max 100MB
            return process.nextTick(send);
          }
          sent += bufSize;
          resumed = true;
          client.resume();
        })();
      }
    );
    client.on('data', (data) => {
      ok(resumed);
      received += data.length;
      if (received >= sent) {
        client.end();
        resolve();
      }
    });

    await promise;
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/cb5f671a34da32e3c2d70d7f3e7f869cda6b806b/test/parallel/test-tls-socket-allow-half-open-option.js
export const testTlsSocketAllowHalfOpenOption = {
  async test() {
    {
      // The option is ignored when the `socket` argument is a `net.Socket`.
      const socket = new tls.TLSSocket(new net.Socket(), {
        allowHalfOpen: true,
      });
      strictEqual(socket.allowHalfOpen, false);
    }

    {
      // The option is ignored when the `socket` argument is a generic
      // `stream.Duplex`.
      const duplex = new stream.Duplex({
        allowHalfOpen: false,
        read() {},
      });
      const socket = new tls.TLSSocket(duplex, { allowHalfOpen: true });
      strictEqual(socket.allowHalfOpen, false);
    }

    {
      const socket = new tls.TLSSocket();
      strictEqual(socket.allowHalfOpen, false);
    }

    {
      // The option is honored when the `socket` argument is not specified.
      const socket = new tls.TLSSocket(undefined, { allowHalfOpen: true });
      strictEqual(socket.allowHalfOpen, true);
    }
  },
};

// Tests are taken from:
// https://github.com/nodejs/node/blob/52d95f53e466016120048fb43b3732ff9089ecd7/test/parallel/test-tls-streamwrap-buffersize.js
export const testTlsStreamwrapBuffersize = {
  async test(ctrl, env) {
    // This test ensures that `bufferSize` also works for those tlsSockets
    // created from `socket` of `Duplex`, with which, TLSSocket will wrap
    // sockets in `StreamWrap`.
    const iter = 10;

    function createDuplex() {
      const [clientSide, serverSide] = stream.duplexPair();
      const dp = Promise.withResolvers();

      const socket = net.connect(env.STREAM_WRAP_SERVER_PORT, () => {
        clientSide.pipe(socket);
        socket.pipe(clientSide);
        clientSide.on('close', () => socket.destroy());
        socket.on('close', () => clientSide.destroy());

        dp.resolve(serverSide);
      });

      return dp.promise;
    }

    const socket = await createDuplex();
    const { promise, resolve } = Promise.withResolvers();
    const onCloseFn = mock.fn(() => {
      // TODO(soon): This should be undefined, not 0.
      strictEqual(client.bufferSize, 0);
      resolve();
    });
    const client = tls.connect({ socket }, () => {
      strictEqual(client.bufferSize, 0);

      for (let i = 1; i < iter; i++) {
        client.write('a');
        strictEqual(client.bufferSize, i);
      }

      client.end();
    });

    client.on('close', onCloseFn);

    await promise;
    strictEqual(onCloseFn.mock.callCount(), 1);
  },
};

export const testEOLMethods = {
  async test() {
    strictEqual(typeof tls.createSecurePair, 'function');
  },
};
