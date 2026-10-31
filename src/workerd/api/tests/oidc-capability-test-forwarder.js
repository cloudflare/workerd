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
    const customer = await binding.getCustomerIdentity('customer_x');
    const customerResponse = await binding.fetchForCustomer(
      'customer_x',
      'https://api.example/'
    );
    const sanitized = await binding.fetch('https://api.example/', {
      headers: { 'cf-workers-identity-customer': 'forged' },
    });
    return {
      token,
      grant,
      fetch: await response.json(),
      customer,
      customerFetch: await customerResponse.json(),
      sanitized: await sanitized.json(),
    };
  }
}
