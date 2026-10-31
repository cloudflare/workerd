/** RPC-safe fetch options. Normalize URL, FormData and URLSearchParams with new Request(). */
type IdentityFetchOptions = Omit<RequestInit, 'body' | 'cf' | 'signal'> & {
  body?:
    string | ArrayBuffer | ArrayBufferView | ReadableStream<Uint8Array> | Blob;
};
/** Service-hosted managed HTTP identity with a fixed parent and optional label. */
interface IdentityFetcher extends Rpc.RpcTargetBranded {
  /** Stable opaque identity ID; await this property on its RPC stub. */
  readonly id: Promise<string>;
  /** Public Web Bot Auth identity URL; await this property on its RPC stub. */
  readonly signatureAgent: Promise<string>;
  /** Public signed key directory URL; await this property on its RPC stub. */
  readonly directoryURL: Promise<string>;
  /** Signs and sends within the parent binding's destination limits. */
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

/** Verified credential facts. Your code must still authorize the caller and JWT audience. */
type CloudflareServiceBindingCaller = Readonly<{
  kind: "service-binding";
  accountId: string;
  workerId: string;
}>;

type CloudflareWorkloadIdentity =
  | CloudflareServiceBindingCaller
  | Readonly<{ kind: 'signed-request'; id: string }>
  | Readonly<{
      kind: 'jwt'; issuer: string; subject: string; audience: string;
      issuedAt: number; expiresAt: number; tokenId: string;
    }>;
