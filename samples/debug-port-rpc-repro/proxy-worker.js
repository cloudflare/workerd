import { WorkerEntrypoint } from 'cloudflare:workers';

// Test 1: Direct method (known to work)
export class DirectProxy extends WorkerEntrypoint {
  async fetch(request) {
    const client = await this.env.DEBUG_PORT.connect('127.0.0.1:9230');
    const fetcher = await client.getEntrypoint('target');
    const resp = await fetcher.fetch(request);
    const body = await resp.arrayBuffer();
    return new Response(body, resp);
  }

  async ping() {
    const client = await this.env.DEBUG_PORT.connect('127.0.0.1:9230');
    const fetcher = await client.getEntrypoint('target');
    return await fetcher.ping();
  }
}

// Test 2: Proxy constructor pattern (same as miniflare's ExternalServiceProxy)
export class ProxyProxy extends WorkerEntrypoint {
  constructor(ctx, env) {
    super(ctx, env);
    return new Proxy(this, {
      get(target, prop) {
        if (Reflect.has(target, prop)) {
          return Reflect.get(target, prop);
        }
        if (typeof prop === 'string') {
          const methodName = prop;
          return async function (...args) {
            const client =
              await target.env.DEBUG_PORT.connect('127.0.0.1:9230');
            const fetcher = await client.getEntrypoint('target');
            const method = fetcher[methodName];
            if (typeof method !== 'function') {
              throw new Error(`Method "${methodName}" not found`);
            }
            return method.apply(fetcher, args);
          };
        }
        return undefined;
      },
    });
  }

  async fetch(request) {
    const client = await this.env.DEBUG_PORT.connect('127.0.0.1:9230');
    const fetcher = await client.getEntrypoint('target');
    const resp = await fetcher.fetch(request);
    const body = await resp.arrayBuffer();
    return new Response(body, resp);
  }
}

// Test 3: Proxy constructor pattern with props (closest to miniflare)
export class ProxyWithProps extends WorkerEntrypoint {
  constructor(ctx, env) {
    super(ctx, env);
    this._props = ctx.props;
    return new Proxy(this, {
      get(target, prop) {
        if (Reflect.has(target, prop)) {
          return Reflect.get(target, prop);
        }
        if (typeof prop === 'string') {
          const methodName = prop;
          return async function (...args) {
            const client =
              await target.env.DEBUG_PORT.connect('127.0.0.1:9230');
            const fetcher = await client.getEntrypoint('target');
            const method = fetcher[methodName];
            if (typeof method !== 'function') {
              throw new Error(`Method "${methodName}" not found`);
            }
            return method.apply(fetcher, args);
          };
        }
        return undefined;
      },
    });
  }

  async fetch(request) {
    const client = await this.env.DEBUG_PORT.connect('127.0.0.1:9230');
    const fetcher = await client.getEntrypoint('target');
    const resp = await fetcher.fetch(request);
    const body = await resp.arrayBuffer();
    return new Response(body, resp);
  }
}

export default {
  async fetch() {
    return new Response('proxy default export');
  },
};
