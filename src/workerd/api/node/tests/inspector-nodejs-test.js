import inspector from 'node:inspector';
import assert from 'node:assert';
import { EventEmitter } from 'node:events';

export const inspectorClose = {
  test() {
    assert.doesNotThrow(() => inspector.close());
    assert.strictEqual(inspector.close(), undefined);
  },
};

export const inspectorConsole = {
  test() {
    assert.strictEqual(typeof inspector.console, 'object');

    const consoleMethods = [
      'debug',
      'error',
      'info',
      'log',
      'warn',
      'dir',
      'dirxml',
      'table',
      'trace',
      'group',
      'groupCollapsed',
      'groupEnd',
      'clear',
      'count',
      'countReset',
      'assert',
      'profile',
      'profileEnd',
      'time',
      'timeLog',
      'timeStamp',
    ];

    for (const method of consoleMethods) {
      assert.strictEqual(typeof inspector.console[method], 'function');
      assert.doesNotThrow(() => inspector.console[method]());
      assert.strictEqual(inspector.console[method](), undefined);
      assert.strictEqual(inspector.console[method]('test'), undefined);
      assert.strictEqual(inspector.console[method]('test', 'args'), undefined);
    }
  },
};

export const inspectorOpen = {
  test() {
    const disposable = inspector.open();
    assert.strictEqual(typeof disposable, 'object');
    assert.strictEqual(typeof disposable[Symbol.dispose], 'function');

    const disposePromise = disposable[Symbol.dispose]();
    assert.strictEqual(disposePromise instanceof Promise, true);

    disposable[Symbol.dispose]().then((result) => {
      assert.strictEqual(result, undefined);
    });

    assert.doesNotThrow(() => inspector.open(9229));
    assert.doesNotThrow(() => inspector.open(9229, 'localhost'));
    assert.doesNotThrow(() => inspector.open(9229, 'localhost', true));
    assert.doesNotThrow(() => inspector.open(undefined, undefined, false));
  },
};

export const inspectorUrl = {
  test() {
    assert.strictEqual(inspector.url(), undefined);
    assert.strictEqual(typeof inspector.url(), 'undefined');
  },
};

export const inspectorWaitForDebugger = {
  test() {
    assert.doesNotThrow(() => inspector.waitForDebugger());
    assert.strictEqual(inspector.waitForDebugger(), undefined);
  },
};

export const inspectorSession = {
  test() {
    assert.throws(() => new inspector.Session(), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
      message: /Session/,
    });

    assert.strictEqual(typeof inspector.Session, 'function');
  },
};

export const inspectorSessionMethods = {
  test() {
    let session;
    try {
      session = new inspector.Session();
    } catch (e) {
      const SessionProto = inspector.Session.prototype;

      assert.strictEqual(typeof SessionProto.connect, 'function');
      assert.strictEqual(typeof SessionProto.connectToMainThread, 'function');
      assert.strictEqual(typeof SessionProto.disconnect, 'function');
      assert.strictEqual(typeof SessionProto.post, 'function');

      assert.strictEqual(typeof SessionProto.on, 'function');
      assert.strictEqual(typeof SessionProto.emit, 'function');
      assert.strictEqual(typeof SessionProto.once, 'function');
      assert.strictEqual(typeof SessionProto.removeListener, 'function');
    }
  },
};

export const inspectorNetwork = {
  test() {
    assert.strictEqual(typeof inspector.Network, 'object');

    assert.throws(() => inspector.Network.requestWillBeSent({}), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
      message: /Network\.requestWillBeSent/,
    });

    assert.throws(() => inspector.Network.dataReceived({}), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
      message: /Network\.dataReceived/,
    });

    assert.throws(() => inspector.Network.responseReceived({}), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
      message: /Network\.responseReceived/,
    });

    assert.throws(() => inspector.Network.loadingFinished({}), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
      message: /Network\.loadingFinished/,
    });

    assert.throws(() => inspector.Network.loadingFailed({}), {
      code: 'ERR_METHOD_NOT_IMPLEMENTED',
      message: /Network\.loadingFailed/,
    });
  },
};

export const inspectorNetworkMethods = {
  test() {
    const networkMethods = [
      'requestWillBeSent',
      'dataReceived',
      'responseReceived',
      'loadingFinished',
      'loadingFailed',
    ];

    for (const method of networkMethods) {
      assert.strictEqual(typeof inspector.Network[method], 'function');
      assert.throws(() => inspector.Network[method]({}), {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      });
      assert.throws(() => inspector.Network[method](undefined), {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      });
      assert.throws(() => inspector.Network[method](null), {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      });
    }
  },
};

export const inspectorDefaultExport = {
  test() {
    assert.strictEqual(typeof inspector, 'object');
    assert.strictEqual(typeof inspector.Session, 'function');
    assert.strictEqual(typeof inspector.close, 'function');
    assert.strictEqual(typeof inspector.console, 'object');
    assert.strictEqual(typeof inspector.open, 'function');
    assert.strictEqual(typeof inspector.url, 'function');
    assert.strictEqual(typeof inspector.waitForDebugger, 'function');
    assert.strictEqual(typeof inspector.Network, 'object');
  },
};

export const inspectorModuleExports = {
  async test() {
    const inspectorModule = await import('node:inspector');

    assert.strictEqual(typeof inspectorModule.default, 'object');
    assert.strictEqual(typeof inspectorModule.Session, 'function');
    assert.strictEqual(typeof inspectorModule.close, 'function');
    assert.strictEqual(typeof inspectorModule.console, 'object');
    assert.strictEqual(typeof inspectorModule.open, 'function');
    assert.strictEqual(typeof inspectorModule.url, 'function');
    assert.strictEqual(typeof inspectorModule.waitForDebugger, 'function');
    assert.strictEqual(typeof inspectorModule.Network, 'object');

    assert.strictEqual(
      inspectorModule.default.Session,
      inspectorModule.Session
    );
    assert.strictEqual(inspectorModule.default.close, inspectorModule.close);
    assert.strictEqual(
      inspectorModule.default.console,
      inspectorModule.console
    );
    assert.strictEqual(inspectorModule.default.open, inspectorModule.open);
    assert.strictEqual(inspectorModule.default.url, inspectorModule.url);
    assert.strictEqual(
      inspectorModule.default.waitForDebugger,
      inspectorModule.waitForDebugger
    );
    assert.strictEqual(
      inspectorModule.default.Network,
      inspectorModule.Network
    );
  },
};

export const inspectorOpenReturnValue = {
  async test() {
    const disposable1 = inspector.open();
    const disposable2 = inspector.open(8080);
    const disposable3 = inspector.open(8080, '127.0.0.1');
    const disposable4 = inspector.open(8080, '127.0.0.1', false);

    for (const disposable of [
      disposable1,
      disposable2,
      disposable3,
      disposable4,
    ]) {
      assert.strictEqual(typeof disposable, 'object');
      assert.strictEqual(typeof disposable[Symbol.dispose], 'function');

      const result = await disposable[Symbol.dispose]();
      assert.strictEqual(result, undefined);
    }
  },
};

export const inspectorConsoleNoOp = {
  test() {
    const testArgs = [
      [],
      ['test'],
      ['test', 123],
      ['test', 123, { key: 'value' }],
      [null, undefined, NaN, Infinity],
      [() => {}, [], {}],
    ];

    const consoleMethods = Object.keys(inspector.console);

    for (const method of consoleMethods) {
      for (const args of testArgs) {
        assert.strictEqual(inspector.console[method](...args), undefined);
      }
    }
  },
};

export const inspectorSessionInheritance = {
  test() {
    assert.strictEqual(
      inspector.Session.prototype instanceof EventEmitter,
      true
    );

    const proto = inspector.Session.prototype;
    const eventMethods = [
      'on',
      'once',
      'emit',
      'removeListener',
      'removeAllListeners',
      'setMaxListeners',
      'getMaxListeners',
      'listeners',
      'rawListeners',
      'listenerCount',
      'prependListener',
      'prependOnceListener',
      'eventNames',
    ];

    for (const method of eventMethods) {
      assert.strictEqual(typeof proto[method], 'function');
    }
  },
};
