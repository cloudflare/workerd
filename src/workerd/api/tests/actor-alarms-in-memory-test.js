// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

export class DurableObjectExample {
  constructor(state, env) {
    this.state = state;
    this.alarmsTriggered = 0;
  }

  async waitForAlarm(scheduledTime) {
    // eslint-disable-next-line @typescript-eslint/no-this-alias
    let self = this;
    const { promise, resolve, reject } = Promise.withResolvers();
    self.resolve = resolve;
    self.reject = reject;

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
    const time = Date.now() + 50;
    this.scheduledTime = time;
    await this.state.storage.setAlarm(time);
    assert.equal(await this.state.storage.getAlarm(), time);

    await new Promise((resolve) => setTimeout(resolve, 200));

    return new Response(String(this.alarmsTriggered));
  }

  async alarm() {
    this.alarmsTriggered++;
    if (this.alarmsTriggered === 1) {
      await this.state.storage.setAlarm(Date.now() + 50);
    }
  }
}

export const test = {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('A');
    let obj = env.ns.get(id);
    let res = await obj.fetch('http://foo/test');
    let text = await res.text();
    assert.equal(text, '2');
  },
};
