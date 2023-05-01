import * as assert from 'node:assert'

async function timeout(ms) {
  await scheduler.wait(ms);
  throw new Error(`timed out`);
}

export class DurableObjectExample {
  constructor(state, env) {
    this.state = state;
  }

  async waitForAlarm(scheduledTime, timeoutOverrideMs) {
    console.log(`waiting for ${scheduledTime.valueOf()}`);
    let self = this;
    let prom = new Promise((resolve) => {
      self.resolve = resolve;
    });

    let timeMs = scheduledTime.valueOf();
    let timeoutMs = (timeMs - Date.now().valueOf()) + (timeoutOverrideMs ?? 4000);
    try {
      await Promise.race([prom, timeout(timeoutMs)]);
      if (Date.now() < scheduledTime.valueOf()) {
        throw new Error(`Date.now() is before scheduledTime! ${Date.now()} vs ${scheduledTime.valueOf()}`);
      }
    } catch (e) {
      throw new Error(`error waiting for alarm at ${scheduledTime.valueOf()}: ${e}`);
    }

    let alarm = await this.state.storage.getAlarm();
    if (alarm != null) {
      throw new Error(`alarm time not cleared when handler ends. ${alarm}`);
    }
  }

  async fetch(req) {
    let url = new URL(req.url);

    const time = Date.now() + 100;
    await this.state.storage.setAlarm(time);
    assert.equal(await this.state.storage.getAlarm(), time);

    await this.waitForAlarm(time, 3000);

    return new Response("OK");
  }

  async alarm() {
    console.log("alarm()");
    let time = await this.state.storage.getAlarm();
    if (time) {
      throw new Error(`time not null inside alarm handler ${time}`);
    }
    this.resolve();
  }
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName("A");
    let obj = env.ns.get(id);
    let res = await obj.fetch("http://foo/test");
    let text = await res.text();
    assert.equal(text, "OK");
  }
}
