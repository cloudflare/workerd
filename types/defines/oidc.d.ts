/** A platform-compiled workload capability. RPC forwarding retains its original authority. */
interface IdentityBinding {
  /** Returns a fresh five-minute JWT for the configured audience. */
  getToken(): Promise<string>;
  /** Returns a fresh WAG within the binding's configured AS and scope limits. */
  getAuthorizationGrant(options: {
    authorizationServer: string;
    scope?: string[];
  }): Promise<string>;
  /** Signs and sends to a configured HTTPS destination. Redirects fail. */
  fetch(input: Request | string | URL, init?: RequestInit): Promise<Response>;
}
