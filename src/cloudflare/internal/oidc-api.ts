// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import wrappedBinding from 'cloudflare-internal:wrapped-binding';

interface IdentityCustomerIdentity {
  id: string;
  signatureAgent: string;
  directoryURL: string;
}

interface Inner {
  getToken(...args: unknown[]): Promise<string>;
  getAuthorizationGrant(...args: unknown[]): Promise<string>;
  fetch(request: Request): Promise<Response>;
  getCustomerIdentity(...args: unknown[]): Promise<IdentityCustomerIdentity>;
  checkSignedFetch(
    url: string,
    method: string,
    customerId?: string
  ): Promise<void>;
}
const INIT = new Set([
  'method',
  'headers',
  'body',
  'redirect',
  'signal',
  'integrity',
  'keepalive',
  'credentials',
  'mode',
  'cache',
  'referrer',
  'referrerPolicy',
  'duplex',
]);
const FRAMING = [
  'host',
  'content-length',
  'transfer-encoding',
  'connection',
  'trailer',
  'upgrade',
  'proxy-authorization',
  'proxy-authenticate',
  'proxy-connection',
  'te',
];

// The normal wrapped-binding serializer retains only the private service stub.
// Reconstructing this wrapper never copies application-supplied authority.
class IdentityBinding extends wrappedBinding.WrappedBinding {
  #inner: Inner;
  constructor(inner: Inner) {
    super(inner);
    this.#inner = inner;
  }
  getToken(...args: unknown[]): Promise<string> {
    return this.#inner.getToken(...args);
  }
  getAuthorizationGrant(...args: unknown[]): Promise<string> {
    return this.#inner.getAuthorizationGrant(...args);
  }
  getCustomerIdentity(...args: unknown[]): Promise<IdentityCustomerIdentity> {
    return this.#inner.getCustomerIdentity(...args);
  }
  fetchForCustomer(
    customerId: string,
    input: Request | string | URL,
    init?: RequestInit
  ): Promise<Response> {
    if (
      typeof customerId !== 'string' ||
      !/^[A-Za-z0-9_-]{1,128}$/.test(customerId)
    ) {
      return Promise.reject(
        new TypeError(
          'Invalid customerId. Use 1–128 ASCII letters, digits, underscores or hyphens.'
        )
      );
    }
    return this.#fetch(input, init, customerId);
  }
  fetch(input: Request | string | URL, init?: RequestInit): Promise<Response> {
    return this.#fetch(input, init);
  }
  async #fetch(
    input: Request | string | URL,
    init?: RequestInit,
    customerId?: string
  ): Promise<Response> {
    if (init && Reflect.ownKeys(init).some((key) => !INIT.has(String(key)))) {
      throw new TypeError(
        'Unsupported request transport override. Use standard fetch options.'
      );
    }
    if (init?.redirect !== undefined && init.redirect !== 'error') {
      throw new TypeError(
        'Signed fetch requires redirect: "error". Call the final destination directly.'
      );
    }
    const headers = new Headers(
      init?.headers ?? (input instanceof Request ? input.headers : undefined)
    );
    if (FRAMING.some((name) => headers.has(name))) {
      throw new TypeError(
        'Routing or framing headers cannot override signed fetch. Remove those headers.'
      );
    }
    // Serialize once. The private HTTP service path propagates ordinary fetch
    // cancellation; the signer refuses redirects and chooses the final egress.
    const request = new Request(input, { ...init, redirect: 'manual' });
    request.headers.delete('content-length');
    request.headers.delete('cf-workers-identity-customer');
    if (customerId !== undefined)
      request.headers.set('cf-workers-identity-customer', customerId);
    request.signal.throwIfAborted();
    await this.#inner.checkSignedFetch(request.url, request.method, customerId);
    request.signal.throwIfAborted();
    return this.#inner.fetch(request);
  }
}
export default function makeBinding(env: { fetcher: Inner }): IdentityBinding {
  return new IdentityBinding(env.fetcher);
}
