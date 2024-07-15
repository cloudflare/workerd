// Copyright (c) 2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

interface Fetcher {
  fetch: typeof fetch
}

class KV {
  private readonly target: Fetcher
  private readonly accountId: string
  private readonly stableId: string
  private readonly stableIdIsUser: boolean

  public constructor(target: Fetcher, accountId: string, stableId: string, stableIdIsUser: boolean) {
    this.target = target
    this.accountId = accountId
    this.stableId = stableId
    this.stableIdIsUser = stableIdIsUser
  }

  public async get(key: string) {
    const url = `https://fake-host/${encodeURIComponent(key)}?urlencoded=true`;
    const fetchOptions = {
      method: "GET",
      headers: {
        "CF-KV-FLPROD-405": url,
        "cf-kv-account-id": `${this.accountId}`,
        "cf-kv-caller-stable-id": `${this.stableId}`,
        "cf-kv-caller-stable-id-is-user": `${this.stableIdIsUser}` ? "1" : "0",
      },
    };
    const response = await this.target.fetch(url, fetchOptions);
    return response.text();
  }
}

export default function makeBinding(env: { fetcher: Fetcher, accountId: string }): KV {
  return new KV(env.fetcher, env.accountId, "", false);
}
