import { strictEqual, throws, ok, deepStrictEqual, rejects } from 'node:assert';
import * as worker_threads from 'node:worker_threads';
import {
  BroadcastChannel,
  MessageChannel,
  MessagePort,
  Worker,
  SHARE_ENV,
  getEnvironmentData,
  isMainThread,
  isMarkedAsUntransferable,
  markAsUntransferable,
  markAsUncloneable,
  moveMessagePortToContext,
  parentPort,
  receiveMessageOnPort,
  resourceLimits,
  setEnvironmentData,
  postMessageToThread,
  threadId,
  workerData,
  isInternalThread,
} from 'node:worker_threads';

export const testExports = {
  async test() {
    strictEqual(typeof BroadcastChannel, 'function');
    strictEqual(typeof MessageChannel, 'function');
    strictEqual(typeof MessagePort, 'function');
    strictEqual(typeof Worker, 'function');
    strictEqual(typeof getEnvironmentData, 'function');
    strictEqual(typeof setEnvironmentData, 'function');
    strictEqual(typeof isMarkedAsUntransferable, 'function');
    strictEqual(typeof markAsUntransferable, 'function');
    strictEqual(typeof markAsUncloneable, 'function');
    strictEqual(typeof moveMessagePortToContext, 'function');
    strictEqual(typeof receiveMessageOnPort, 'function');
    strictEqual(typeof postMessageToThread, 'function');

    strictEqual(typeof isMainThread, 'boolean');
    strictEqual(typeof SHARE_ENV, 'symbol');
    strictEqual(typeof resourceLimits, 'object');
    strictEqual(typeof threadId, 'number');
    strictEqual(typeof isInternalThread, 'boolean');

    strictEqual(worker_threads.default.BroadcastChannel, BroadcastChannel);
    strictEqual(worker_threads.default.MessageChannel, MessageChannel);
    strictEqual(worker_threads.default.MessagePort, MessagePort);
    strictEqual(worker_threads.default.Worker, Worker);
    strictEqual(worker_threads.default.getEnvironmentData, getEnvironmentData);
    strictEqual(worker_threads.default.setEnvironmentData, setEnvironmentData);
    strictEqual(worker_threads.default.isMainThread, isMainThread);
    strictEqual(
      worker_threads.default.isMarkedAsUntransferable,
      isMarkedAsUntransferable
    );
    strictEqual(
      worker_threads.default.markAsUntransferable,
      markAsUntransferable
    );
    strictEqual(worker_threads.default.markAsUncloneable, markAsUncloneable);
    strictEqual(
      worker_threads.default.moveMessagePortToContext,
      moveMessagePortToContext
    );
    strictEqual(worker_threads.default.parentPort, parentPort);
    strictEqual(
      worker_threads.default.receiveMessageOnPort,
      receiveMessageOnPort
    );
    strictEqual(worker_threads.default.resourceLimits, resourceLimits);
    strictEqual(worker_threads.default.SHARE_ENV, SHARE_ENV);
    strictEqual(
      worker_threads.default.postMessageToThread,
      postMessageToThread
    );
    strictEqual(worker_threads.default.threadId, threadId);
    strictEqual(worker_threads.default.workerData, workerData);
    strictEqual(worker_threads.default.isInternalThread, isInternalThread);
  },
};

export const testBroadcastChannelConstructor = {
  async test() {
    throws(
      () => {
        new BroadcastChannel();
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testWorkerConstructor = {
  async test() {
    throws(
      () => {
        new Worker();
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testWorkerProperties = {
  async test() {
    const proto = Worker.prototype;

    strictEqual(typeof proto.postMessage, 'function');
    strictEqual(typeof proto.postMessageToThread, 'function');
    strictEqual(typeof proto.ref, 'function');
    strictEqual(typeof proto.unref, 'function');
    strictEqual(typeof proto.terminate, 'function');
    strictEqual(typeof proto.getHeapSnapshot, 'function');
    strictEqual(typeof proto.getHeapStatistics, 'function');

    ok(Symbol.asyncDispose in proto);
    strictEqual(typeof proto[Symbol.asyncDispose], 'function');
  },
};

export const testMessageChannelAndPort = {
  async test() {
    // Only check reference equality if the global MessageChannel is exposed.
    // The node:worker_threads module now imports from cloudflare-internal:messagechannel
    // so it works independently of the expose_global_message_channel compat flag.
    if (typeof globalThis.MessageChannel !== 'undefined') {
      strictEqual(MessageChannel, globalThis.MessageChannel);
      strictEqual(MessagePort, globalThis.MessagePort);
    }

    const channel = new MessageChannel();
    ok(channel instanceof MessageChannel);
    ok(channel.port1 instanceof MessagePort);
    ok(channel.port2 instanceof MessagePort);
  },
};

export const testEnvironmentData = {
  async test() {
    const key1 = 'test_key_1';
    const key2 = 'test_key_2';
    const value1 = 'test_value_1';
    const value2 = { test: 'object' };

    strictEqual(getEnvironmentData(key1), undefined);
    strictEqual(getEnvironmentData(key2), undefined);

    setEnvironmentData(key1, value1);
    strictEqual(getEnvironmentData(key1), value1);

    setEnvironmentData(key2, value2);
    deepStrictEqual(getEnvironmentData(key2), value2);

    setEnvironmentData(key1, null);
    strictEqual(getEnvironmentData(key1), null);
  },
};

export const testIsMainThread = {
  async test() {
    strictEqual(isMainThread, true);
    strictEqual(typeof isMainThread, 'boolean');
  },
};

export const testIsMarkedAsUntransferable = {
  async test() {
    strictEqual(isMarkedAsUntransferable({}), false);
    strictEqual(isMarkedAsUntransferable(null), false);
    strictEqual(isMarkedAsUntransferable(undefined), false);
    strictEqual(isMarkedAsUntransferable(123), false);
    strictEqual(isMarkedAsUntransferable('string'), false);
    strictEqual(isMarkedAsUntransferable(new ArrayBuffer(8)), false);
  },
};

export const testMarkAsUntransferable = {
  async test() {
    const obj = {};
    const buffer = new ArrayBuffer(8);

    markAsUntransferable(obj);
    markAsUntransferable(buffer);
    markAsUntransferable(null);
    markAsUntransferable(undefined);

    ok(true);
  },
};

export const testMarkAsUncloneable = {
  async test() {
    const obj = {};
    const buffer = new ArrayBuffer(8);

    markAsUncloneable(obj);
    markAsUncloneable(buffer);
    markAsUncloneable(null);
    markAsUncloneable(undefined);

    ok(true);
  },
};

export const testParentPort = {
  async test() {
    strictEqual(parentPort, null);
  },
};

export const testReceiveMessageOnPort = {
  async test() {
    const channel = new MessageChannel();

    strictEqual(receiveMessageOnPort(channel.port1), undefined);
    strictEqual(receiveMessageOnPort(channel.port2), undefined);
  },
};

export const testSHARE_ENV = {
  async test() {
    strictEqual(typeof SHARE_ENV, 'symbol');
    strictEqual(SHARE_ENV, Symbol.for('nodejs.worker_threads.SHARE_ENV'));
  },
};

export const testResourceLimits = {
  async test() {
    strictEqual(typeof resourceLimits, 'object');
    ok(resourceLimits !== null);
    deepStrictEqual(resourceLimits, {});
  },
};

export const testThreadId = {
  async test() {
    strictEqual(typeof threadId, 'number');
    strictEqual(threadId, 0);
  },
};

export const testWorkerData = {
  async test() {
    strictEqual(workerData, null);
  },
};

export const testPostMessageToThread = {
  async test() {
    throws(
      () => {
        postMessageToThread(1, 'message');
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        postMessageToThread(1, 'message', 1000);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        postMessageToThread(0, {});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );

    throws(
      () => {
        postMessageToThread(123, null);
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testIsInternalThread = {
  async test() {
    strictEqual(typeof isInternalThread, 'boolean');
    strictEqual(isInternalThread, false);
  },
};

export const testDefaultExport = {
  async test() {
    const defaultExport = worker_threads.default;

    ok(defaultExport);
    strictEqual(typeof defaultExport, 'object');

    ok('BroadcastChannel' in defaultExport);
    ok('MessageChannel' in defaultExport);
    ok('MessagePort' in defaultExport);
    ok('Worker' in defaultExport);
    ok('SHARE_ENV' in defaultExport);
    ok('getEnvironmentData' in defaultExport);
    ok('isMainThread' in defaultExport);
    ok('isMarkedAsUntransferable' in defaultExport);
    ok('markAsUntransferable' in defaultExport);
    ok('markAsUncloneable' in defaultExport);
    ok('moveMessagePortToContext' in defaultExport);
    ok('parentPort' in defaultExport);
    ok('receiveMessageOnPort' in defaultExport);
    ok('resourceLimits' in defaultExport);
    ok('setEnvironmentData' in defaultExport);
    ok('postMessageToThread' in defaultExport);
    ok('threadId' in defaultExport);
    ok('workerData' in defaultExport);
    ok('isInternalThread' in defaultExport);

    const expectedKeys = [
      'BroadcastChannel',
      'MessageChannel',
      'MessagePort',
      'Worker',
      'SHARE_ENV',
      'getEnvironmentData',
      'isMainThread',
      'isMarkedAsUntransferable',
      'markAsUntransferable',
      'markAsUncloneable',
      'moveMessagePortToContext',
      'parentPort',
      'receiveMessageOnPort',
      'resourceLimits',
      'setEnvironmentData',
      'postMessageToThread',
      'threadId',
      'workerData',
      'isInternalThread',
    ];
    deepStrictEqual(Object.keys(defaultExport).sort(), expectedKeys.sort());
  },
};

export const testWorkerTerminate = {
  async test() {
    const proto = Worker.prototype;
    await rejects(
      async () => {
        await proto.terminate.call({});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testWorkerGetHeapSnapshot = {
  async test() {
    const proto = Worker.prototype;
    await rejects(
      async () => {
        await proto.getHeapSnapshot.call({});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};

export const testWorkerGetHeapStatistics = {
  async test() {
    const proto = Worker.prototype;
    await rejects(
      async () => {
        await proto.getHeapStatistics.call({});
      },
      {
        code: 'ERR_METHOD_NOT_IMPLEMENTED',
      }
    );
  },
};
