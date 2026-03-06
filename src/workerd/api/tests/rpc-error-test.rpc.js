// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { RpcTarget } from 'cloudflare:workers';

class Baz extends RpcTarget {
  async qux() {
    await scheduler.wait(10);
    const err = new Error('bork');
    err.stack2 = err.stack;
    throw err;
  }
}

class Bar extends RpcTarget {
  async baz() {
    await scheduler.wait(10);
    return new Baz();
  }
}

export default {
  foo() {
    const err = new Error('boom');
    err.stack2 = err.stack;
    throw err;
  },

  async bar() {
    await scheduler.wait(10);
    return new Bar();
  },

  xyz() {
    const err = new Error('bang');
    err.stack2 = err.stack;
    return Promise.reject(err);
  },

  abc() {
    const err = new Error('boom');
    err.stack2 = err.stack;
    return { a: err };
  },
};
