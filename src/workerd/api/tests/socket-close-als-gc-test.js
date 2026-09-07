// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Regression test for the AsyncLocalStorage frame leak reported in issue #7202.
//
// Socket::close() chains several jsg::Promise continuations that capture the Socket via
// JSG_THIS. While those captures were plain (untraced) jsg::Refs they acted as GC roots, so a
// close() whose flush was still pending at request teardown pinned the Socket, its promises and
// every AsyncContextFrame reachable from them for the lifetime of the isolate. The `passive`
// mode (never calling close()) is the control: pending promises alone are ordinary garbage once
// the request context is gone. The `rpcClose` mode repeats the pending close() on a socket that
// was transferred over a real (loopback capnp) RPC boundary and re-hydrated in this worker.
import { AsyncLocalStorage } from 'node:async_hooks';
import { connect } from 'cloudflare:sockets';
import { WorkerEntrypoint } from 'cloudflare:workers';
import { ok, strictEqual } from 'node:assert';

const storage = new AsyncLocalStorage();
// mode -> WeakRefs to each request's ALS store.
const refs = { close: [], passive: [], rpcClose: [] };
// mode -> requests whose close() had already settled before the request returned. The
// pending-close modes only exercise the leak path while this stays 0; it is checked instead of
// asserting anything about whether the promises settle after teardown.
const closedBeforeReturn = { close: 0, passive: 0, rpcClose: 0 };

const PEER = { hostname: 'local.invalid', port: 5432 };

async function openSocket(mode, env) {
  if (mode === 'rpcClose') {
    return await env.PRODUCER.getSocket();
  }
  return connect(PEER);
}

export default {
  async fetch(request, env) {
    const mode = new URL(request.url).pathname.slice(1);
    if (mode === 'collect') {
      // Separate jobs allow WeakRef targets and conservative stack roots to clear.
      for (let i = 0; i < 6; i++) {
        await scheduler.wait(0);
        gc();
      }
      const retained = {};
      for (const [m, list] of Object.entries(refs)) {
        retained[m] = list.filter((r) => r.deref() !== undefined).length;
      }
      return Response.json({ retained, closedBeforeReturn });
    }
    await storage.run({ bytes: new Uint8Array(100_000) }, async () => {
      refs[mode].push(new WeakRef(storage.getStore()));
      const socket = await openSocket(mode, env);
      // Reactions registered inside storage.run() capture the current frame, as in the report.
      socket.closed.then(
        () => {},
        () => {}
      );
      socket.readable
        .getReader()
        .read()
        .catch(() => {});
      await socket.opened;
      if (mode !== 'passive') {
        // The peer never reads, so the flush started by close() cannot complete before the
        // request returns. The RPC path buffers more, hence the larger write.
        const writer = socket.writable.getWriter();
        writer
          .write(new Uint8Array(mode === 'rpcClose' ? 1 << 20 : 65_536))
          .catch(() => {});
        writer.releaseLock();
        let settled = false;
        socket.close().then(
          () => {
            settled = true;
          },
          () => {
            settled = true;
          }
        );
        await scheduler.wait(0);
        if (settled) closedBeforeReturn[mode]++;
      }
    });
    return new Response('done');
  },
};

// Target of globalOutbound: accepts every connection and never reads from it.
export const peer = {
  async connect() {
    await scheduler.wait(10_000);
  },
};

// Returns an opened socket over the loopback capnp RPC boundary, transferring it to the caller.
export class Producer extends WorkerEntrypoint {
  async getSocket() {
    const socket = connect(PEER);
    // Socket::serialize() requires an established connection.
    await socket.opened;
    return socket;
  }
}

async function checkCollected(env, mode) {
  for (let batch = 0; batch < 2; batch++) {
    for (let i = 0; i < 12; i++) {
      const response = await env.SUBJECT.fetch('http://local.invalid/' + mode);
      strictEqual(await response.text(), 'done');
      await scheduler.wait(0);
    }
    const response = await env.SUBJECT.fetch('http://local.invalid/collect');
    const { retained, closedBeforeReturn } = await response.json();
    // A conservative root can retain the newest allocation for an extra GC.
    ok(
      retained[mode] <= 2,
      `${mode}: retained ${retained[mode]} request stores`
    );
    // Fixture guard: close() must still have been pending when each request returned.
    strictEqual(
      closedBeforeReturn[mode],
      0,
      `${mode}: close() settled before the request returned`
    );
  }
}

export const pendingCloseCollects = {
  async test(ctrl, env) {
    await checkCollected(env, 'close');
  },
};

export const pendingWithoutCloseCollects = {
  async test(ctrl, env) {
    await checkCollected(env, 'passive');
  },
};

export const rpcTransferredPendingCloseCollects = {
  async test(ctrl, env) {
    await checkCollected(env, 'rpcClose');
  },
};
