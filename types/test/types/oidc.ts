import { expectTypeOf } from 'expect-type';
import { DurableObject, WorkerEntrypoint } from 'cloudflare:workers';
interface Env { IDENTITY: OidcIdentityBinding }
declare const env: Env;
declare const ctx: ExecutionContext;
declare const state: DurableObjectState;
expectTypeOf(env.IDENTITY.getToken()).toEqualTypeOf<Promise<string>>();
expectTypeOf(env.IDENTITY.getToken({ audience: 'urn:recipient', fresh: true })).toEqualTypeOf<Promise<string>>();
// @ts-expect-error Authority comes from an explicit binding.
ctx.oidc;
// @ts-expect-error DO state has no implicit authority.
state.oidc;
// @ts-expect-error The assignment fixes subject scope.
env.IDENTITY.getToken({ subject: 'namespace' });
// @ts-expect-error Callers cannot select an identity.
env.IDENTITY.getToken({ identityId: 'forged' });
// @ts-expect-error Freshness is strictly boolean.
env.IDENTITY.getToken({ fresh: 'true' });
class IdentityObject extends DurableObject<Env> {
  token(): Promise<string> { return this.env.IDENTITY.getToken(); }
}
class IdentityService extends WorkerEntrypoint<Env> {
  token(): Promise<string> { return this.env.IDENTITY.getToken({ fresh: true }); }
}
