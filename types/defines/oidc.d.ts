/** RPC-safe fetch options. Normalize URL, FormData and URLSearchParams with new Request(). */
type IdentityFetchOptions = Omit<RequestInit, 'body' | 'cf' | 'signal'> & {
  body?:
    string | ArrayBuffer | ArrayBufferView | ReadableStream<Uint8Array> | Blob;
};
/** Public registration metadata; contains no private key, assertion or access token. */
interface IdentityOAuthClientMetadataDocument {
  /** Exact configured or explicitly selected client ID. */
  client_id: string;
  /** Public opaque workload name. */
  client_name: string;
  token_endpoint_auth_method: 'private_key_jwt';
  token_endpoint_auth_signing_alg: 'Ed25519';
  /** Active and spare public verification keys. */
  jwks: { keys: JsonWebKey[] };
  grant_types: 'client_credentials'[];
  response_types: never[];
}
/** Service-hosted managed HTTP identity with a fixed parent and optional label. */
interface IdentityFetcher extends Rpc.RpcTargetBranded {
  /** Stable opaque identity ID; await this property on its RPC stub. */
  readonly id: Promise<string>;
  /** Public Web Bot Auth identity URL; await this property on its RPC stub. */
  readonly signatureAgent: Promise<string>;
  /** Public signed key directory URL; await this property on its RPC stub. */
  readonly directoryURL: Promise<string>;
  /** Configured client metadata; explicit publication URLs require client_authentication. */
  getClientMetadata(clientId?: string): Promise<IdentityOAuthClientMetadataDocument>;
  /** Fresh 60-second assertion. Requires client_authentication; performs no exchange. */
  getClientAssertion(options: { clientId: string; audience: string }): Promise<string>;
  /** Authenticates and sends within the parent binding's destination limits. */
  fetch(
    input: Request | string,
    init?: IdentityFetchOptions
  ): Promise<Response>;
}
/** A direct platform-compiled JSRPC capability. Forwarding retains its authority. */
interface IdentityBinding {
  /** Returns a fresh five-minute JWT; requires a configured audience. */
  getToken(): Promise<string>;
  /** Returns a fresh WAG within the configured AS and scope limits. */
  getAuthorizationGrant(options: {
    authorizationServer: string;
    scope?: string[];
  }): Promise<string>;
  /** Returns a fixed identity RPC stub; use normal dup() and disposal rules. */
  getIdentity(label?: string): Promise<Rpc.Stub<IdentityFetcher>>;
}

/** Protected facts about the immediate calling Worker on an eligible direct binding. */
type CloudflareServiceBindingCaller = Readonly<{
  kind: "service-binding";
  accountId: string;
  workerId: string;
}>;

/**
 * Native facts identify the immediate caller; HTTP facts identify credential authority.
 * Forwarded identity capabilities retain their originating authority, not the current executor.
 * Your code must still authorize the caller and JWT issuer, subject and audience.
 */
type CloudflareWorkloadIdentity =
  | CloudflareServiceBindingCaller
  | Readonly<{ kind: 'signed-request'; signatureAgent: string }>
  | Readonly<{
      kind: 'jwt'; issuer: string; subject: string; audience: string;
      issuedAt: number; expiresAt: number; tokenId: string;
      /** Signed version_id of the originating identity binding, preserved by forwarding. */
      originVersionId: string;
    }>;
