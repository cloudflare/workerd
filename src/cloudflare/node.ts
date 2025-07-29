// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { portMapper } from 'cloudflare-internal:http';

export function httpServerHandler(
  { port }: { port?: number } = {},
  handlers: Record<string, unknown> = {}
): {
  fetch(request: Request): Promise<Response>;
} {
  if (port == null) {
    throw new Error('Port is required when calling httpServerHandler()');
  }
  // eslint-disable-next-line @typescript-eslint/no-unnecessary-condition
  if (handlers == null || typeof handlers !== 'object') {
    throw new Error(
      'Handlers parameter passed to httpServerHandler method must be an object'
    );
  }

  return Object.assign(handlers, {
    // We intentionally omitted ctx and env variables. Users should use
    // importable equivalents to access those values. For example, using
    // import { env, waitUntil } from 'cloudflare:workers
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
