// A counter per name: GET /<name> adds one to the Durable Object named <name> and reports which
// pod served the request and which pod runs the object.

import { DurableObject } from 'cloudflare:workers';

export class Counter extends DurableObject {
  async fetch() {
    const count = ((await this.ctx.storage.get('count')) ?? 0) + 1;
    await this.ctx.storage.put('count', count);
    return Response.json({ count, objectPod: this.env.POD_NAME });
  }
}

export default {
  async fetch(request, env) {
    const name = new URL(request.url).pathname.slice(1) || 'default';
    // fetch() rather than a JS RPC call: a request that reaches a pod while it hands the object
    // to another pod is sent on to the object's new pod.
    const stub = env.COUNTER.get(env.COUNTER.idFromName(name));
    const response = await stub.fetch(request);
    const { count, objectPod } = await response.json();
    return Response.json({ name, count, objectPod, requestPod: env.POD_NAME });
  },
};
