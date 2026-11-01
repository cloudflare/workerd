export {};

declare const binding: IdentityBinding;
const token: Promise<string> = binding.getToken();
const grant: Promise<string> = binding.getAuthorizationGrant({
  authorizationServer: 'https://auth.example.com',
  scope: ['read'],
});
const response: Promise<Response> = (await binding.getIdentity()).fetch(
  'https://api.example.com'
);
void [token, grant, response];
// @ts-expect-error Audience selection belongs in configuration.
binding.getToken({ audience: 'other' });
// @ts-expect-error No signing-only API.
binding.signRequest('https://api.example.com');

// @ts-expect-error No authority overrides.
binding.getIdentity({ label: 'x', accountId: 'forged' });

const selected = await binding.getIdentity('fixed');
// @ts-expect-error Selection is fixed by getIdentity.
selected.getIdentity('other');
// @ts-expect-error Metadata is immutable.
selected.id = 'forged';
const selectedId: Promise<string> = selected.id;
const selectedResponse: Promise<Response> = selected.fetch(
  new Request(new URL('https://api.example.com'))
);
const selectedCopy: Rpc.Stub<IdentityFetcher> = selected.dup();
selectedCopy[Symbol.dispose]();
// @ts-expect-error Identity metadata is remote, even before forwarding.
const synchronousId: string = selected.id;
// @ts-expect-error URL is not RPC serializable; use new Request(url).
selected.fetch(new URL('https://api.example.com'));
selected.fetch('https://api.example.com', {
  method: 'POST',
  // @ts-expect-error FormData needs caller-side Request normalization.
  body: new FormData(),
});
selected.fetch('https://api.example.com', {
  method: 'POST',
  // @ts-expect-error URLSearchParams needs caller-side Request normalization.
  body: new URLSearchParams(),
});
selected.fetch('https://api.example.com', {
  // @ts-expect-error A labeled handle cannot carry routing overrides.
  cf: { resolveOverride: 'forged' },
});
void [selectedId, selectedResponse, synchronousId];

// @ts-expect-error Signed requests use the selected identity's RPC fetch method.
binding.fetch('https://api.example.com');
selected.fetch('https://api.example.com', {
  // @ts-expect-error AbortSignal transport is outside the identity fetch contract.
  signal: new AbortController().signal,
});

// Receiver facts need no outbound binding or configuration.
declare const receiverContext: ExecutionContext;
const caller: CloudflareWorkloadIdentity | null = await receiverContext.identity.caller();
if (caller?.kind === 'signed-request') {
  const signatureAgent: string = caller.signatureAgent;
  // @ts-expect-error Signed callers expose the URL as signatureAgent, not an opaque id.
  caller.id;
  // @ts-expect-error Signed URLs do not assert JWT workload metadata.
  caller.subject;
  // @ts-expect-error Signed requests do not expose a JWT originating version.
  caller.originVersionId;
  // @ts-expect-error Validated facts are immutable.
  caller.signatureAgent = 'forged';
  void signatureAgent;
 } else if (caller?.kind === 'service-binding') {
  const native: CloudflareServiceBindingCaller = caller;
  const ids: [string, string] = [native.accountId, native.workerId];
  // @ts-expect-error Native callers expose no delegated credential subject.
  native.subject;
  // @ts-expect-error Native caller facts do not expose a JWT originating version.
  native.originVersionId;
  // @ts-expect-error Native caller facts are immutable.
  native.workerId = 'forged';
  void ids;
} else if (caller?.kind === 'jwt') {
  const audience: string = caller.audience;
  const expiry: number = caller.expiresAt;
  const version: string = caller.originVersionId;
  // @ts-expect-error Verified origin metadata is immutable.
  caller.originVersionId = 'forged';
  // @ts-expect-error JWT identities do not assert a Signature-Agent URL.
  caller.signatureAgent;
  void [audience, expiry, version];
}
// @ts-expect-error Receiver trust is platform-owned; no issuer/configuration argument.
receiverContext.identity.caller({ issuer: 'https://attacker.example' });

declare const actorContext: DurableObjectState;
const actorCaller: Promise<CloudflareWorkloadIdentity | null> = actorContext.identity.caller();
void actorCaller;

const publicMetadata: IdentityOAuthClientMetadataDocument = await selected.getClientMetadata();
const explicitMetadata = await selected.getClientMetadata('https://client.example/client.json');
const assertion: string = await selected.getClientAssertion({ clientId: 'provider-id', audience: 'https://auth.example' });
// @ts-expect-error Client and AS selection are required for public assertions.
selected.getClientAssertion();
// @ts-expect-error No token, key or nonce API is exposed.
selected.getDpopProof('https://api.example');
void [publicMetadata, explicitMetadata, assertion];
