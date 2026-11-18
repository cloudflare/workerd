// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
  DurableObject,
  RpcTarget,
  WorkerEntrypoint,
  restore,
} from 'cloudflare:workers';
import unsafe from 'workerd:unsafe';

// The identity of this isolate: global state, so each loop thread's copy of the Worker has its
// own. The Durable Objects are classes of this Worker and run in the copy of the thread that has
// them, so an object reports that thread's identity.
let isolate;

function isolateId() {
  isolate ??= crypto.randomUUID();
  return isolate;
}

function upperCase() {
  return new TransformStream({
    transform(chunk, controller) {
      controller.enqueue(
        chunk.map((byte) => (byte >= 97 && byte <= 122 ? byte - 32 : byte))
      );
    },
  });
}

class Session extends RpcTarget {
  #total = 0;

  add(n) {
    this.#total += n;
    return this.#total;
  }

  where() {
    return isolateId();
  }
}

export class Counter extends DurableObject {
  // A new one for every time the object is constructed.
  #instance = crypto.randomUUID();
  #messages = 0;
  #order = [];

  where() {
    return { isolate: isolateId(), instance: this.#instance };
  }

  idName() {
    return this.ctx.id.name ?? null;
  }

  async increment() {
    const count = ((await this.ctx.storage.get('count')) ?? 0) + 1;
    await this.ctx.storage.put('count', count);
    return { count, isolate: isolateId() };
  }

  async incrementSlowly(ms) {
    await scheduler.wait(ms);
    return await this.increment();
  }

  record(n) {
    this.#order.push(n);
  }

  recorded() {
    return this.#order;
  }

  session() {
    return new Session();
  }

  // `other` is a stub of another Counter, made on the caller's thread.
  async incrementOther(other) {
    return await other.increment();
  }

  // `service` is a service binding of the caller's thread.
  async askService(service) {
    const response = await service.fetch('http://back/');
    return await response.json();
  }

  async greetWith(greeter) {
    return await greeter.greet('object');
  }

  async callBack(callback) {
    return await callback(isolateId());
  }

  async bumpFacet(name) {
    const facet = this.ctx.facets.get(name, () => ({
      class: this.ctx.exports.Bumper,
    }));
    return await facet.bump();
  }

  fail() {
    throw new RangeError('the object refused');
  }

  // A stub that this object's `[restore]()` makes.
  vend() {
    return this.ctx.restore({ by: 3 });
  }

  // A service that this object's `[restore]()` makes of `greeter`, a service of the caller's
  // thread.
  vendGreeter(greeter) {
    return this.ctx.restore({ greeter });
  }

  // The facet `name`, as this object's `[restore]()` gives it.
  vendFacet(name) {
    return this.ctx.restore({ facet: name });
  }

  [restore]({ by, greeter, facet }) {
    if (greeter !== undefined) return greeter;
    if (facet !== undefined) {
      return this.ctx.facets.get(facet, () => ({
        class: this.ctx.exports.Bumper,
      }));
    }
    const session = new Session();
    session.add(by);
    return session;
  }

  async setAlarmIn(ms) {
    await this.ctx.storage.setAlarm(Date.now() + ms);
  }

  async alarm() {
    const alarms = ((await this.ctx.storage.get('alarms')) ?? 0) + 1;
    await this.ctx.storage.put('alarms', alarms);
  }

  async alarms() {
    return (await this.ctx.storage.get('alarms')) ?? 0;
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/echo') {
      return new Response(request.body.pipeThrough(upperCase()), {
        headers: { 'x-object': isolateId() },
      });
    }
    if (url.pathname === '/ws') {
      const [client, server] = Object.values(new WebSocketPair());
      if (url.searchParams.has('hibernate')) {
        this.ctx.acceptWebSocket(server);
      } else {
        server.accept();
        server.addEventListener('message', (event) => {
          server.send(this.#reply(event.data));
        });
        server.addEventListener('close', (event) => server.close(event.code));
      }
      return new Response(null, { status: 101, webSocket: client });
    }
    return new Response('not found', { status: 404 });
  }

  #reply(data) {
    return JSON.stringify({
      echo: data,
      instance: this.#instance,
      messages: ++this.#messages,
      isolate: isolateId(),
    });
  }

  webSocketMessage(ws, message) {
    ws.send(this.#reply(message));
  }

  webSocketClose(ws, code) {
    ws.close(code);
  }

  async connect(socket) {
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode(`object ${isolateId()}`));
    await writer.close();
  }
}

// A facet's class: it has no namespace of its own.
export class Bumper extends DurableObject {
  async bump() {
    const bumps = ((await this.ctx.storage.get('bumps')) ?? 0) + 1;
    await this.ctx.storage.put('bumps', bumps);
    return { bumps, isolate: isolateId() };
  }

  // A stub that this facet's `[restore]()` makes.
  vend() {
    return this.ctx.restore({ by: 5 });
  }

  [restore]({ by }) {
    const session = new Session();
    session.add(by);
    return session;
  }
}

export class Ephemeral extends DurableObject {
  #count = 0;

  increment() {
    return { count: ++this.#count, isolate: isolateId() };
  }
}

export class Greeter extends WorkerEntrypoint {
  greet(name) {
    return {
      greeting: `${this.ctx.props.greeting}, ${name}`,
      isolate: isolateId(),
    };
  }
}

// Its props hold a stub as what restores it, not as a live capability.
export class Keeper extends WorkerEntrypoint {
  async add(n) {
    return await this.ctx.props.stub.add(n);
  }
}

async function readAll(readable) {
  let text = '';
  for await (const chunk of readable.pipeThrough(new TextDecoderStream())) {
    text += chunk;
  }
  return text;
}

// What a stub does, each from this thread: the results, for the driver to check.
async function features(env, ctx, name) {
  const counter = env.COUNTER.getByName(name);
  const result = { name: await counter.idName() };

  // A stub the object returns, and calls on it in the order they were made.
  using session = await counter.session();
  const sums = await Promise.all([session.add(1), session.add(2)]);
  result.session = { sums, isolate: await session.where() };

  for (let n = 0; n < 50; n++) counter.record(n);
  result.recorded = await counter.recorded();

  // Stubs of this thread as arguments: the object gets its own thread's.
  const other = env.COUNTER.getByName(`${name}-other`);
  result.other = await counter.incrementOther(other);
  result.otherAgain = await other.increment();
  result.service = await counter.askService(env.BACK);
  result.greeter = await counter.greetWith(
    ctx.exports.Greeter({ props: { greeting: 'hello' } })
  );
  // A function of this thread: the object calls back into this isolate.
  result.callBack = await counter.callBack((object) => ({
    object,
    front: isolateId(),
  }));

  result.facet = [
    await counter.bumpFacet('a'),
    await counter.bumpFacet('a'),
    await counter.bumpFacet('b'),
  ];

  try {
    await counter.fail();
    result.error = null;
  } catch (error) {
    result.error = { name: error.name, message: error.message };
  }

  const ephemeral = env.EPHEMERAL.get(`${name}-ephemeral`);
  await ephemeral.increment();
  result.ephemeral = await ephemeral.increment();

  const exported = ctx.exports.Counter.getByName(`${name}-exported`);
  await exported.increment();
  result.exported = await env.COUNTER.getByName(`${name}-exported`).increment();

  const socket = counter.connect('object:1');
  result.connect = await readAll(socket.readable);

  // A stub the object vends through `restore()`, called from this thread.
  using vended = await counter.vend();
  result.vended = await vended.add(1);

  // The same stub restored from this thread: the object's `[restore]()` runs again, for an
  // event this thread sends it.
  const keeper = ctx.exports.Keeper({ props: { stub: vended } });
  result.restored = await keeper.add(1);

  // A service stub the object vended arrives as what restores it, and its parameters hold a
  // service of this thread: the object's `[restore]()` gets its own thread's.
  const greeter = await counter.vendGreeter(
    ctx.exports.Greeter({ props: { greeting: 'restored' } })
  );
  result.restoredGreeter = await greeter.greet('front');

  // A restore chain: the object's `[restore]()` gives a facet, whose own makes the stub.
  const facet = await counter.vendFacet('a');
  using deep = await facet.vend();
  result.restoredChain = await ctx.exports
    .Keeper({ props: { stub: deep } })
    .add(1);

  // An ephemeral object's stub cannot be an argument.
  try {
    await counter.incrementOther(ephemeral);
    result.ephemeralArgument = null;
  } catch (error) {
    result.ephemeralArgument = error.message;
  }
  return result;
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const name = url.searchParams.get('name') ?? 'default';
    const counter = env.COUNTER.getByName(name);
    const front = isolateId();
    switch (url.pathname) {
      case '/spin': {
        // Keeps this isolate's thread from serving anything else for a while.
        const until = Date.now() + Number(url.searchParams.get('ms'));
        while (Date.now() < until) {}
        return Response.json({ front });
      }
      case '/count': {
        // The response waits for the end of the request body, which the client decides.
        await request.arrayBuffer();
        return Response.json({ front, ...(await counter.increment()) });
      }
      case '/slow': {
        const ms = Number(url.searchParams.get('ms'));
        return Response.json({ front, ...(await counter.incrementSlowly(ms)) });
      }
      case '/where':
        return Response.json({ front, ...(await counter.where()) });
      case '/features':
        return Response.json({ front, ...(await features(env, ctx, name)) });
      case '/alarm':
        await counter.setAlarmIn(Number(url.searchParams.get('ms')));
        return Response.json({ front });
      case '/alarms':
        return Response.json({ front, alarms: await counter.alarms() });
      case '/abort-all':
      case '/delete-all': {
        // The test hooks of `workerd:unsafe`, which act on the objects of this thread.
        const hook =
          url.pathname === '/abort-all'
            ? unsafe.abortAllDurableObjects
            : unsafe.deleteAllDurableObjects;
        try {
          await hook.call(unsafe);
          return Response.json({ front, error: null });
        } catch (error) {
          return Response.json({ front, error: error.message });
        }
      }
      case '/echo': {
        const response = await counter.fetch('http://object/echo', {
          method: 'POST',
          body: request.body,
        });
        const headers = new Headers(response.headers);
        headers.set('x-front', front);
        return new Response(response.body, { headers });
      }
      case '/ws': {
        const response = await counter.fetch(request);
        return new Response(null, {
          status: 101,
          webSocket: response.webSocket,
          headers: { 'x-front': front },
        });
      }
    }
    return new Response('not found', { status: 404 });
  },
};
