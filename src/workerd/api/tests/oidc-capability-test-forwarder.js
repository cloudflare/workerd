// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import { WorkerEntrypoint } from 'cloudflare:workers';
export class Forwarder extends WorkerEntrypoint {
  async exercise(binding) {
    const token = await binding.getToken();
    const grant = await binding.getAuthorizationGrant({
      authorizationServer: 'https://auth.example/',
    });
    const response = await binding.fetch('https://api.example/');
    return { token, grant, fetch: await response.json() };
  }
}
