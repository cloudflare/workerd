// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export class DurableObjectExample {
  constructor(state, env) {
    this.state = state;
  }

  async waitForAlarm(scheduledTime, promise) {
    try {
      await promise;
      if (Date.now() < scheduledTime.valueOf()) {
        throw new Error(
          `Date.now() is before scheduledTime! ${Date.now()} vs ${scheduledTime.valueOf()}`
        );
      }
    } catch (e) {
      throw new Error(
        `error waiting for alarm at ${scheduledTime.valueOf()}: ${e}`
      );
    }

    let alarm = await this.state.storage.getAlarm();
    if (alarm != null) {
      throw new Error(`alarm time not cleared when handler ends. ${alarm}`);
    }
  }

  async fetch() {
    const { promise, resolve, reject } = Promise.withResolvers();
    this.resolve = resolve;
    this.reject = reject;
    await this.state.blockConcurrencyWhile(async () => {
      const time = Date.now() + 50;
      await this.state.storage.setAlarm(time);
      const storedTime = await this.state.storage.getAlarm();
      // setAlarm clamps expired dates to the runtime clock. Hold the input gate until the
      // stored deadline and completion handlers are ready for the alarm invocation.
      assert.ok(storedTime >= time);
      assert.ok(storedTime <= Math.max(time, Date.now()));
      this.scheduledTime = storedTime;
    });

    await this.waitForAlarm(this.scheduledTime, promise);

    return new Response('OK');
  }

  async alarm(alarmInfo) {
    try {
      let time = await this.state.storage.getAlarm();
      if (time !== null) {
        throw new Error(`time not null inside alarm handler ${time}`);
      }
      assert.strictEqual(typeof alarmInfo.scheduledTime, 'number');
      assert.strictEqual(alarmInfo.scheduledTime, this.scheduledTime);
      this.resolve();
    } catch (e) {
      this.reject(e);
    }
  }
}

export const test = {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('A');
    let obj = env.ns.get(id);
    let res = await obj.fetch('http://foo/test');
    let text = await res.text();
    assert.equal(text, 'OK');
  },
};
