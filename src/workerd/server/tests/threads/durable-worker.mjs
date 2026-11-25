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
// own. The Durable Objects are classes of this Worker, and each runs in the copy of its home
// thread, so an object reports the identity of that thread.
let isolate;

function isolateId() {
  isolate ??= crypto.randomUUID();
  return isolate;
}

// The `x-attempt` of every request this isolate acted on.
const attempts = new Set();

const encoder = new TextEncoder();

function hex(buffer) {
  return [...new Uint8Array(buffer)]
    .map((byte) => byte.toString(16).padStart(2, '0'))
    .join('');
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
  #slowCalls = 0;
  // What the body of the last `/echo-digest` request was: `{ digest, bytes }`, once it ended.
  #received;

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
    this.#slowCalls++;
    await scheduler.wait(ms);
    return await this.increment();
  }

  // How many calls of `incrementSlowly()` this instance has had.
  slowCalls() {
    return this.#slowCalls;
  }

  async put(value) {
    await this.ctx.storage.put('value', value);
  }

  async value() {
    return (await this.ctx.storage.get('value')) ?? null;
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

  // The first leg of a cycle of calls: `other`, a stub of another object, calls this object
  // back by name.
  async ping(other) {
    const pong = await other.pong(this.ctx.id.name);
    return { ...this.where(), pong };
  }

  // The second leg: calls the object `name` back, by JS RPC and by fetch.
  async pong(name) {
    const back = this.env.COUNTER.getByName(name);
    const rpc = await back.finish();
    const response = await back.fetch('http://object/finish');
    return { ...this.where(), rpc, fetched: await response.json() };
  }

  finish() {
    return this.where();
  }

  // `service` is a service binding of the caller's thread.
  async askService(service) {
    const response = await service.fetch('http://back/');
    return await response.json();
  }

  // What this object's own service binding answers.
  async askOwnService() {
    const response = await this.env.BACK.fetch('http://back/');
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

  cloneFacet(from, to) {
    this.ctx.facets.clone(from, to);
  }

  deleteFacet(name) {
    this.ctx.facets.delete(name);
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

  async setAlarmAt(ms) {
    await this.ctx.storage.setAlarm(ms);
  }

  // Counts its runs, and notes the isolate of each.
  async alarm() {
    const alarms = ((await this.ctx.storage.get('alarms')) ?? 0) + 1;
    const isolates = (await this.ctx.storage.get('alarm-isolates')) ?? [];
    await this.ctx.storage.put({
      alarms,
      'alarm-isolates': [...isolates, isolateId()],
    });
  }

  // How many times `alarm()` ran, and in which isolates.
  async alarms() {
    return {
      alarms: (await this.ctx.storage.get('alarms')) ?? 0,
      isolates: (await this.ctx.storage.get('alarm-isolates')) ?? [],
    };
  }

  // What the body of the last `/echo-digest` request was.
  async received() {
    return await this.#received;
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === '/echo') {
      return new Response(request.body.pipeThrough(upperCase()), {
        headers: { 'x-object': isolateId() },
      });
    }
    if (url.pathname === '/echo-digest') {
      // As `/echo`, and notes the digest and the length of what arrived.
      const [kept, echoed] = request.body.tee();
      const digest = new crypto.DigestStream('SHA-256');
      let bytes = 0;
      const counted = new TransformStream({
        transform(chunk, controller) {
          bytes += chunk.byteLength;
          controller.enqueue(chunk);
        },
      });
      this.#received = kept
        .pipeThrough(counted)
        .pipeTo(digest)
        .then(async () => ({ digest: hex(await digest.digest), bytes }));
      return new Response(echoed.pipeThrough(upperCase()), {
        headers: { 'x-object': isolateId() },
      });
    }
    if (url.pathname === '/finish') {
      return Response.json(this.where());
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
    await writer.write(encoder.encode(`object ${isolateId()}`));
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

  where() {
    return isolateId();
  }

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

  async where() {
    return await this.ctx.props.stub.where();
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
  // Where each object of this case runs.
  result.homes = {
    counter: (await counter.where()).isolate,
    other: (await env.COUNTER.getByName(`${name}-other`).where()).isolate,
    exported: (await env.COUNTER.getByName(`${name}-exported`).where()).isolate,
    ephemeral: await env.EPHEMERAL.get(`${name}-ephemeral`).where(),
  };

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
  result.ownService = await counter.askOwnService();
  result.frontService = await (await env.BACK.fetch('http://back/')).json();
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

// Runs the test hook `hook` of `workerd:unsafe` (`abort`, `delete`, `evict-all`, or `evict`
// for the object `counter()` makes a stub of), then calls each of the objects `names` once its
// promise has resolved: when the hook was called and when it resolved, and where each object
// runs and what it stored.
async function hookThenCall(env, hook, counter, names) {
  const calledAt = Date.now();
  try {
    if (hook === 'abort') await unsafe.abortAllDurableObjects();
    else if (hook === 'delete') await unsafe.deleteAllDurableObjects();
    else if (hook === 'evict-all') await unsafe.evictAllDurableObjects();
    else if (hook === 'evict') await unsafe.evict(counter());
    else throw new Error(`no hook ${hook}`);
  } catch (error) {
    return { error: error.message };
  }
  const resolvedAt = Date.now();
  const objects = await Promise.all(
    names.map(async (name) => {
      const object = env.COUNTER.getByName(name);
      return { ...(await object.where()), value: await object.value() };
    })
  );
  return { error: null, calledAt, resolvedAt, objects };
}

export default {
  async fetch(request, env, ctx) {
    const front = isolateId();
    // A request for another isolate is refused before anything else is done with it (its body
    // is not read, an upgrade not accepted), so that the client can send it again until the
    // isolate it names serves it. The refusal is held for a while: the front gives the next
    // request to a sleeping thread with the fewest requests in flight, so the client's other
    // attempts at once go to other threads.
    //
    // The attempts of one request carry one `x-attempt`, and the isolate named acts on the
    // first it gets only: it refuses the others too.
    const wanted = request.headers.get('x-thread');
    const attempt = request.headers.get('x-attempt');
    if (
      wanted !== null &&
      (wanted !== front || (attempt !== null && attempts.has(attempt)))
    ) {
      await scheduler.wait(100);
      return Response.json(
        { front },
        { status: 409, headers: { 'x-front': front } }
      );
    }
    if (attempt !== null) attempts.add(attempt);
    const url = new URL(request.url);
    const query = url.searchParams;
    const name = query.get('name') ?? 'default';
    // A stub of the object `name`, made by the requests that use one only.
    let stub;
    const counter = () => (stub ??= env.COUNTER.getByName(name));
    switch (url.pathname) {
      case '/front':
        // Answers after `ms` milliseconds, and calls no object.
        await scheduler.wait(Number(query.get('ms') ?? 0));
        return Response.json({ front });
      case '/spin': {
        // Keeps this isolate's thread from serving anything else for a while.
        const until = Date.now() + Number(query.get('ms'));
        while (Date.now() < until) {}
        return Response.json({ front });
      }
      case '/spin-body': {
        // The head goes out first; then the body's first pull keeps this thread in JavaScript
        // for `ms` milliseconds, during which it serves nothing else. The body says when the
        // spin started and ended.
        const ms = Number(query.get('ms'));
        const body = new ReadableStream(
          {
            pull(controller) {
              const start = Date.now();
              while (Date.now() < start + ms) {}
              const end = Date.now();
              controller.enqueue(
                encoder.encode(JSON.stringify({ front, start, end }))
              );
              controller.close();
            },
          },
          { highWaterMark: 0 }
        );
        return new Response(body, { headers: { 'x-front': front } });
      }
      case '/count': {
        // The response waits for the end of the request body, which the client decides.
        await request.arrayBuffer();
        return Response.json({ front, ...(await counter().increment()) });
      }
      case '/increments': {
        // `n` increments of the object at once, each through a stub of its own.
        const results = await Promise.all(
          Array.from({ length: Number(query.get('n')) }, () =>
            env.COUNTER.getByName(name).increment()
          )
        );
        return Response.json({ front, results });
      }
      case '/calls': {
        // `n` calls of the object one after another, each with when it started and ended.
        const calls = [];
        for (let i = 0; i < Number(query.get('n')); i++) {
          const start = Date.now();
          const { isolate } = await counter().where();
          calls.push({ start, end: Date.now(), isolate });
        }
        return Response.json({ front, calls });
      }
      case '/cycle': {
        // The object `a` calls `b`, which calls `a` back.
        const a = env.COUNTER.getByName(query.get('a'));
        const b = env.COUNTER.getByName(query.get('b'));
        return Response.json({ front, ...(await a.ping(b)) });
      }
      case '/slow': {
        const ms = Number(query.get('ms'));
        return Response.json({
          front,
          ...(await counter().incrementSlowly(ms)),
        });
      }
      case '/slow-calls':
        return Response.json({ front, calls: await counter().slowCalls() });
      case '/where':
        // Answers after `ms` milliseconds, if given, so that requests sent together are in
        // flight together, each on a thread of its own.
        await scheduler.wait(Number(query.get('ms') ?? 0));
        return Response.json({ front, ...(await counter().where()) });
      case '/put':
        await counter().put(query.get('value'));
        return Response.json({ front });
      case '/value':
        return Response.json({ front, value: await counter().value() });
      case '/features':
        return Response.json({ front, ...(await features(env, ctx, name)) });
      case '/facet': {
        // `op` on the facet `facet` of the object: `bump`, `clone` (to `to`) or `delete`.
        const facet = query.get('facet');
        const op = query.get('op');
        if (op === 'bump') {
          return Response.json({
            front,
            ...(await counter().bumpFacet(facet)),
          });
        }
        if (op === 'clone') await counter().cloneFacet(facet, query.get('to'));
        else if (op === 'delete') await counter().deleteFacet(facet);
        else return new Response(`no op ${op}`, { status: 400 });
        return Response.json({ front });
      }
      case '/restore': {
        // A stub the object vends, called from this thread, then restored here from what
        // restores it: each says where it runs.
        using vended = await counter().vend();
        const keeper = ctx.exports.Keeper({ props: { stub: vended } });
        return Response.json({
          front,
          vended: await vended.add(1),
          vendedAt: await vended.where(),
          restored: await keeper.add(1),
          restoredAt: await keeper.where(),
        });
      }
      case '/alarm':
        await counter().setAlarmIn(Number(query.get('ms')));
        return Response.json({ front });
      case '/alarm-at':
        await counter().setAlarmAt(Number(query.get('at')));
        return Response.json({ front });
      case '/alarms':
        return Response.json({ front, ...(await counter().alarms()) });
      case '/hook': {
        const names = (query.get('names') ?? '').split(',').filter(Boolean);
        return Response.json({
          front,
          ...(await hookThenCall(env, query.get('hook'), counter, names)),
        });
      }
      case '/hibernate':
        // Evicts the object; its hibernatable WebSockets stay open.
        await unsafe.evict(counter(), { webSockets: 'hibernate' });
        return Response.json({ front });
      case '/received':
        return Response.json({ front, ...(await counter().received()) });
      case '/echo':
      case '/echo-digest': {
        const response = await counter().fetch(`http://object${url.pathname}`, {
          method: 'POST',
          body: request.body,
        });
        const headers = new Headers(response.headers);
        headers.set('x-front', front);
        return new Response(response.body, { headers });
      }
      case '/ws': {
        const response = await counter().fetch(request);
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
