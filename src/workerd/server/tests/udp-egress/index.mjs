import { connect } from 'cloudflare:sockets';

function check(value, message) {
  if (!value) throw new Error(message);
}

async function send(request) {
  const url = new URL(request.url);
  const address = {
    hostname: url.searchParams.get('host') ?? '127.0.0.1',
    port: Number(url.searchParams.get('port')),
  };
  const socket = connect(address, { protocol: 'udp' });
  check(socket.protocol === 'udp', 'incorrect protocol');
  let byobRejected = false;
  try {
    socket.readable.getReader({ mode: 'byob' });
  } catch (e) {
    byobRejected = e instanceof TypeError;
  }
  check(byobRejected, 'UDP readable must be a value stream');
  const writer = socket.writable.getWriter();
  let lastWrite;
  try {
    if (url.pathname === '/denied') {
      try {
        await socket.opened;
      } catch {
        return new Response('denied');
      }
      return new Response('opened');
    }
    if (url.pathname === '/echo') {
      const bytes = new Uint8Array([0, 32, 255]);
      await writer.write(bytes);
      const { value, done } = await socket.readable.getReader().read();
      check(!done && value instanceof Uint8Array, 'expected Uint8Array packet');
      check(
        value.length === bytes.length && value.every((v, i) => v === bytes[i]),
        'bad reply'
      );
    } else {
      // Intentionally write before awaiting opened, and never read or wait for a reply.
      await writer.write(new Uint8Array());
      await writer.write(new Uint8Array([0, 32, 255]));
      const backing = new Uint8Array([99, 1, 2, 3, 99]);
      await writer.write(backing.subarray(1, 4));
      // close() must drain this queued packet even before its write promise is awaited.
      lastWrite = writer.write(
        new Uint8Array(Number(url.searchParams.get('size') ?? 9000)).fill(42)
      );
    }
  } finally {
    writer.releaseLock();
    await socket.close();
    await lastWrite;
  }
  return new Response('sent');
}

export class Sender {
  constructor(state) {
    this.state = state;
  }
  async fetch(request) {
    // The outgoing open must wait for this storage operation's output gate.
    const stored = this.state.storage.put('sent', true);
    const sent = send(request);
    await stored;
    return sent;
  }
}

export default {
  async fetch(request, env) {
    if (new URL(request.url).pathname === '/object') {
      return env.OBJECT.get(env.OBJECT.idFromName('sender')).fetch(request);
    }
    return send(request);
  },
};
