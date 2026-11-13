// GET / answers "hello world" from any pod. GET /counter/<name> adds one to the Durable Object
// counter named <name>, and reports which pod took the request and which pod runs the object.

import { DurableObject } from 'cloudflare:workers';

// A random ID for this pod's workerd process, picked at its first request.
let podId;
function thisPod() {
  podId ??= crypto.randomUUID().slice(0, 8);
  return podId;
}

export class Counter extends DurableObject {
  async fetch() {
    const count = ((await this.ctx.storage.get('count')) ?? 0) + 1;
    await this.ctx.storage.put('count', count);
    return Response.json({ count, objectPod: thisPod() });
  }
}

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);
    if (pathname === '/') {
      return new Response('hello world\n');
    }
    const name = pathname.match(/^\/counter\/([^/]+)$/)?.[1];
    if (name === undefined) {
      return new Response('not found\n', { status: 404 });
    }
    // fetch() rather than a JS RPC call: a request that reaches a pod while it hands the object
    // to another pod is sent on to the object's new pod.
    const stub = env.COUNTER.get(env.COUNTER.idFromName(name));
    const { count, objectPod } = await (await stub.fetch(request)).json();
    return Response.json({ name, count, requestPod: thisPod(), objectPod });
  },
};
