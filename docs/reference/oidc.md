# OIDC identity bindings

The experimental runtime exposes `env.IDENTITY.getToken()` through an explicitly configured
identity binding. `ExecutionContext` and `DurableObjectState` do not expose an OIDC singleton.
Use an experimental-enabled runtime and the `workers_oidc` compatibility flag.

```js
const token = await env.IDENTITY.getToken();
const freshToken = await env.IDENTITY.getToken({
  audience: "https://api.example.com",
  fresh: true,
});
```

The getter returns `Promise<string>`. The binding fixes the approved assignment. Your options
can select an approved audience and request a fresh assertion. Unknown options, including
`subject`, `identityId`, `issuer`, `claims`, and `objectId`, throw. `fresh` must be a boolean.
Omitting `audience` uses the assignment's default. The runtime preserves accepted audience bytes
and never fetches the audience. Audiences must contain 1–2048 UTF-8 bytes, must not contain only
whitespace, and must follow JWT StringOrURI rules. Values containing `:` must form RFC 3986 URIs.

The host checks current authorization before every return, including cache hits. It fixes the
subject profile in the approved assignment: `identity:<ID>`,
`identity:<ID>:durable-object:<NAMESPACE>:<OBJECT>`, or
`identity:<ID>:durable-object-namespace:<NAMESPACE>`. Caller code cannot select a profile.
An assignment can restrict Worker, executing version, namespace, object, and audience. An identity's
policy applies to all of its assignments. Configuring an identity ID never grants access to it.

The binding uses the current I/O context at each call, including calls through saved handles.
The host must reject calls from an unrelated consumer. Module evaluation has no active I/O
context. Containers can receive a token from their managing Durable Object through an existing
request or egress integration; the API does not create a separate container identity.

## Host integration

Embedding runtimes implement `IoChannelFactory::getOidcToken(OidcTokenRequest)`. This protected
native channel carries a fixed assignment selector, optional audience, and `fresh`. The current
request's host derives the account, immutable Worker and Preview IDs, actual executing version,
and any Durable Object namespace/object IDs from trusted state.

The host must:

- Resolve current identity and assignment authority independently of deployment snapshots.
- Enforce account enablement, assignment restrictions, identity-wide restrictions, and audience policy.
- Reject unsupported execution contexts or missing authoritative evidence.
- Sign RS256 assertions with `typ: cf-workload+jwt`, a published `kid`, and a five-minute lifetime.
- Include `account_id`, `product: workers`, a random `jti` of at least 128 bits, and all execution claims.
- Keep namespace and object claims on all Durable Object assertions, regardless of subject profile.
- Partition reuse by assignment, policy revision, subject, audience, and every attested field.
- Return cached assertions only while at least 60 seconds remain. Preserve their original `jti`.
- Bypass reuse and request coalescing for `fresh: true`; each fresh assertion needs a distinct `jti`.
- Keep keys outside customer code and audit issuance without logging bearer tokens.

An unconfigured runtime rejects issuance. workerd itself neither signs nor caches tokens.
Discovery and JWKS serving belong to separate services.

## Local development broker

A trusted tool configures a native binding and `Worker.oidcTokenService`:

```capnp
bindings = [(name = "IDENTITY", oidc = "local-orders")],
oidcTokenService = (name = "development-broker"),
```

The selector identifies the binding's approved local assignment. The host-only broker resolves
it through an authenticated API session. Each local Worker gets a separate broker channel.
Dynamic Workers do not inherit it. Keep OAuth credentials and the broker outside customer modules.

The getter sends `POST http://oidc/token` with `Content-Type: application/json` and
`{"assignment":"local-orders","fresh":false}`, plus `audience` when supplied. The broker returns
status 200, exactly `Content-Type: text/plain`, and a compact JWT as the entire body. workerd bounds
the response to 64 KiB and the operation to 30 seconds. It rejects other status codes, content
types, malformed tokens, and oversized bodies without exposing error bodies. It never resolves
the operation URL through global fetch.

On failure, the broker forwards 400 for invalid options, 401 for login, 403 for access denial,
409 for session renewal, 429 for limits, or 503 for unavailability. The optional fixed hints
`X-Workers-OIDC-Error: session_limit` and `X-Workers-OIDC-Error: not_configured` supply corrective
messages. The runtime never reflects arbitrary backend errors.

Local issuance requires current human OAuth, Worker-development permission, and an approved local
assignment. Local assignments use only the `identity` profile. Tokens include user and development
session claims and omit version, Preview, namespace, and object claims. Local Durable Objects use
the same binding assignment as their Worker. Production, Preview, and local assignments remain
independent unless an identity administrator explicitly approves reuse.
