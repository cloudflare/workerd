// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { portMapper } from 'cloudflare-internal:http';

interface ServerDescriptor {
  port?: number | null | undefined;
}

interface NodeStyleServer {
  listen(...args: unknown[]): this;
  address(): { port?: number | null | undefined };
}

export function httpServerHandler(
  desc: ServerDescriptor | NodeStyleServer,
  handlers?: Record<string, unknown> | null
): {
  fetch(request: Request): Promise<Response>;
} {
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

  if (port == null) {
    throw new Error('Failed to determine port for server');
  }

  if (handlers !== undefined && typeof handlers !== 'object') {
    throw new Error(
      'Handlers parameter passed to httpServerHandler method must be an object'
    );
  }

  return Object.assign(handlers ?? {}, {
    // We intentionally omitted ctx and env variables. Users should use
    // importable equivalents to access those values. For example, using
    // import { env, waitUntil } from 'cloudflare:workers
    // `disallow_importable_env` compat flag should not be set if you are using this
    // and need access to the env since that will prevent access.
    async fetch(request: Request): Promise<Response> {
      const instance = portMapper.get(port);
      // TODO: Investigate supporting automatically assigned ports as being bound without a port configuration.
      if (!instance) {
        const error = new Error(
          `Http server with port ${port} not found. This is likely a bug with your code. You should check if server.listen() was called with the same port (${port})`
        );
        // @ts-expect-error TS2339 We're imitating Node.js errors.
        error.code = 'ERR_INVALID_ARG_VALUE';
        throw error;
      }
      return instance.fetch(request);
    },
  });
}
