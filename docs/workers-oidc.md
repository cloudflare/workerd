# Managed OAuth transport

Identity bindings use the existing protected service binding and JSRPC transport.
The identity service owns OAuth acquisition, credential custody, authentication
headers, cache partitioning, and destination limits. The runtime does not inspect
OAuth configuration, access tokens, or DPoP proofs.

The service uses automatic DPoP for new capabilities, with no public policy
setting. It sends a token proof, accepts DPoP or Bearer, and authenticates API
requests using the returned type. Only DPoP credentials carry API proofs and
process resource nonce challenges. Bearer API responses never trigger an
authentication replay. Failed exchanges never retry without proof.

New compiler OAuth capabilities use protected schema version 4 without policy
fields. Earlier compiled versions 1–3 retain their original behavior internally.
Forwarding a selected handle preserves its compiled authority through ordinary RPC. No additional
runtime method, compatibility flag, or OAuth-aware native wrapper is needed for
this automatic behavior. The existing local unsupported stub remains appropriate.

Web Bot Auth composition signs final Authorization and DPoP when present. It does
not convert a bearer token into a bound token. Receiving Workers still need an
OAuth verifier for these credentials: this sender policy does not extend
`ctx.identity.caller()` to verify external OAuth tokens or DPoP.

The authoritative wire and failure contract is the
[identity service specification](https://gitlab.cfdata.org/birvine-broque/workers-oidc/-/blob/main/docs/spec.md#managed-oauth-fetch).
Production enablement remains subject to its receiver and connection-path
qualification gates.
