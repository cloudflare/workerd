import { DurableObject } from 'cloudflare:workers';

// class Baz extends RpcTarget {
//   async qux() {
//     await scheduler.wait(10);
//     const err = new Error('bork');
//     err.stack2 = err.stack;
//     throw err;
//   }
// }

export class Bar extends DurableObject {
  async bar() {
    await this.blah()
  }

  async blah () {
    await scheduler.wait(10);
    const err = new Error('bork');
    err.trusted = true
    throw err;
  }
}

// export default {
//   foo() {
//     const err = new Error('boom');
//     err.stack2 = err.stack;
//     throw err;
//   },

//   async bar() {
//     await scheduler.wait(10);
//     return new Bar();
//   },

//   xyz() {
//     const err = new Error('bang');
//     err.stack2 = err.stack;
//     return Promise.reject(err);
//   },

//   abc() {
//     const err = new Error('boom');
//     err.stack2 = err.stack;
//     return { a: err };
//   },
// };
