// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
  lookupHandler,
  type FetchHandler as Fetcher,
  type ConnectHandler,
  type InboundSocket,
} from 'cloudflare-internal:http';

interface ServerDescriptor {
  port?: number | null | undefined;
}

interface NodeStyleServer {
  listen(...args: unknown[]): this;
  address(): { port?: number | null | undefined } | null;
}

function validatePort(port: unknown): number {
  if (
    !Number.isFinite(port) ||
    (port as number) < 0 ||
    (port as number) > 65535
  ) {
    throw new Error('Failed to determine port for server');
  }
  return port as number;
}

function invalidArg(message: string): Error {
  const error = new Error(message);
  // @ts-expect-error TS2339 We're imitating Node.js errors.
  error.code = 'ERR_INVALID_ARG_VALUE';
  return error;
}

// Resolves the port a descriptor refers to: a number, { port }, or a Node-style
// server, which is started with listen() if it has no address yet.
function resolvePort(
  desc: number | ServerDescriptor | NodeStyleServer
): number {
  if (typeof desc === 'number') {
    desc = { port: desc };
  }
  // While the TypeScript type system prevents `desc` from being null or undefined,
  // JavaScript does not, so we need to check for it at runtime.
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (desc == null) {
    throw new Error('Server descriptor cannot be null or undefined');
  }

  let port: number | null = null;
  if (
    (desc as ServerDescriptor).port == null &&
    typeof (desc as NodeStyleServer).listen === 'function'
  ) {
    const server = desc as NodeStyleServer;
    let serverPort = server.address()?.port;
    if (typeof serverPort === 'number') {
      port = serverPort;
    } else {
      // listen() assigns the port synchronously.
      server.listen();
      serverPort = server.address()?.port;
      if (typeof serverPort === 'number') {
        port = serverPort;
      }
    }
  } else if (typeof (desc as ServerDescriptor).port === 'number') {
    port = (desc as ServerDescriptor).port as number;
  }

  return validatePort(port);
}

// The port of an inbound socket is the port of its declared local address: the
// CONNECT authority for a service binding, or the listener address for a
// sockets entry.
function portFromAuthority(authority: string | null | undefined): number {
  const m = typeof authority === 'string' ? /:(\d+)$/.exec(authority) : null;
  if (m === null) {
    throw new Error(
      `Failed to determine port for inbound socket from local address ${String(authority)}`
    );
  }
  return validatePort(Number(m[1]));
}

export async function handleAsNodeRequest(
  desc: number | ServerDescriptor,
  request: Request,
  env?: unknown,
  ctx?: unknown
): Promise<Response> {
  if (typeof desc === 'number') {
    desc = { port: desc };
  }
  // While TypeScript will complain if `desc` is null or undefined,
  // JavaScript does not enforce this, so we need to check at runtime.
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  const port = validatePort(desc?.port);
  const instance = lookupHandler(port);
  if (!instance || !('fetch' in instance)) {
    throw invalidArg(
      `Http server with port ${port} not found. This is likely a bug with your code. ` +
        `You should check if server.listen() was called with the same port (${port})`
    );
  }
  return await instance.fetch(request, env, ctx);
}

// Routes an inbound platform socket to the net.Server listening on the port the
// socket arrived on. Resolves when the connection is finished.
export async function handleAsNodeConnection(
  socket: InboundSocket,
  env?: unknown,
  ctx?: unknown
): Promise<void> {
  const port = portFromAuthority((await socket.opened).localAddress);
  const instance = lookupHandler(port);
  if (!instance || !('connect' in instance)) {
    throw invalidArg(
      `No net.Server is listening on port ${port}. Call server.listen(${port}) to accept ` +
        `connections arriving on that port.`
    );
  }
  await instance.connect(socket, env, ctx);
}

export function connectHandler(): ConnectHandler {
  return {
    async connect(
      socket: InboundSocket,
      env?: unknown,
      ctx?: unknown
    ): Promise<void> {
      await handleAsNodeConnection(socket, env, ctx);
    },
  };
}

export function httpServerHandler(
  desc: number | ServerDescriptor | NodeStyleServer
): Fetcher {
  const port = resolvePort(desc);
  return {
    async fetch(req: Request, env?: unknown, ctx?: unknown): Promise<Response> {
      return await handleAsNodeRequest({ port }, req, env, ctx);
    },
  };
}
