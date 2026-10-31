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
    const workload = await binding.getIdentity();
    const response = await workload.fetch('https://api.example/');
    const customer = await binding.getIdentity('customer_x');
    const customerResponse = await customer.fetch('https://api.example/');
    const sanitized = await workload.fetch('https://api.example/', {
      headers: {
        'cf-workers-identity-label': 'forged',
        'cf-workers-identity-customer': 'forged',
      },
    });
    return {
      token,
      grant,
      fetch: await response.json(),
      customer: { id: await customer.id },
      customerFetch: await customerResponse.json(),
      sanitized: await sanitized.json(),
    };
  }
  async exerciseIdentity(identity) {
    return {
      id: await identity.id,
      directoryURL: await identity.directoryURL,
      fetch: await (
        await identity.fetch('https://api.example/', {
          headers: { 'cf-workers-identity-label': 'forged' },
        })
      ).json(),
    };
  }
  async select(binding) {
    return binding.getIdentity('returned');
  }
}
