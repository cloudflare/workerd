// A worker without the `durable_object_pre_shutdown` compatibility flag, for which `preShutdown`
// is an ordinary RPC method name.

import { WorkerEntrypoint } from 'cloudflare:workers';

export class PlainEntrypoint extends WorkerEntrypoint {
  async preShutdown() {
    return 'plain rpc method';
  }
}

export default {};
