import { WorkerEntrypoint } from 'cloudflare:workers';
export default class Worker extends WorkerEntrypoint {
  fetch() {}
  foo(emoji) {
    return this.env.USER.foo(emoji);
  }
}
