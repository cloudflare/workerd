import { WorkerEntrypoint } from 'cloudflare:workers';

// Exposes RPC methods whose names collide with Python's iterator/generator
// protocol. Pyodide's JsProxy detects objects with a callable "next" property
// as iterators and installs C-level descriptors for send/next/throw/close that
// shadow JS property access via __getattr__.
export class JsService extends WorkerEntrypoint {
  async send(msg) {
    return `sent:${msg}`;
  }

  async next(val) {
    return `next:${val}`;
  }

  async throw(err) {
    return `throw:${err}`;
  }

  async close() {
    return 'closed';
  }

  async normalMethod(x) {
    return `normal:${x}`;
  }
}
