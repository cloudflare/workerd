export interface Fetcher {
  fetch: (
    request: Request,
    env: unknown,
    context: unknown
  ) => Promise<Response>;
}
export const portMapper = new Map<number, Fetcher>();
