// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export interface FetchHandler {
  // We don't use typeof fetch here because that would use the client-side signature,
  // which is different from the server-side signature.
  fetch: (request: Request, env?: unknown, ctx?: unknown) => Promise<Response>;
}

export const portMapper = new Map<number, FetchHandler>();
