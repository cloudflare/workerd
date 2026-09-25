// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkerEntrypoint } from 'cloudflare:workers';
import { rejects } from 'assert';

// Sends back everything it receives, and ends its side once its peer has ended theirs.
export class Echo extends WorkerEntrypoint {
  async connect(socket) {
    await socket.readable.pipeTo(socket.writable);
  }
}

// Says hello, reads until its peer ends its side, then returns. Ending its own side right after
// saying hello is what winds the relay down.
//
// It reads concurrently with writing, as any client of a relay has to: the tunnels between Workers
// are unbuffered pipes, so its write completes only once the bytes have made it all the way round
// the loop and back to its own reader.
export class Greeter extends WorkerEntrypoint {
  async connect(socket) {
    const drained = (async () => {
      for await (const _ of socket.readable) {
        // Discard input
      }
    })();

    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('hello'));
    await writer.close();
    await drained;
  }
}

// Neither sends anything nor ends its side, for as long as its peer is connected.
export class Silent extends WorkerEntrypoint {
  async connect(socket) {
    for await (const _ of socket.readable) {
      // Discard input
    }
  }
}

// proxyTo() settles once both directions are done: the Greeter's hello goes to the Echo worker
// and back, and it is the Greeter ending its side that winds down both directions afterwards.
export const proxyToResolvesOnceBothDirectionsFinish = {
  async test(ctrl, env) {
    const echo = env.ECHO.connect('echo.example:1', { allowHalfOpen: true });
    const greeter = env.GREETER.connect('greeter.example:1', {
      allowHalfOpen: true,
    });

    await echo.proxyTo(greeter);
  },
};

// proxyTo() rejects when the relay fails, even though neither direction would ever finish on its
// own: nothing is ever sent between the Echo and the Silent peer, and neither ends its side.
export const proxyToRejectsWhenTheRelayFails = {
  async test(ctrl, env) {
    const echo = env.ECHO.connect('echo.example:1', { allowHalfOpen: true });
    const silent = env.SILENT.connect('silent.example:1', {
      allowHalfOpen: true,
    });
    const controller = new AbortController();

    const relay = echo.proxyTo(silent, { signal: controller.signal });
    controller.abort(new Error('relay aborted'));

    await rejects(relay, { message: 'relay aborted' });
  },
};
