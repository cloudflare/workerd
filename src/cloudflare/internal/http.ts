export interface FetchHandler {
  // We don't use typeof fetch here because that would use the client-side signature,
  // which is different from the server-side signature.
  fetch: (request: Request, env?: unknown, ctx?: unknown) => Promise<Response>
}

export const portMapper = new Map<number, FetchHandler>()
