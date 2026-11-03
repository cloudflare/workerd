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
  token_endpoint_auth_signing_alg: 'ES256' | 'Ed25519';
  /** Active and spare public verification keys. */
  jwks: { keys: JsonWebKey[] };
  grant_types: 'client_credentials'[];
  response_types: never[];
}
/** Select an application-supplied immutable agent ID within the parent workload. */
interface IdentitySelection {
  /** Case-sensitive, non-reused ID; 1–128 ASCII letters, digits, underscores or hyphens. */
  agentId?: string;
}
/** Service-hosted identity with a fixed actor and the parent binding's authority. */
interface IdentityFetcher extends Rpc.RpcTargetBranded {
  /** Stable opaque identity ID; await this property on its RPC stub. */
  readonly id: Promise<string>;
  /** Fresh five-minute JWT for this actor; requires the binding's configured audience. */
  getToken(): Promise<string>;
  /** Fresh WAG for this actor within the binding's authorization-server and scope limits. */
  getAuthorizationGrant(options: {
    authorizationServer: string;
    scope?: string[];
  }): Promise<string>;
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
  /** Fresh five-minute JWT for the default workload principal; requires an audience. */
  getToken(): Promise<string>;
  /** Fresh WAG for the default workload principal within the configured limits. */
  getAuthorizationGrant(options: {
    authorizationServer: string;
    scope?: string[];
  }): Promise<string>;
  /** A string always means agentId. Omission selects the default workload principal.
   * The returned stub fixes the actor; forwarding delegates its enabled token and fetch operations.
   * Saved authorization uses the app/customer connector profiles below.
   */
  getIdentity(selection?: string | IdentitySelection): Promise<Rpc.Stub<IdentityFetcher>>;
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

/** Saved authorization is a separate purpose; no token or assertion export. */
type IdentityConnectionState = 'consent_required' | 'default_required' | 'pending' | 'ready' |
  'disconnected' | 'reconnect_required' | 'scope_incompatible' | 'owner_disabled' |
  'connector_deleted' | 'connection_removed' | 'superseded';
type IdentityProviderCleanup = 'complete' | 'pending' | 'unsupported' | 'unknown';
/** Local lifecycle observation, without provider account interpretation or liveness probing. */
interface IdentityConnectionStatus {
  connectionId: string | null;
  state: IdentityConnectionState;
  observedAt: number;
  grantedScopes: string[] | null;
  scopeSource: 'token_response' | 'requested' | 'unknown';
  providerCleanup: IdentityProviderCleanup | null;
}
interface IdentityConnectionCleanup {
  revocationPending: boolean;
  providerCleanup: IdentityProviderCleanup;
}
interface IdentityConnectionCompletion { sessionId: string; flowId: string; receipt: string }
interface IdentityConnectionFlow { connectionId: string; flowId: string; expiresAt: number; authorizationUrl: string }
/** Protocol facts for application validation before activating a saved connection. */
interface IdentityPendingAuthorizationFacts {
  /** Pinned authorization server, when present in the registration snapshot. */
  issuer?: string;
  /** Opaque token-response sub; this is not an OIDC identity assertion. */
  subject?: string;
  /** Whether effective scopes came from the response or a configured fallback. */
  scopeSource: 'token_response' | 'requested' | 'unknown';
}
interface IdentityPendingConnection { connectionId: string; flowId: string; expiresAt: number; scopes: string[]; authorization: IdentityPendingAuthorizationFacts }
interface IdentityConnectionPage {
  connections: IdentityConnectionStatus[];
  defaultConnectionId: string | null;
  defaultInitialized: boolean;
  nextCursor: string | null;
}
/** Fixed actor/owner/connection generation or absence; forwarding cannot reselect. */
interface IdentityConnectedFetcher extends Rpc.RpcTargetBranded {
  readonly id: Promise<string>;
  readonly connectionId: Promise<string | null>;
  fetch(input: Request | string, init?: IdentityFetchOptions): Promise<Response>;
  getStatus(): Promise<IdentityConnectionStatus>;
}
/** Temporary inspection authority; never becomes an active handle. */
interface IdentityPendingFetcher extends Rpc.RpcTargetBranded {
  fetch(input: Request | string, init?: IdentityFetchOptions): Promise<Response>;
}
interface IdentityAppConnectionSelection { agentId?: string; connectionId?: string }
interface IdentityCustomerConnectionSelection extends IdentityAppConnectionSelection { customerId: string }
type IdentityConnectionOwnerPrefix<M extends 'app' | 'customer'> = M extends 'customer' ? [customerId: string] : [];
interface IdentityConnectionReadOperations<M extends 'app' | 'customer'> {
  listConnections(...args: [...IdentityConnectionOwnerPrefix<M>, options?: { cursor?: string; limit?: number }]): Promise<IdentityConnectionPage>;
}
interface IdentityAppConnectionBinding extends IdentityConnectionReadOperations<'app'> {
  getIdentity(): Promise<Rpc.Stub<IdentityConnectedFetcher>>;
  getIdentity(agentId: string): Promise<Rpc.Stub<IdentityConnectedFetcher>>;
  getIdentity(selection: IdentityAppConnectionSelection): Promise<Rpc.Stub<IdentityConnectedFetcher>>;
}
interface IdentityCustomerConnectionBinding extends IdentityConnectionReadOperations<'customer'> {
  getIdentity(selection: IdentityCustomerConnectionSelection): Promise<Rpc.Stub<IdentityConnectedFetcher>>;
}
/** Only roots compiled with connection_management expose these operations. */
interface IdentityConnectionManagement<M extends 'app' | 'customer'> {
  beginConnection(...args: [...IdentityConnectionOwnerPrefix<M>, options: { sessionId: string; returnTo: string; connectionId?: string }]): Promise<IdentityConnectionFlow>;
  previewConnection(...args: [...IdentityConnectionOwnerPrefix<M>, completion: IdentityConnectionCompletion]): Promise<IdentityPendingConnection>;
  getPendingIdentity(...args: [...IdentityConnectionOwnerPrefix<M>, completion: IdentityConnectionCompletion]): Promise<Rpc.Stub<IdentityPendingFetcher>>;
  completeConnection(...args: [...IdentityConnectionOwnerPrefix<M>, completion: IdentityConnectionCompletion]): Promise<{ connectionId: string; generation: string; status: 'ready' }>;
  cancelConnection(...args: [...IdentityConnectionOwnerPrefix<M>, completion: IdentityConnectionCompletion]): Promise<IdentityConnectionCleanup>;
  setDefaultConnection(...args: [...IdentityConnectionOwnerPrefix<M>, connectionId: string]): Promise<void>;
  disconnect(...args: [...IdentityConnectionOwnerPrefix<M>, connectionId: string]): Promise<IdentityConnectionCleanup>;
  removeConnection(...args: [...IdentityConnectionOwnerPrefix<M>, connectionId: string]): Promise<IdentityConnectionCleanup>;
  disconnectAll(...args: IdentityConnectionOwnerPrefix<M>): Promise<IdentityConnectionCleanup>;
  disableConnections(...args: IdentityConnectionOwnerPrefix<M>): Promise<IdentityConnectionCleanup>;
  enableConnections(...args: IdentityConnectionOwnerPrefix<M>): Promise<void>;
}
type IdentityAppManagedConnectionBinding = IdentityAppConnectionBinding & IdentityConnectionManagement<'app'>;
type IdentityCustomerManagedConnectionBinding = IdentityCustomerConnectionBinding & IdentityConnectionManagement<'customer'>;
