// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { portMapper } from 'cloudflare-internal:http';

interface Fetcher {
  fetch(request: Request, env?: unknown, ctx?: unknown): Promise<Response>;
}

interface ServerDescriptor {
  port?: number | null | undefined;
}

interface NodeStyleServer {
  listen(...args: unknown[]): this;
  address(): { port?: number | null | undefined };
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
  const instance = portMapper.get(port);
  if (!instance) {
    const error = new Error(
      `Http server with port ${port} not found. This is likely a bug with your code. ` +
        `You should check if server.listen() was called with the same port (${port})`
    );
    // @ts-expect-error TS2339 We're imitating Node.js errors.
    error.code = 'ERR_INVALID_ARG_VALUE';
    throw error;
  }
  return await instance.fetch(request, env, ctx);
}

export function httpServerHandler(
  desc: number | ServerDescriptor | NodeStyleServer
): Fetcher {
  if (typeof desc === 'number') {
    desc = { port: desc };
  }
  // While the TypeScript type system prevents `desc` from being null or undefined,
  // JavaScript does not, so we need to check for it at runtime.
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (desc == null) {
    throw new Error('Server descriptor cannot be null or undefined');
  }

  // The desc can be a ServerDescriptor or a Server (where "Server" is defined
  // as a type that has a "listen()" method and an "address()" method).
  let port: number | null = null;

  // If there is no port defined and desc has a listen method, we will try to
  // access it as a Server, calling listen() if necessary to determine the port.
  if (
    (desc as ServerDescriptor).port == null &&
    typeof (desc as NodeStyleServer).listen === 'function'
  ) {
    const server = desc as NodeStyleServer;
    // First, let's see if the server-like thing already has a port.
    // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
    let serverPort = server.address()?.port;
    if (typeof serverPort === 'number') {
      // Nice, we're already bound to a port.
      port = serverPort;
    } else {
      // We're not yet bound to a port. Try calling listen() to start the server
      // and determine the port.
      server.listen();
      // Did it work? We do expect the listen in this case to synchronously
      // assign the port so we can check it immediately.
      // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
      serverPort = server.address()?.port;
      if (typeof serverPort === 'number') {
        // Nice. We've got a port now.
        port = serverPort;
      }
    }
  } else if (typeof (desc as ServerDescriptor).port === 'number') {
    // We're a ServerDescriptor with a port defined.
    port = (desc as ServerDescriptor).port as number;
  }

  port = validatePort(port);

  return {
    async fetch(req: Request, env?: unknown, ctx?: unknown): Promise<Response> {
      return await handleAsNodeRequest(
        { port: validatePort(port) },
        req,
        env,
        ctx
      );
    },
  };
}
