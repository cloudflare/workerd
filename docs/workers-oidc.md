# Managed OAuth transport

Identity bindings use the existing protected service binding and JSRPC transport.
The identity service owns OAuth acquisition, credential custody, authentication
headers, cache partitioning, and destination limits. The runtime does not inspect
OAuth configuration, access tokens, or DPoP proofs.

The service supports `oauth.dpop: "preferred"` (the public default), `"required"`,
and `"disabled"`. Preferred accepts either a DPoP or Bearer token response to its
proof-carrying token request. Required accepts only DPoP. Disabled sends no token
proof and accepts only Bearer. Only DPoP credentials carry API proofs and process
DPoP nonce challenges. Bearer API responses never trigger an authentication replay.

New OAuth compilations carry explicit resolved policy in protected schema version
3. Legacy version-1/2 OAuth retains required DPoP. Forwarding a selected handle
preserves its compiled authority and policy through ordinary RPC. No additional
runtime method, compatibility flag, or OAuth-aware native wrapper is needed for
these policy choices. The existing local unsupported stub remains appropriate.

Web Bot Auth composition signs final Authorization and DPoP when present. It does
not convert a bearer token into a bound token. Receiving Workers still need an
OAuth verifier for these credentials: this sender policy does not extend
`ctx.identity.caller()` to verify external OAuth tokens or DPoP.

The authoritative wire and failure contract is the
[identity service specification](https://gitlab.cfdata.org/birvine-broque/workers-oidc/-/blob/main/docs/spec.md#managed-oauth-fetch).
Production enablement remains subject to its receiver and connection-path
qualification gates.
