/** Stable managed HTTP identity with a fixed parent and optional label. */
interface IdentityFetcher extends Rpc.RpcTargetBranded {
  readonly id: string;
  readonly signatureAgent: string;
  readonly directoryURL: string;
  /** Signs and sends within the parent binding's destination limits. */
  fetch(input: Request | string | URL, init?: RequestInit): Promise<Response>;
}
/** A platform-compiled workload capability. Forwarding retains its authority. */
interface IdentityBinding {
  /** Returns a fresh five-minute JWT; requires a configured audience. */
  getToken(): Promise<string>;
  /** Returns a fresh WAG within the configured AS and scope limits. */
  getAuthorizationGrant(options: {
    authorizationServer: string;
    scope?: string[];
  }): Promise<string>;
  /** Returns the default identity or a stable labeled identity and fixed fetcher. */
  getIdentity(label?: string): Promise<IdentityFetcher>;
  /** Signs and sends as the default workload identity. Redirects fail. */
  fetch(input: Request | string | URL, init?: RequestInit): Promise<Response>;
}
