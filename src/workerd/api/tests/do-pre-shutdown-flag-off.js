// A worker without the `durable_object_pre_shutdown` compatibility flag. Its preShutdown()
// method must never be invoked by the runtime.

import { DurableObject } from 'cloudflare:workers';

export class FlagOffObject extends DurableObject {
  async ping() {
    return 'pong';
  }

  async getHookRan() {
    return (await this.ctx.storage.get('hookRan')) ?? false;
  }

  async preShutdown() {
    await this.ctx.storage.put('hookRan', true);
  }
}

export default {};
