export interface Fetcher {
  fetch: (request: Request) => Promise<Response>;
}
export const portMapper = new Map<number, Fetcher>();
