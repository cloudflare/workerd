// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkerEntrypoint } from 'cloudflare:workers';
import { rejects, strictEqual, throws } from 'assert';

async function readAll(readable) {
  const dec = new TextDecoder();
  let result = '';
  for await (const chunk of readable) {
    result += dec.decode(chunk, { stream: true });
  }
  return result + dec.decode();
}

// Sends back everything it receives, and ends its side once its peer has ended theirs.
export class Echo extends WorkerEntrypoint {
  async connect(socket) {
    await socket.readable.pipeTo(socket.writable);
  }
}

// Says hello and ends its side, then checks that the hello comes back before its peer ends
// theirs.
//
// It reads concurrently with writing, as any client of a relay has to: the tunnels between Workers
// are unbuffered pipes, so its write completes only once the bytes have made it all the way round
// the loop and back to its own reader.
export class Greeter extends WorkerEntrypoint {
  async connect(socket) {
    const received = readAll(socket.readable);
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('hello'));
    await writer.close();
    strictEqual(await received, 'hello');
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

// Relays its socket to an Echo and returns before a single byte has crossed the relay: the
// handler is synchronous, and nothing is relayed until it has returned.
export class Relay extends WorkerEntrypoint {
  connect(socket) {
    socket.proxyTo(this.env.ECHO.connect('echo.example:1'));
  }
}

// Relays its socket to a Silent peer, which would never end the relay, and aborts the relay once
// the handler has returned.
export class AbortedRelay extends WorkerEntrypoint {
  connect(socket) {
    const controller = new AbortController();
    socket.proxyTo(this.env.SILENT.connect('silent.example:1'), {
      signal: controller.signal,
    });
    queueMicrotask(() => controller.abort(new Error('relay aborted')));
  }
}

// proxyTo() returns nothing; the relay settles both sockets' `closed` once both directions are
// done. The Greeter's hello goes to the Echo worker and back after the Greeter has ended its side,
// so each direction must end on its own, even though neither socket allows half-open connections.
export const proxyToSettlesClosedOnceBothDirectionsFinish = {
  async test(ctrl, env) {
    const echo = env.ECHO.connect('echo.example:1');
    const greeter = env.GREETER.connect('greeter.example:1');

    strictEqual(echo.proxyTo(greeter), undefined);
    // The relay holds both sockets' streams.
    throws(() => echo.readable.getReader(), { name: 'TypeError' });
    throws(() => greeter.writable.getWriter(), { name: 'TypeError' });

    await Promise.all([echo.closed, greeter.closed]);
  },
};

// A connect() handler's connection stays open until its relay ends, however soon the handler
// returns. Two round trips cross the relay after the Relay handler has returned. Each write
// completes only once its bytes are read back, as for the Greeter, so the reads run alongside it.
export const proxyToOutlivesTheConnectHandler = {
  async test(ctrl, env) {
    const socket = env.RELAY.connect('relay.example:1');
    const reader = socket.readable.getReader();
    const writer = socket.writable.getWriter();
    const enc = new TextEncoder();
    const dec = new TextDecoder();

    for (const message of ['ping', 'pong']) {
      const written = writer.write(enc.encode(message));
      let received = '';
      while (received.length < message.length) {
        const { value, done } = await reader.read();
        strictEqual(done, false);
        received += dec.decode(value, { stream: true });
      }
      strictEqual(received, message);
      await written;
    }

    await writer.close();
    strictEqual((await reader.read()).done, true);
    await socket.closed;
  },
};

// Aborting the signal fails the relay, even though neither direction would ever finish on its
// own: nothing is ever sent between the Echo and the Silent peer, and neither ends its side.
export const proxyToFailsWhenAborted = {
  async test(ctrl, env) {
    const echo = env.ECHO.connect('echo.example:1');
    const silent = env.SILENT.connect('silent.example:1');
    const controller = new AbortController();

    echo.proxyTo(silent, { signal: controller.signal });
    controller.abort(new Error('relay aborted'));

    await rejects(echo.closed, { message: 'relay aborted' });
    await rejects(silent.closed, { message: 'relay aborted' });
  },
};

// The relay keeps a connect() handler's connection open after the handler has returned (see
// proxyToOutlivesTheConnectHandler), and a relay that fails then lets the connection go, even
// though the far side of the relay would never end it. The failure fails the handler's request, and
// a peer over a service binding sees a failed connect() handler as the end of the stream.
export const proxyToFailureEndsTheConnectHandlersConnection = {
  async test(ctrl, env) {
    const socket = env.ABORTED_RELAY.connect('relay.example:1');
    await socket.opened;
    strictEqual(await readAll(socket.readable), '');
    await socket.closed;
  },
};

// proxyTo() refuses, before it has taken either socket, any socket it can't relay.
export const proxyToRefusesSocketsItCannotRelay = {
  async test(ctrl, env) {
    const echo = env.ECHO.connect('echo.example:1');
    const greeter = env.GREETER.connect('greeter.example:1');

    throws(() => echo.proxyTo(echo), {
      name: 'TypeError',
      message: 'A socket cannot be proxied to itself.',
    });

    const reader = greeter.readable.getReader();
    throws(() => echo.proxyTo(greeter), {
      name: 'TypeError',
      message:
        'proxyTo() cannot relay a socket whose readable or writable is locked.',
    });
    reader.releaseLock();

    const closed = env.SILENT.connect('silent.example:1');
    await closed.close();
    throws(() => echo.proxyTo(closed), {
      name: 'TypeError',
      message: 'proxyTo() cannot relay a socket that is closed or closing.',
    });

    // None of the refusals took anything from `echo` or `greeter`.
    echo.proxyTo(greeter);
    throws(() => echo.proxyTo(greeter), {
      name: 'TypeError',
      message: 'The socket is already being relayed by proxyTo().',
    });
    await Promise.all([echo.closed, greeter.closed]);
  },
};

// A relayed socket's connection belongs to the relay, so it can't be upgraded under it.
export const proxyToRefusesStartTlsOnARelayedSocket = {
  async test(ctrl, env) {
    const echo = env.ECHO.connect('echo.example:1', {
      secureTransport: 'starttls',
    });
    const greeter = env.GREETER.connect('greeter.example:1');

    echo.proxyTo(greeter);
    throws(() => echo.startTls(), {
      name: 'TypeError',
      message: 'startTls() cannot be called on a socket relayed by proxyTo().',
    });
    await Promise.all([echo.closed, greeter.closed]);
  },
};
