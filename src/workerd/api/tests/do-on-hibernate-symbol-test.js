// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { strictEqual } from 'node:assert';
import { DurableObject } from 'cloudflare:workers';
import { onHibernate } from 'cloudflare:durable-objects';

export class OnHibernateObject extends DurableObject {
  async [onHibernate]() {}

  async onHibernate() {
    return 'onHibernate';
  }
}

// The runtime reads the handler once, after construction. A read that throws must not fail
// construction. Records the read, so the test knows it happened.
export class ThrowingReadObject extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    return new Proxy(this, {
      get(target, prop, receiver) {
        if (prop === onHibernate) {
          target.handlerRead = true;
          throw new Error('onHibernate read failed');
        }
        return Reflect.get(target, prop, receiver);
      },
    });
  }

  async fetch() {
    return new Response(String(this.handlerRead === true));
  }
}

export const exportsSymbol = {
  test() {
    strictEqual(typeof onHibernate, 'symbol');
  },
};

export const symbolHandlerIsNotRpcMethod = {
  async test(ctrl, env) {
    const stub = env.ns.getByName('rpc');
    strictEqual(stub[onHibernate], undefined);
    // A string-named method with the same name is still an ordinary RPC method.
    strictEqual(await stub.onHibernate(), 'onHibernate');
  },
};

export const throwingHandlerReadDoesNotFailConstruction = {
  async test(ctrl, env) {
    const response = await env.throwingRead.getByName('a').fetch('http://x/');
    strictEqual(await response.text(), 'true');
  },
};
