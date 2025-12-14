import { WorkerEntrypoint, DurableObject } from 'cloudflare:workers';
import assert from 'node:assert';

// This test verifies that we can store ServiceStubs (Fetchers) and DurableObjectClasses in DO KV
// storage.

export class MyActor extends DurableObject {
  async get(name) {
    return await this.ctx.storage.get(name);
  }
  async put(name, value) {
    await this.ctx.storage.put(name, value);
  }

  async useFacet(name) {
    let facet = this.ctx.facets.get(name, async () => {
      let cls = await this.ctx.storage.get(name);
      return { class: cls, id: name };
    });

    await facet.setName('Bob');
    return await facet.greet();
  }
}

export class Greeter extends WorkerEntrypoint {
  async greet(name) {
    return `${this.ctx.props.greeting}, ${name}!`;
  }
}

export class MyFacet extends DurableObject {
  async setName(name) {
    await this.ctx.storage.put('name', name);
  }

  async greet() {
    let name = await this.ctx.storage.get('name');
    return `${this.ctx.props.greeting}, ${name}?`;
  }
}

export let storeServiceBinding = {
  async test(controller, env, ctx) {
    let id = ctx.exports.MyActor.idFromName('foo');
    let stub = ctx.exports.MyActor.get(id);

    {
      await stub.put('foo', ctx.exports.Greeter({ props: { greeting: 'Yo' } }));
      let greeter = await stub.get('foo');
      assert.strictEqual(await greeter.greet('Alice'), 'Yo, Alice!');
    }

    {
      await stub.put(
        'bar',
        ctx.exports.MyFacet({ props: { greeting: 'Hiya' } })
      );

      assert.strictEqual(await stub.useFacet('bar'), 'Hiya, Bob?');
    }
  },
};
