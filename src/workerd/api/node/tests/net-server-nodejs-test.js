// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { ok, strictEqual, notStrictEqual, throws, rejects } from 'node:assert';
import { once } from 'node:events';
import * as net from 'node:net';
import * as http from 'node:http';
import { connect } from 'cloudflare:sockets';
import { DurableObject } from 'cloudflare:workers';
import {
  connectHandler,
  handleAsNodeConnection,
  httpServerHandler,
} from 'cloudflare:node';

// Inbound connections on this worker's two declared TCP listeners are routed to
// whichever net.Server listens on the port they arrived on.
export default connectHandler();

async function readAll(socket) {
  const decoder = new TextDecoder();
  let out = '';
  for await (const chunk of socket.readable)
    out += decoder.decode(chunk, { stream: true });
  return out + decoder.decode();
}

async function listenError(server, ...args) {
  const errored = once(server, 'error');
  server.listen(...args);
  const [err] = await errored;
  return err;
}

// The isolate's port table is seeded with the two listener ports from the
// config: listen(0) claims them in order, further servers must use one of them.
export const testDeclaredPorts = {
  async test() {
    const a = net.createServer().listen(0);
    const b = net.createServer().listen(0);
    const portA = a.address().port;
    const portB = b.address().port;
    ok(portA > 0 && portB > 0);
    notStrictEqual(portA, portB);
    strictEqual(a.address().address, '0.0.0.0');

    // Both declared ports are claimed.
    strictEqual((await listenError(net.createServer(), 0)).code, 'EADDRINUSE');
    // Only declared ports may be listened on at the entrypoint.
    const err = await listenError(net.createServer(), 8090);
    strictEqual(err.code, 'EADDRNOTAVAIL');
    strictEqual(err.message, 'bind EADDRNOTAVAIL 0.0.0.0:8090');
    strictEqual(
      (await listenError(net.createServer(), portA)).code,
      'EADDRINUSE'
    );

    // close() returns the declared port; listen(declared) takes it explicitly.
    a.close();
    const c = net.createServer().listen(portA);
    strictEqual(c.address().port, portA);
    c.close();

    // Binding is role-neutral: a BoundSocket with port 0 takes an ephemeral
    // port, never a listener port, so egress binders cannot starve servers.
    const egress = [
      new net.BoundSocket(),
      new net.BoundSocket(),
      new net.BoundSocket(),
    ];
    for (const e of egress) ok(e.address().port >= 49152);
    const e1 = net.createServer().listen(0);
    strictEqual(e1.address().port, portA);
    e1.close();
    for (const e of egress) e.close();

    // Adopting a port-0 BoundSocket as a server re-homes it onto a declared
    // port, so Node's new BoundSocket() + listen(bound) idiom lands where
    // connections arrive.
    const bound = new net.BoundSocket();
    const ephemeralPort = bound.address().port;
    ok(ephemeralPort >= 49152);
    const d = net.createServer().listen(bound);
    strictEqual(d.address().port, portA);
    // The ephemeral port was released by the re-home.
    new net.BoundSocket({ port: ephemeralPort }).close();
    // With every declared port claimed, adoption fails with EADDRINUSE.
    strictEqual(
      (await listenError(net.createServer(), new net.BoundSocket())).code,
      'EADDRINUSE'
    );
    d.close();
    // A BoundSocket on an undeclared port can be adopted by a client but not a
    // server; the failed adoption releases it.
    const stray = new net.BoundSocket({ port: 8091 });
    strictEqual(
      (await listenError(net.createServer(), stray)).code,
      'EADDRNOTAVAIL'
    );
    throws(() => stray.address(), { code: 'ERR_SOCKET_HANDLE_ADOPTED' });
    new net.BoundSocket({ port: 8091 }).close();

    // http servers are reached through httpServerHandler, not a listener, so
    // they are unconstrained and listen(0) never takes a declared port.
    const h1 = http.createServer().listen(8080);
    strictEqual(h1.address().port, 8080);
    const h2 = http.createServer().listen(0);
    ok(h2.address().port >= 49152);
    h1.close();
    h2.close();

    // Client-side reservations are egress and never constrained.
    new net.BoundSocket({ host: '127.0.0.1', port: 8092 }).close();

    b.close();
  },
};

// A real inbound connection on a declared listener reaches the net.Server that
// listen(0)'d on it, with the socket's local port being that port.
export const testInboundOnDeclaredPort = {
  async test() {
    const server = net.createServer((socket) => {
      socket.on('data', (chunk) =>
        socket.end(
          `${socket.localAddress}:${socket.localPort}:${String(chunk).toUpperCase()}`
        )
      );
    });
    server.listen(0);
    await once(server, 'listening');
    const { port } = server.address();

    const socket = connect(`127.0.0.1:${port}`);
    const writer = socket.writable.getWriter();
    await writer.write(new TextEncoder().encode('ping'));
    // The wildcard bind resolved to the isolate's host address, not the
    // listener's 127.0.0.1 authority.
    strictEqual(await readAll(socket), `240.1.0.1:${port}:PING`);
    await socket.closed;
    server.close();
  },
};

// The isolate is the port namespace: Durable Object instances in it share the
// table with each other and with the entrypoint, as processes on one host do.
export class PortHost extends DurableObject {
  #server = null;
  #bound = null;
  #http = null;

  // Listens on port (0 claims a declared port); returns the port, or the
  // error code.
  async listen(port) {
    const server = net.createServer((s) =>
      s.end(`${this.ctx.id.toString()} ${s.localAddress} ${s.remoteAddress}`)
    );
    const outcome = await new Promise((resolve) => {
      server.once('error', (err) => resolve(err.code));
      server.once('listening', () => resolve(server.address().port));
      server.listen(port);
    });
    if (typeof outcome === 'number') this.#server = server;
    return outcome;
  }

  closeServer() {
    this.#server.close();
    this.#server = null;
  }

  async connect(socket) {
    try {
      await handleAsNodeConnection(socket);
    } catch (err) {
      const writer = socket.writable.getWriter();
      await writer.write(new TextEncoder().encode(err.message));
      await writer.close();
    }
  }

  reserve(port) {
    this.#bound = new net.BoundSocket({ port });
    return this.#bound.address().port;
  }

  release() {
    this.#bound.close();
    this.#bound = null;
  }

  // Whether port is free in the table as seen from this instance.
  probe(port) {
    try {
      new net.BoundSocket({ port }).close();
      return true;
    } catch {
      return false;
    }
  }

  listenHttp(port) {
    this.#http = http
      .createServer((req, res) => res.end(`http ${this.ctx.id.toString()}`))
      .listen(port);
    return this.#http.address().port;
  }

  closeHttp() {
    this.#http.close();
    this.#http = null;
  }

  async fetchHttp(port) {
    const res = await httpServerHandler({ port }).fetch(
      new Request('http://example.com/')
    );
    return res.text();
  }
}

export const testDurableObjectsShareIsolatePorts = {
  async test(ctrl, env) {
    const a = env.HOSTS.get(env.HOSTS.idFromName('a'));
    const b = env.HOSTS.get(env.HOSTS.idFromName('b'));
    const aId = env.HOSTS.idFromName('a').toString();

    // A server listened on in one instance holds the port for the isolate and
    // is what a connection to any instance on that port reaches.
    const port = await a.listen(0);
    ok(port > 0);
    strictEqual(await b.listen(port), 'EADDRINUSE');
    throws(() => new net.BoundSocket({ port }), { code: 'EADDRINUSE' });
    strictEqual(
      (await listenError(net.createServer(), port)).code,
      'EADDRINUSE'
    );
    const [id, host, peer] = (await readAll(a.connect(`world:${port}`))).split(
      ' '
    );
    strictEqual(id, aId);
    // One host address for the isolate; the stub caller is behind the gateway.
    strictEqual(host, '240.1.0.1');
    strictEqual(peer, '240.1.255.254');
    strictEqual((await readAll(b.connect(`world:${port}`))).split(' ')[0], aId);
    ok(
      (await readAll(a.connect('world:1'))).startsWith(
        'No net.Server is listening on port 1.'
      )
    );
    await a.closeServer();
    new net.BoundSocket({ port }).close();

    // Reservations are likewise shared, and released across requests.
    strictEqual(await a.reserve(9000), 9000);
    strictEqual(await b.probe(9000), false);
    throws(() => new net.BoundSocket({ port: 9000 }), { code: 'EADDRINUSE' });
    await a.release();
    strictEqual(await b.probe(9000), true);
    const held = new net.BoundSocket({ port: 9001 });
    strictEqual(await a.probe(9001), false);
    held.close();
    strictEqual(await a.probe(9001), true);

    // An http server listened on inside an instance is served through
    // httpServerHandler from the entrypoint and from another instance.
    strictEqual(await a.listenHttp(8085), 8085);
    const res = await httpServerHandler({ port: 8085 }).fetch(
      new Request('http://example.com/')
    );
    strictEqual(await res.text(), `http ${aId}`);
    strictEqual(await b.fetchHttp(8085), `http ${aId}`);
    throws(() => http.createServer().listen(8085), { code: 'EADDRINUSE' });
    await rejects(b.listenHttp(8085), /EADDRINUSE/);
    await a.closeHttp();
    const entry = http
      .createServer((req, res) => res.end('http entry'))
      .listen(8085);
    strictEqual(await a.fetchHttp(8085), 'http entry');
    entry.close();
    await rejects(a.fetchHttp(8085), /Http server with port 8085 not found/);
  },
};
