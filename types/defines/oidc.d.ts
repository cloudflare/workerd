/** Stable opaque Web Bot Auth customer metadata. No key material or child capability. */
interface IdentityCustomerIdentity {
  id: string;
  signatureAgent: string;
  directoryURL: string;
}
/** A platform-compiled workload capability. RPC forwarding retains its original authority. */
interface IdentityBinding {
  /** Returns a fresh five-minute JWT for the configured audience. */
  getToken(): Promise<string>;
  /** Returns a fresh WAG within the binding's configured AS and scope limits. */
  getAuthorizationGrant(options: {
    authorizationServer: string;
    scope?: string[];
  }): Promise<string>;
  /** Returns stable customer metadata under a web-bot-auth binding. The platform authorizes customer access. */
  getCustomerIdentity(customerId: string): Promise<IdentityCustomerIdentity>;
  /** Signs and sends as the selected customer using Web Bot Auth. Redirects fail. */
  fetchForCustomer(customerId: string, input: Request | string | URL, init?: RequestInit): Promise<Response>;
  /** Signs and sends to a configured HTTPS destination. Redirects fail. */
  fetch(input: Request | string | URL, init?: RequestInit): Promise<Response>;
}
