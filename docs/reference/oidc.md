# OIDC workload identity

The experimental `workers_oidc` compatibility flag exposes `ctx.oidc.getToken()` on
`ExecutionContext` and `DurableObjectState`. It has no automatic enable date and requires an
experimental-enabled runtime. Issuance requires a host signer or an explicitly configured local
development broker.

```js
const token = await ctx.oidc.getToken();
const tokenForApi = await ctx.oidc.getToken({ audience: "https://api.example.com" });
```

The result is a `Promise<string>`. Omitting `audience` asks the host to use
`https://workers.cloudflare.com/accounts/<ACCOUNT_ID>`, with the immutable public account ID.
An explicit audience must be a non-whitespace-only string of 1–2048 UTF-8 bytes. It follows JWT's
StringOrURI rules: values containing `:` must be valid RFC 3986 URIs, including non-HTTP schemes
such as `urn:example:recipient`. URI characters outside ASCII must be percent-encoded. Plain
strings without `:` can contain Unicode or spaces. Accepted values are preserved exactly;
workerd never normalizes or fetches the audience. Use distinct audiences for independent
destinations: recipients sharing an issuer, audience, and subject policy can replay tokens at
each other.

Inside a Durable Object, the default subject identifies the namespace and current object.
`{ subject: "object" }` explicitly selects that default; `{ subject: "namespace" }` selects
namespace identity. Subject options outside a Durable Object are rejected. Callers cannot supply
account, Worker, version, namespace, or object IDs, or an arbitrary token subject.

The API uses the current execution context at each call, including calls through a saved OIDC
handle. It is independent of HTTP, service/RPC, queue, scheduled, alarm, and hibernation event
dispatch. Issuance requires an active I/O context and is unavailable during module evaluation.
Containers receive their managing Durable Object's token through existing request or egress
integration; the API does not create a separate container identity.

## Host integration

Embedding runtimes implement `IoChannelFactory::getOidcToken(OidcTokenRequest)` to issue tokens.
This is a protected native channel, separate from global fetch and customer bindings. Requests
contain only an optional audience, whether the subject was explicit, and a subject mode (`WORKER`, `DURABLE_OBJECT`, or
`DURABLE_OBJECT_NAMESPACE`). The factory belongs to the current incoming request; the signer
must identify the executing workload, including when another Worker called it.

The host is responsible for:

- Deriving account, Worker, persistent Preview, executing version, namespace, and object IDs from trusted state.
- Rejecting missing metadata, remote development sessions, Version URLs, Dynamic Workers, invalid subject modes, and workloads
  without issuance permission; enforcing issuance controls and rate limits.
- Constructing account issuer and workload subject claims, signing five-minute RS256 tokens with
  `typ: "cf-workload+jwt"`, and keeping private keys outside customer code. These are workload
  assertions, not OAuth access tokens or an implementation of OIDC end-user sign-in.
- Partitioning any token cache by the execution kind, Preview ID, full identity, executing version, audience, subject mode,
  and issuance permissions; never extending token expiry during an outage.
- Auditing issuance without logging tokens and maintaining signing-key provisioning and rotation.

An unconfigured runtime rejects issuance. Host errors propagate to the returned promise. workerd
does not produce unsigned tokens or cache tokens. Public discovery and JWKS serving are separate
services and must remain available independently of issuance.


## Local development broker

A trusted development tool can set `Worker.oidcTokenService` to a `ServiceDesignator` for its
broker. The channel is separate from application bindings and global fetch. Each linked Worker
gets its own channel; Dynamic Workers do not inherit it. Keep the broker implementation and CLI
credentials outside customer modules. The `workers_oidc` flag and experimental runtime option
remain required.

```capnp
oidcTokenService = (name = "development-broker"),
```

The native getter sends `POST http://oidc/token`, `Content-Type: application/json`, with `{}` or
`{"audience":"<exact requested audience>"}`. The broker returns status 200, exactly
`Content-Type: text/plain`, and a compact JWT as the entire body. workerd bounds the response to
64 KiB and the operation to 30 seconds. It rejects other status codes, content types, malformed
compact tokens and oversized bodies without exposing response error bodies. The URL identifies
the operation on the configured channel; workerd does not resolve it through global fetch.

The broker owns the authenticated Cloudflare API session and the shared token cache. It resolves
an authorized Worker-development identity through the API, preserves the requested audience, and
returns only the resulting token. It never substitutes its own deployed workload identity. The
local Worker and its local Durable Objects share the development identity. Both explicit
`subject: "object"` and `subject: "namespace"` reject locally. No caller option selects the account,
Worker, user or development session. Cloud issuance uses the embedding host's native signer.
