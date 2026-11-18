// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Every service in node-proto-descriptor-pollution-test.wd-test runs this module in its own
// isolate with a different `key` binding. Each test adds that property descriptor field to
// Object.prototype and then calls node: code that defines properties from descriptor literals
// on every call.
import { deepStrictEqual, ok, strictEqual } from 'node:assert';
import http from 'node:http';
import { Writable } from 'node:stream';
import { Console } from 'node:console';
import util from 'node:util';
import { handleAsNodeRequest } from 'cloudflare:node';

// A value for the polluted field that would change the resulting property if it were inherited.
function pollutionFor(key) {
  switch (key) {
    case 'get':
    case 'set':
      return function polluted() {
        return 'polluted';
      };
    case 'value':
      return 'polluted';
    default:
      return true;
  }
}

async function withPollutedPrototype(env, fn) {
  const key = env.key;
  Object.defineProperty(Object.prototype, key, {
    __proto__: null,
    value: pollutionFor(key),
    writable: true,
    configurable: true,
  });
  try {
    await fn();
  } finally {
    Reflect.deleteProperty(Object.prototype, key);
  }
}

export const httpIncomingSocket = {
  async test(_ctrl, env) {
    let socketProps;
    const server = http.createServer((req, res) => {
      socketProps = {
        encrypted: req.socket.encrypted,
        remoteFamily: req.socket.remoteFamily,
        localPort: req.socket.localPort,
        remoteAddressEnumerable: Object.getOwnPropertyDescriptor(
          req.socket,
          'remoteAddress'
        ).enumerable,
      };
      res.end('ok');
    });
    const { promise, resolve } = Promise.withResolvers();
    server.listen(0, resolve);
    await promise;
    const port = server.address().port;
    try {
      await withPollutedPrototype(env, async () => {
        const res = await handleAsNodeRequest(
          { port },
          new Request('https://example.com/', {
            headers: { 'x-forwarded-proto': 'https' },
          })
        );
        strictEqual(await res.text(), 'ok');
      });
    } finally {
      server.close();
    }
    deepStrictEqual(socketProps, {
      encrypted: true,
      remoteFamily: 'IPv4',
      localPort: port,
      remoteAddressEnumerable: false,
    });
  },
};

export const inspectJsgResource = {
  async test(_ctrl, env) {
    await withPollutedPrototype(env, () => {
      ok(util.inspect(new Headers()).startsWith('Headers'));
    });
  },
};

export const consoleConstructor = {
  async test(_ctrl, env) {
    const chunks = [];
    const stream = new Writable({
      write(chunk, _encoding, callback) {
        chunks.push(String(chunk));
        callback();
      },
    });
    let c;
    await withPollutedPrototype(env, () => {
      c = new Console(stream);
    });
    c.log('hello');
    deepStrictEqual(chunks, ['hello\n']);
    strictEqual(c.log.name, 'log');
    deepStrictEqual(Object.getOwnPropertyDescriptor(c, '_stdout'), {
      value: stream,
      writable: true,
      enumerable: false,
      configurable: true,
    });
  },
};

export const debuglogEnabled = {
  async test(_ctrl, env) {
    let logger;
    await withPollutedPrototype(env, () => {
      logger = util.debuglog('proto-test');
    });
    strictEqual(logger.enabled, true);
    strictEqual(
      Object.getOwnPropertyDescriptor(logger, 'enabled').writable,
      undefined
    );
  },
};

export const processStdioAssignment = {
  async test(_ctrl, env) {
    const original = process.stdout;
    const replacement = { fd: 1 };
    await withPollutedPrototype(env, () => {
      process.stdout = replacement;
    });
    try {
      deepStrictEqual(Object.getOwnPropertyDescriptor(process, 'stdout'), {
        value: replacement,
        writable: true,
        enumerable: true,
        configurable: true,
      });
    } finally {
      process.stdout = original;
    }
  },
};

export const utilInherits = {
  async test(_ctrl, env) {
    function Base() {}
    function Derived() {}
    await withPollutedPrototype(env, () => {
      util.inherits(Derived, Base);
    });
    deepStrictEqual(Object.getOwnPropertyDescriptor(Derived, 'super_'), {
      value: Base,
      writable: true,
      enumerable: false,
      configurable: true,
    });
  },
};

export const utilPromisifyCustom = {
  async test(_ctrl, env) {
    const custom = async () => 'custom';
    function original() {}
    original[util.promisify.custom] = custom;
    let promisified;
    await withPollutedPrototype(env, () => {
      promisified = util.promisify(original);
    });
    strictEqual(promisified, custom);
    strictEqual(await promisified(), 'custom');
    deepStrictEqual(
      Object.getOwnPropertyDescriptor(custom, util.promisify.custom),
      {
        value: custom,
        writable: false,
        enumerable: false,
        configurable: true,
      }
    );
  },
};

export const utilPromisify = {
  async test(_ctrl, env) {
    function original(a, cb) {
      cb(null, a + 1);
    }
    let promisified;
    await withPollutedPrototype(env, () => {
      promisified = util.promisify(original);
    });
    strictEqual(await promisified(1), 2);
    strictEqual(promisified.name, 'original');
    deepStrictEqual(
      Object.getOwnPropertyDescriptor(promisified, util.promisify.custom),
      {
        value: promisified,
        writable: false,
        enumerable: false,
        configurable: true,
      }
    );
  },
};

export const utilCallbackify = {
  async test(_ctrl, env) {
    async function original(a) {
      return a + 1;
    }
    let callbackified;
    await withPollutedPrototype(env, () => {
      callbackified = util.callbackify(original);
    });
    strictEqual(callbackified.name, 'originalCallbackified');
    const { promise, resolve, reject } = Promise.withResolvers();
    callbackified(1, (err, value) => (err ? reject(err) : resolve(value)));
    strictEqual(await promise, 2);
  },
};
