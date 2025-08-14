import { notStrictEqual } from 'assert';
import { DurableObject } from 'cloudflare:workers';

export class Foo extends DurableObject {
  constructor(state, env) {
    super(state, env)
    this.state = state
  }

  async foo() {
    await scheduler.wait(10);
    let id = this.env.Bar.idFromName('bar');
    let stub = this.env.Bar.get(id);

    try {
      await stub.bar();
    } catch (e) {
      console.log('err:', e)
      throw e;
    }
  }

  async alarm() {
    console.log('alarm called')
    try {
      await this.foo()
      this.resolve();
    } catch (e) {
      this.reject(e);
    }
  }

  async fetch1() {
    console.log('fetch called')
    const time = Date.now() + 50;
    await this.state.storage.setAlarm(time);

    await this.waitForAlarm(time);
  }


  async waitForAlarm(scheduledTime) {
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
}

export const test = {
  async test(_, env) {
    try {
      let id = env.Foo.idFromName('foo');
      let stub = env.Foo.get(id);
      await stub.fetch1();
    } catch (e) {
      // e.stack should not contain bar.blah()
      // if it does then we're leaking

    }
  },
};
