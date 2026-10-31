import { WorkerEntrypoint } from 'cloudflare:workers';
declare const binding: IdentityBinding;
const token: Promise<string> = binding.getToken();
const grant: Promise<string> = binding.getAuthorizationGrant({
  authorizationServer: 'https://auth.example.com',
  scope: ['read'],
});
const response: Promise<Response> = binding.fetch('https://api.example.com');
void [token, grant, response];
// @ts-expect-error Audience selection belongs in configuration.
binding.getToken({ audience: 'other' });
// @ts-expect-error Freshness is unconditional.
binding.getToken({ fresh: true });
// @ts-expect-error No signing-only API.
binding.signRequest('https://api.example.com');

const identity: Promise<Rpc.Stub<IdentityFetcher>> =
  binding.getIdentity('customer_x');
const defaultIdentity: Promise<Rpc.Stub<IdentityFetcher>> =
  binding.getIdentity();
void [identity, defaultIdentity];
// @ts-expect-error No authority overrides.
binding.getIdentity({ label: 'x', accountId: 'forged' });
// @ts-expect-error Selection is fixed by getIdentity.
(await identity).getIdentity('other');
// @ts-expect-error Metadata is immutable.
(await identity).id = 'forged';

class IdentityRelay extends WorkerEntrypoint {
  async relay(
    identity: Rpc.Stub<IdentityFetcher>
  ): Promise<Rpc.Stub<IdentityFetcher>> {
    return identity.dup();
  }
}
declare const relay: Service<IdentityRelay>;
const relayed = await relay.relay(await binding.getIdentity('fixed'));
const remoteId: Promise<string> = relayed.id;
const relayedResponse: Promise<Response> = relayed.fetch(
  'https://api.example.com'
);
void [remoteId, relayedResponse];

const selected = await binding.getIdentity('fixed');
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
