// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as assert from 'node:assert';

// With in-memory storage, alarms are run by a task that the alarm itself can replace or clear.
// These tests cover updating the alarm from inside that task.

export class AlarmClearObject {
  constructor(state) {
    this.state = state;
  }

  async fetch() {
    // Running the alarm clears it afterwards, from within the task that ran it.
    await this.state.storage.setAlarm(Date.now());
    await scheduler.wait(500);

    assert.equal(await this.state.storage.getAlarm(), null);
    return new Response('OK');
  }

  async alarm() {}
}

export class AlarmUpdateObject {
  constructor(state) {
    this.state = state;
  }

  async fetch() {
    const { promise, resolve, reject } = Promise.withResolvers();
    this.resolveAlarm = resolve;
    this.rejectAlarm = reject;

    await this.state.storage.setAlarm(Date.now() + 50);
    await promise;

    assert.equal(await this.state.storage.getAlarm(), null);
    return new Response('OK');
  }

  async alarm() {
    try {
      const future = Date.now() + 60_000;
      await Promise.all([
        this.state.storage.setAlarm(future),
        this.state.storage.deleteAlarm(),
        this.state.storage.setAlarm(future + 1),
        this.state.storage.deleteAlarm(),
      ]);
      this.resolveAlarm();
    } catch (error) {
      this.rejectAlarm(error);
    }
  }
}

export const clearAfterAlarm = {
  async test(ctrl, env) {
    const id = env.clearNs.idFromName('clear-after-alarm');
    const response = await env.clearNs.get(id).fetch('http://example.test/');
    assert.equal(await response.text(), 'OK');
  },
};

export const updateDuringAlarm = {
  async test(ctrl, env) {
    const id = env.updateNs.idFromName('update-during-alarm');
    const response = await env.updateNs.get(id).fetch('http://example.test/');
    assert.equal(await response.text(), 'OK');
  },
};
