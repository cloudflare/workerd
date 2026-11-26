// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Subrequests through the worker's I/O channel factory, each to a target that does as little as
// it can (target.js), so that the cost of reaching the target is most of what is measured:
//
// - a service binding's fetch() (`startSubrequest`) and an RPC call on one (a subrequest
//   channel, then a request on it);
// - an entrypoint of the worker's own `ctx.exports` with props (a channel bound to its props);
// - a dynamic worker's fetch() through a service binding in its `env`, and to compare it with, a
//   dynamic worker's fetch() that sends no subrequest;
// - a dynamic worker's fetch() through a stub restored with `ctx.restore()` (restorer.js) in its
//   `env`: a C++ channel (the channel token handler's restored channel) rather than one of the
//   server's Rust channels;
// - as controls: a Durable Object stub's fetch(), whose stub resolves the actor's channel through
//   `getGlobalActor` once, on its first fetch, so that no timed iteration consults the channel
//   table; and Date.now() (`getTimer`), which costs far less than a subrequest, so that it shows
//   a change in the factory's own overhead but not the noise the subrequests see.

import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';

export class Counter extends DurableObject {
  fetch() {
    return new Response(null);
  }
}

export class Entry extends WorkerEntrypoint {
  fetch() {
    return new Response(String(this.ctx.props.n));
  }
}

// A dynamic worker that answers itself, or forwards to the `TARGET` service binding in its `env`.
function dynamicWorker(forward) {
  return {
    compatibilityDate: '2025-01-01',
    mainModule: 'worker.js',
    modules: {
      'worker.js': `
        export default {
          fetch(req, env) {
            return ${forward ? 'env.TARGET.fetch(req)' : 'new Response(null)'};
          },
        };
      `,
    },
    env: forward ? { TARGET: forward } : {},
    globalOutbound: null,
  };
}

async function consume(response) {
  await response.arrayBuffer();
  return response.status;
}

export default {
  async bench(b, env, ctx) {
    b.run('service binding fetch', async () => {
      b.blackBox(await consume(await env.TARGET.fetch('http://target/')));
    });

    b.run('service binding rpc', async () => {
      b.blackBox(await env.TARGET_RPC.ping());
    });

    b.run('ctx.exports fetch with props', async () => {
      const entry = ctx.exports.Entry({ props: { n: 1 } });
      b.blackBox(await consume(await entry.fetch('http://entry/')));
    });

    const direct = env.LOADER.get('direct', () => dynamicWorker(null));
    b.run('dynamic worker fetch', async () => {
      b.blackBox(
        await consume(await direct.getEntrypoint().fetch('http://dynamic/'))
      );
    });

    const forwarding = env.LOADER.get('forwarding', () =>
      dynamicWorker(env.TARGET)
    );
    b.run('dynamic worker env fetch', async () => {
      b.blackBox(
        await consume(await forwarding.getEntrypoint().fetch('http://dynamic/'))
      );
    });

    const restored = await env.RESTORER.stub();
    const restoredForwarding = env.LOADER.get('restored-forwarding', () =>
      dynamicWorker(restored)
    );
    b.run('dynamic worker restored env fetch', async () => {
      b.blackBox(
        await consume(
          await restoredForwarding.getEntrypoint().fetch('http://dynamic/')
        )
      );
    });

    const counter = env.COUNTERS.get(env.COUNTERS.idFromName('bench'));
    b.run('durable object fetch (control)', async () => {
      b.blackBox(await consume(await counter.fetch('http://counter/')));
    });

    b.run('Date.now() (control)', () => {
      b.blackBox(Date.now());
    });
  },
};
