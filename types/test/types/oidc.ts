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

const identity: Promise<IdentityFetcher> = binding.getIdentity('customer_x');
const defaultIdentity: Promise<IdentityFetcher> = binding.getIdentity();
void [identity, defaultIdentity];
// @ts-expect-error No authority overrides.
binding.getIdentity({ label: 'x', accountId: 'forged' });
// @ts-expect-error Selection is fixed by getIdentity.
(await identity).getIdentity('other');
// @ts-expect-error Metadata is immutable.
(await identity).id = 'forged';

class IdentityRelay extends WorkerEntrypoint {
  async relay(identity: IdentityFetcher): Promise<IdentityFetcher> { return identity; }
}
declare const relay: Service<IdentityRelay>;
const relayed = await relay.relay(await binding.getIdentity('fixed'));
const remoteId: Promise<string> = relayed.id;
const relayedResponse: Promise<Response> = relayed.fetch('https://api.example.com');
void [remoteId, relayedResponse];
