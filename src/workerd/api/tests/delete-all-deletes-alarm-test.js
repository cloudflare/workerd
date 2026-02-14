// Tests that the delete_all_deletes_alarm compat flag correctly controls whether
// deleteAll() also deletes alarms. This file is used by two test services: one
// with the flag enabled and one with it disabled. The EXPECT_ALARM_DELETED binding
// tells this code which behavior to expect.

import * as assert from 'node:assert';

export class DurableObjectExample {
  constructor(state) {
    this.state = state;
  }

  async fetch(request) {
    const url = new URL(request.url);
    const expectDeleted = url.searchParams.get('expectDeleted') === 'true';

    // Set an alarm for 10 minutes from now.
    const alarmTime = Date.now() + 10 * 60 * 1000;
    await this.state.storage.setAlarm(alarmTime);
    assert.equal(await this.state.storage.getAlarm(), alarmTime);

    // Also put some KV data so deleteAll has something to delete.
    await this.state.storage.put('key', 'value');

    // Call deleteAll().
    await this.state.storage.deleteAll();

    // KV data should always be deleted.
    assert.equal(await this.state.storage.get('key'), undefined);

    const alarmAfter = await this.state.storage.getAlarm();

    if (expectDeleted) {
      // With the compat flag enabled, the alarm should be deleted.
      assert.equal(
        alarmAfter,
        null,
        `Expected alarm to be null after deleteAll(), got ${alarmAfter}`
      );
    } else {
      // Without the compat flag, the alarm should be preserved.
      assert.equal(
        alarmAfter,
        alarmTime,
        `Expected alarm to be preserved after deleteAll(), got ${alarmAfter}`
      );
      // Clean up the alarm so it doesn't fire during the test.
      await this.state.storage.deleteAlarm();
    }

    return new Response('OK');
  }

  async alarm() {
    // Should not be invoked during the test.
    throw new Error('alarm handler unexpectedly invoked');
  }
}

export const test = {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName('A');
    let obj = env.ns.get(id);
    let res = await obj.fetch(
      `http://foo/test?expectDeleted=${env.EXPECT_ALARM_DELETED}`
    );
    let text = await res.text();
    assert.equal(text, 'OK');
  },
};
