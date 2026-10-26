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
