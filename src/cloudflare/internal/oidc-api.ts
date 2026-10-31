// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import wrappedBinding from 'cloudflare-internal:wrapped-binding';
import entrypoints from 'cloudflare-internal:workers';

interface IdentityMetadata {
  id: string;
  signatureAgent: string;
  directoryURL: string;
}

interface Inner {
  getToken(...args: unknown[]): Promise<string>;
  getAuthorizationGrant(...args: unknown[]): Promise<string>;
  fetch(request: Request): Promise<Response>;
  getIdentity(...args: unknown[]): Promise<IdentityMetadata>;
  checkSignedFetch(url: string, method: string, label?: string): Promise<void>;
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
  async getIdentity(...args: unknown[]): Promise<IdentityFetcher> {
    if (
      args.length > 1 ||
      (args[0] !== undefined &&
        (typeof args[0] !== 'string' ||
          !/^[A-Za-z0-9_-]{1,128}$/.test(args[0])))
    ) {
      throw new TypeError(
        'getIdentity accepts an optional label of 1–128 ASCII letters, digits, underscores or hyphens.'
      );
    }
    const metadata = await this.#inner.getIdentity(...args);
    const label = args[0] as string | undefined;
    return new IdentityFetcher(this.#inner, metadata, label);
  }
  fetch(input: Request | string | URL, init?: RequestInit): Promise<Response> {
    return signedFetch(this.#inner, input, init);
  }
}
async function signedFetch(
  inner: Inner,
  input: Request | string | URL,
  init?: RequestInit,
  label?: string
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
  for (const name of [
    'authorization',
    'content-type',
    'content-encoding',
    'idempotency-key',
  ]) {
    if (/[^\x00-\x7f]/.test(headers.get(name) ?? '')) {
      throw new TypeError(
        `Signed header ${name} must contain only ASCII characters.`
      );
    }
  }
  // Serialize once. The private HTTP service path propagates ordinary fetch
  // cancellation; the signer refuses redirects and chooses the final egress.
  const request = new Request(input, { ...init, redirect: 'manual' });
  request.headers.delete('content-length');
  request.headers.delete('cf-workers-identity-customer');
  request.headers.delete('cf-workers-identity-label');
  if (label !== undefined)
    request.headers.set('cf-workers-identity-label', label);
  request.signal.throwIfAborted();
  await inner.checkSignedFetch(request.url, request.method, label);
  request.signal.throwIfAborted();
  return inner.fetch(request);
}

// Ordinary RPC forwarding retains the original private stub and fixed selection.
class IdentityFetcher extends entrypoints.RpcTarget {
  #inner: Inner;
  #metadata: IdentityMetadata;
  #label: string | undefined;
  constructor(inner: Inner, metadata: IdentityMetadata, label?: string) {
    super();
    this.#inner = inner;
    this.#metadata = Object.freeze(metadata);
    this.#label = label;
  }
  get id(): string {
    return this.#metadata.id;
  }
  get signatureAgent(): string {
    return this.#metadata.signatureAgent;
  }
  get directoryURL(): string {
    return this.#metadata.directoryURL;
  }
  fetch(input: Request | string | URL, init?: RequestInit): Promise<Response> {
    return signedFetch(this.#inner, input, init, this.#label);
  }
}
export default function makeBinding(env: { fetcher: Inner }): IdentityBinding {
  return new IdentityBinding(env.fetcher);
}
