// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import * as dgram from 'node:dgram';
import { once } from 'node:events';
import { strictEqual, deepStrictEqual, throws, rejects, ok } from 'node:assert';
import { handleAsNodeConnection } from 'cloudflare:node';

// The config declares one UDP listener on 127.0.0.1:0; its bound port is the
// one declared port a socket may bind.
const PEER = { address: '10.0.0.5', port: 4444 };

// A platform UDP flow as connect() would deliver it, driven from the test.
function fakeFlow(remoteAddress, localPort) {
  let controller;
  const sent = [];
  let onSent = null;
  const socket = {
    protocol: 'udp',
    opened: Promise.resolve({
      remoteAddress,
      localAddress: `127.0.0.1:${localPort}`,
    }),
    readable: new ReadableStream({
      start(c) {
        controller = c;
      },
    }),
    writable: new WritableStream({
      write(chunk) {
        sent.push(chunk);
        onSent?.();
        onSent = null;
      },
    }),
    async close() {},
  };
  return {
    socket,
    sent,
    push(bytes) {
      controller.enqueue(new Datagram(bytes));
    },
    end() {
      controller.close();
    },
    nextSent() {
      return new Promise((resolve) => {
        onSent = resolve;
      });
    },
  };
}

function bound(type = 'udp4', port = 0) {
  const socket = dgram.createSocket(type);
  socket.bind(port);
  return once(socket, 'listening').then(() => socket);
}

function sendAsync(socket, ...args) {
  return new Promise((resolve, reject) => {
    socket.send(...args, (err, bytes) => {
      if (err) reject(err);
      else resolve(bytes);
    });
  });
}

function emittedError(socket) {
  return once(socket, 'error').then(([err]) => err);
}

export const testConstruction = {
  test() {
    for (const type of ['udp4', 'udp6']) {
      ok(dgram.createSocket(type) instanceof dgram.Socket);
      ok(dgram.createSocket({ type }) instanceof dgram.Socket);
      ok(new dgram.Socket(type) instanceof dgram.Socket);
      strictEqual(dgram.createSocket(type).type, type);
    }
    for (const bad of [null, undefined, 123, [], 'tcp', {}, { type: 'udp' }]) {
      throws(() => dgram.createSocket(bad), { code: 'ERR_SOCKET_BAD_TYPE' });
    }
    // A message listener is attached; anything else is ignored, as in Node.
    const listener = () => {};
    strictEqual(
      dgram.createSocket('udp4', listener).listeners('message')[0],
      listener
    );
    strictEqual(
      dgram.createSocket('udp4', 'invalid').listenerCount('message'),
      0
    );
    throws(() => dgram.createSocket({ type: 'udp4', recvBufferSize: -1 }), {
      code: 'ERR_SOCKET_BAD_BUFFER_SIZE',
    });
    const sized = dgram.createSocket({
      type: 'udp4',
      recvBufferSize: 1024,
      sendBufferSize: 2048,
    });
    strictEqual(sized.getRecvBufferSize(), 1024);
    strictEqual(sized.getSendBufferSize(), 2048);
    strictEqual(sized.getSendQueueSize(), 0);
    strictEqual(sized.getSendQueueCount(), 0);
    strictEqual(sized.ref(), sized);
    strictEqual(sized.unref(), sized);

    strictEqual(typeof dgram.default, 'object');
    strictEqual(dgram.default.createSocket, dgram.createSocket);
    strictEqual(dgram.default.Socket, dgram.Socket);
  },
};

export const testDeclaredPorts = {
  async test() {
    const a = await bound();
    const declared = a.address().port;
    ok(declared > 0);
    deepStrictEqual(a.address(), {
      address: '0.0.0.0',
      family: 'IPv4',
      port: declared,
    });
    throws(() => a.bind(0), { code: 'ERR_SOCKET_ALREADY_BOUND' });

    // The one declared port is claimed; nothing else may be bound explicitly.
    const b = dgram.createSocket('udp4');
    b.bind(0);
    const inUse = await emittedError(b);
    strictEqual(inUse.code, 'EADDRINUSE');
    strictEqual(inUse.message, 'bind EADDRINUSE 0.0.0.0:0');
    const c = dgram.createSocket('udp4');
    c.bind(8090);
    const notAvail = await emittedError(c);
    strictEqual(notAvail.code, 'EADDRNOTAVAIL');
    strictEqual(notAvail.message, 'bind EADDRNOTAVAIL 0.0.0.0:8090');
    const d = dgram.createSocket('udp4');
    d.bind(declared);
    strictEqual((await emittedError(d)).code, 'EADDRINUSE');

    // close() releases the port; a later bind takes it explicitly or by 0.
    const closed = once(a, 'close');
    strictEqual(a.close(), a);
    await closed;
    throws(() => a.close(), { code: 'ERR_SOCKET_DGRAM_NOT_RUNNING' });
    throws(() => a.address(), { code: 'ERR_SOCKET_DGRAM_NOT_RUNNING' });
    const e = await bound('udp6', declared);
    deepStrictEqual(e.address(), {
      address: '::',
      family: 'IPv6',
      port: declared,
    });
    await e[Symbol.asyncDispose]();
    const f = dgram.createSocket('udp4');
    f.bind({ port: 0, address: 'localhost' }, () => {});
    await once(f, 'listening');
    deepStrictEqual(f.address(), {
      address: '127.0.0.1',
      family: 'IPv4',
      port: declared,
    });
    f.close();
    await once(f, 'close');

    // Unbound sockets have no address.
    throws(() => dgram.createSocket('udp4').address(), { code: 'EBADF' });
    throws(() => dgram.createSocket('udp4').bind('nope'), {
      code: 'ERR_SOCKET_BAD_PORT',
    });
  },
};

export const testMessageAndReply = {
  async test() {
    const socket = await bound();
    const port = socket.address().port;
    const flow = fakeFlow(`${PEER.address}:${PEER.port}`, port);
    const finished = handleAsNodeConnection(flow.socket);

    const message = once(socket, 'message');
    flow.push(new TextEncoder().encode('hello'));
    const [msg, rinfo] = await message;
    ok(Buffer.isBuffer(msg));
    strictEqual(msg.toString(), 'hello');
    deepStrictEqual(rinfo, { ...PEER, family: 'IPv4', size: 5 });

    // Replies are single datagrams whatever the input shape.
    const cases = [
      [[Buffer.from('reply'), PEER.port, PEER.address], 'reply'],
      [['string', PEER.port, PEER.address], 'string'],
      [[Buffer.from('xxabcxx'), 2, 3, PEER.port, PEER.address], 'abc'],
      [
        [
          [Buffer.from('a'), 'b', new Uint8Array([99])],
          PEER.port,
          PEER.address,
        ],
        'abc',
      ],
      [[new Uint8Array(0), PEER.port, PEER.address], ''],
    ];
    for (const [args, expected] of cases) {
      const written = flow.nextSent();
      strictEqual(await sendAsync(socket, ...args), expected.length);
      await written;
      const chunk = flow.sent.pop();
      ok(chunk instanceof Datagram);
      strictEqual(new TextDecoder().decode(chunk.data), expected);
    }
    // The sendto form and a callback-less send both deliver.
    const viaSendto = flow.nextSent();
    socket.sendto(Buffer.from('to'), 0, 2, PEER.port, PEER.address);
    await viaSendto;
    strictEqual(new TextDecoder().decode(flow.sent.pop().data), 'to');

    throws(() => socket.send('x', 0, PEER.address), {
      code: 'ERR_SOCKET_BAD_PORT',
    });
    throws(() => socket.send(42, PEER.port, PEER.address), {
      code: 'ERR_INVALID_ARG_TYPE',
    });
    throws(
      () => socket.send(Buffer.from('ab'), 3, 1, PEER.port, PEER.address),
      {
        code: 'ERR_BUFFER_OUT_OF_BOUNDS',
      }
    );

    // The flow ending finishes the inbound request; the peer is then
    // unreachable until it sends again.
    flow.end();
    await finished;
    await rejects(sendAsync(socket, 'late', PEER.port, PEER.address), {
      code: 'EHOSTUNREACH',
      message: `send EHOSTUNREACH ${PEER.address}:${PEER.port}`,
    });
    socket.close();
    await once(socket, 'close');
  },
};

export const testMappedPeerAddresses = {
  async test() {
    // A v4-mapped peer on the dual-stack listener is an IPv4 peer to a udp4
    // socket and an IPv6 peer to a udp6 socket; both can reply to either form.
    for (const [type, address, family] of [
      ['udp4', '10.0.0.6', 'IPv4'],
      ['udp6', '::ffff:10.0.0.6', 'IPv6'],
    ]) {
      const socket = await bound(type);
      const flow = fakeFlow('[::ffff:10.0.0.6]:5555', socket.address().port);
      const finished = handleAsNodeConnection(flow.socket);
      const message = once(socket, 'message');
      flow.push(new Uint8Array([1]));
      const [, rinfo] = await message;
      deepStrictEqual(rinfo, { address, family, port: 5555, size: 1 });
      for (const to of ['10.0.0.6', '::ffff:10.0.0.6', '::FFFF:10.0.0.6']) {
        const written = flow.nextSent();
        await sendAsync(socket, 'r', 5555, to);
        await written;
      }
      strictEqual(flow.sent.length, 3);
      flow.end();
      await finished;
      socket.close();
      await once(socket, 'close');
    }
  },
};

export const testConnectedMode = {
  async test() {
    const socket = await bound();
    const flow = fakeFlow(
      `${PEER.address}:${PEER.port}`,
      socket.address().port
    );
    const finished = handleAsNodeConnection(flow.socket);
    const message = once(socket, 'message');
    flow.push(new Uint8Array([1]));
    await message;

    throws(() => socket.remoteAddress(), {
      code: 'ERR_SOCKET_DGRAM_NOT_CONNECTED',
    });
    throws(() => socket.disconnect(), {
      code: 'ERR_SOCKET_DGRAM_NOT_CONNECTED',
    });
    const connected = once(socket, 'connect');
    socket.connect(PEER.port, PEER.address, () => {});
    await connected;
    deepStrictEqual(socket.remoteAddress(), { ...PEER, family: 'IPv4' });
    throws(() => socket.connect(PEER.port, PEER.address), {
      code: 'ERR_SOCKET_DGRAM_IS_CONNECTED',
    });
    throws(() => socket.send('x', 0, 1, PEER.port, PEER.address), {
      code: 'ERR_SOCKET_DGRAM_IS_CONNECTED',
    });

    const written = flow.nextSent();
    strictEqual(await sendAsync(socket, 'connected'), 9);
    await written;
    strictEqual(new TextDecoder().decode(flow.sent.pop().data), 'connected');
    const sliced = flow.nextSent();
    strictEqual(await sendAsync(socket, Buffer.from('xxyy'), 2, 2), 2);
    await sliced;
    strictEqual(new TextDecoder().decode(flow.sent.pop().data), 'yy');

    socket.disconnect();
    throws(() => socket.remoteAddress(), {
      code: 'ERR_SOCKET_DGRAM_NOT_CONNECTED',
    });
    flow.end();
    await finished;
    socket.close();
    await once(socket, 'close');
  },
};

export const testImplicitBind = {
  async test() {
    // send() and connect() on an unbound socket bind an ephemeral port rather
    // than a declared listener port, and the destination is unreachable.
    const declared = await bound();
    const declaredPort = declared.address().port;
    declared.close();
    await once(declared, 'close');

    const s = dgram.createSocket('udp4');
    await rejects(sendAsync(s, 'x', 53, '1.1.1.1'), { code: 'EHOSTUNREACH' });
    ok(s.address().port >= 49152);
    // The declared port is still free.
    const again = await bound('udp4', declaredPort);
    again.close();
    s.close();

    const c = dgram.createSocket('udp4');
    c.connect(53, '1.1.1.1');
    ok(c.address().port >= 49152);
    await rejects(sendAsync(c, 'x'), { code: 'EHOSTUNREACH' });
    c.close();
    // A callback-less send to an unreachable peer is dropped, as a UDP send
    // failure is in Node.
    const q = dgram.createSocket('udp4');
    q.send('x', 53, '1.1.1.1');
    q.close();
  },
};

export const testCloseEndsFlows = {
  async test() {
    const socket = await bound();
    const port = socket.address().port;
    const flows = [fakeFlow('10.0.0.7:1', port), fakeFlow('10.0.0.7:2', port)];
    const finished = flows.map((f) => handleAsNodeConnection(f.socket));
    const messages = once(socket, 'message');
    flows[0].push(new Uint8Array([1]));
    await messages;
    socket.close();
    await Promise.all(finished);

    // Nothing is bound to the port any more, so a new flow is rejected.
    await rejects(handleAsNodeConnection(fakeFlow('10.0.0.7:3', port).socket), {
      code: 'ERR_INVALID_ARG_VALUE',
    });
  },
};

export const testSocketOptions = {
  async test() {
    const controller = new AbortController();
    const aborted = dgram.createSocket({
      type: 'udp4',
      signal: controller.signal,
    });
    const closed = once(aborted, 'close');
    controller.abort();
    await closed;
    throws(() => aborted.close(), { code: 'ERR_SOCKET_DGRAM_NOT_RUNNING' });

    const s = dgram.createSocket('udp4');
    strictEqual(s.setTTL(64), 64);
    throws(() => s.setTTL(0), { code: 'ERR_OUT_OF_RANGE' });
    throws(() => s.setTTL('64'), { code: 'ERR_INVALID_ARG_TYPE' });
    strictEqual(s.setMulticastTTL(1), 1);
    strictEqual(s.setMulticastLoopback(true), true);
    s.setBroadcast(true);
    s.setMulticastInterface('127.0.0.1');
    throws(() => s.setMulticastInterface(1), { code: 'ERR_INVALID_ARG_TYPE' });
    throws(() => s.addMembership(), { code: 'ERR_MISSING_ARGS' });
    for (const call of [
      () => s.addMembership('224.0.0.1'),
      () => s.dropMembership('224.0.0.1', '127.0.0.1'),
      () => s.addSourceSpecificMembership('10.0.0.1', '224.0.0.1'),
      () => s.dropSourceSpecificMembership('10.0.0.1', '224.0.0.1'),
    ]) {
      throws(call, { code: 'ERR_FEATURE_UNAVAILABLE_ON_PLATFORM' });
    }
    throws(() => s.setRecvBufferSize('big'), {
      code: 'ERR_SOCKET_BAD_BUFFER_SIZE',
    });
    s.setSendBufferSize(4096);
    strictEqual(s.getSendBufferSize(), 4096);
    s.close();
    await once(s, 'close');
    throws(() => s.getRecvBufferSize(), {
      code: 'ERR_SOCKET_DGRAM_NOT_RUNNING',
    });
  },
};
