// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The TestController passed to test() and the AlarmInvocationInfo passed to
// alarm(), which src/per_isolate/handlers/handlers.ts implements so that they
// look as JSG resource types do.

import * as assert from 'node:assert';
import { inspect } from 'node:util';
import { DurableObject } from 'cloudflare:workers';

const flags = Cloudflare.compatibilityFlags;
const kInternalClass = Symbol.for('cloudflare:internal-class');

// What JSG gives every resource type's constructor and prototype.
function assertRuntimeClass(value, name) {
  const proto = Object.getPrototypeOf(value);
  const ctor = proto.constructor;
  assert.strictEqual(typeof ctor, 'function');
  assert.strictEqual(ctor.name, name);
  assert.strictEqual(ctor.length, 0);
  assert.strictEqual(Object.getPrototypeOf(ctor), Function.prototype);
  assert.strictEqual(Object.getPrototypeOf(proto), Object.prototype);
  assert.deepStrictEqual(Object.getOwnPropertyDescriptor(ctor, 'prototype'), {
    value: proto,
    writable: flags.immutable_api_prototypes !== true,
    enumerable: false,
    configurable: false,
  });
  assert.deepStrictEqual(
    Object.getOwnPropertyDescriptor(proto, 'constructor'),
    { value: ctor, writable: true, enumerable: false, configurable: true }
  );

  const keys = Reflect.ownKeys(proto).map(String);
  assert.deepStrictEqual(keys, [
    'constructor',
    ...(flags.set_tostring_tag ? ['Symbol(Symbol.toStringTag)'] : []),
    'Symbol(cloudflare:internal-class)',
    'Symbol(kResourceTypeInspect)',
  ]);
  assert.deepStrictEqual(
    Object.getOwnPropertyDescriptor(proto, kInternalClass),
    {
      value: kInternalClass,
      writable: false,
      enumerable: false,
      configurable: false,
    }
  );
  if (flags.set_tostring_tag) {
    assert.deepStrictEqual(
      Object.getOwnPropertyDescriptor(proto, Symbol.toStringTag),
      { value: name, writable: true, enumerable: false, configurable: true }
    );
  }
  assert.strictEqual(
    Object.prototype.toString.call(value),
    flags.set_tostring_tag ? `[object ${name}]` : '[object Object]'
  );

  // Illegal with or without `new`.
  const illegal = { name: 'TypeError', message: 'Illegal constructor' };
  assert.throws(() => new ctor(), illegal);
  assert.throws(() => ctor(), illegal);
  assert.throws(() => Reflect.construct(ctor, []), illegal);

  assert.ok(Object.isExtensible(value));
}

export class EchoObject extends DurableObject {
  echo(value) {
    return value;
  }
}

export const testController = {
  async test(ctrl, env) {
    assertRuntimeClass(ctrl, 'TestController');
    assert.deepStrictEqual(Reflect.ownKeys(ctrl), []);
    assert.strictEqual(inspect(ctrl), 'TestController {}');
    assert.strictEqual(JSON.stringify(ctrl), '{}');
    // An object without internal fields clones as a plain object.
    assert.deepStrictEqual(structuredClone(ctrl), {});

    // Not an RpcTarget or a plain object, so RPC refuses it.
    const stub = env.echo.get(env.echo.idFromName('echo'));
    await assert.rejects(() => stub.echo(ctrl), {
      name: 'DataCloneError',
      message:
        'Could not serialize object of type "TestController". This type ' +
        'does not support serialization.',
    });
  },
};

export const alarmInvocationInfo = {
  async test(ctrl, env) {
    // RPC refuses an AlarmInvocationInfo, so the Durable Object checks each
    // one itself and returns a copy of its properties.
    const stub = env.alarm.get(env.alarm.idFromName('alarm'));
    const { scheduledTime, infos } = await stub.run();
    assert.deepStrictEqual(infos, [
      { scheduledTime, retryCount: 0, isRetry: false },
      { scheduledTime, retryCount: 1, isRetry: true },
    ]);
  },
};

export class AlarmObject extends DurableObject {
  async run() {
    const { promise, resolve, reject } = Promise.withResolvers();
    this.resolve = resolve;
    this.reject = reject;
    this.infos = [];
    // setAlarm() can clamp the time to the runtime clock, so the alarm is read
    // back before it can run.
    await this.ctx.blockConcurrencyWhile(async () => {
      await this.ctx.storage.setAlarm(Date.now());
      this.scheduledTime = await this.ctx.storage.getAlarm();
    });
    await promise;
    return { scheduledTime: this.scheduledTime, infos: this.infos };
  }

  async alarm(info) {
    try {
      assertRuntimeClass(info, 'AlarmInvocationInfo');
      const keys = ['scheduledTime', 'retryCount', 'isRetry'];
      assert.deepStrictEqual(Reflect.ownKeys(info), keys);
      for (const key of keys) {
        assert.deepStrictEqual(Object.getOwnPropertyDescriptor(info, key), {
          value: info[key],
          writable: false,
          enumerable: true,
          configurable: true,
        });
      }
      assert.strictEqual(typeof info.scheduledTime, 'number');
      assert.strictEqual(
        inspect(info),
        `AlarmInvocationInfo { scheduledTime: ${info.scheduledTime}, ` +
          `retryCount: ${info.retryCount}, isRetry: ${info.isRetry} }`
      );
      assert.strictEqual(
        JSON.stringify(info),
        `{"scheduledTime":${info.scheduledTime},` +
          `"retryCount":${info.retryCount},"isRetry":${info.isRetry}}`
      );
      // An object without internal fields is serialized as a plain object.
      assert.deepStrictEqual(structuredClone(info), { ...info });
      await this.ctx.storage.put('info', info);
      assert.deepStrictEqual(await this.ctx.storage.get('info'), { ...info });
      // Read through an object that inherits from it.
      assert.strictEqual(Object.create(info).retryCount, info.retryCount);

      const stub = this.env.echo.get(this.env.echo.idFromName('echo'));
      await assert.rejects(() => stub.echo(info), {
        name: 'DataCloneError',
        message:
          'Could not serialize object of type "AlarmInvocationInfo". This ' +
          'type does not support serialization.',
      });

      this.infos.push({ ...info });

      // Read-only, but configurable.
      assert.throws(
        () => {
          info.retryCount = 42;
        },
        {
          name: 'TypeError',
          message:
            "Cannot assign to read only property 'retryCount' of object " +
            "'#<AlarmInvocationInfo>'",
        }
      );
      assert.ok(delete info.retryCount);
      assert.strictEqual(info.retryCount, undefined);
    } catch (e) {
      this.reject(e);
      return;
    }
    // Throwing makes the alarm run again, as a retry.
    if (this.infos.length === 1) throw new Error('retry me');
    this.resolve();
  }
}
