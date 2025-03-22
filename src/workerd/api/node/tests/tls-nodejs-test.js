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
