// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch;
}

export class BrowserRendering {
  private readonly fetcher: Fetcher;

  public constructor(fetcher: Fetcher) {
    this.fetcher = fetcher;
  }

  public async fetch(
    input: RequestInfo | URL,
    init?: RequestInit
  ): Promise<Response> {
    return this.fetcher.fetch(input, init);
  }
}

export default function makeBinding(env: {
  fetcher: Fetcher;
}): BrowserRendering {
  return new BrowserRendering(env.fetcher);
}
