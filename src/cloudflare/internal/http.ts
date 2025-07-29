export interface Fetcher {
  fetch: typeof fetch;
}
export const portMapper = new Map<number, Fetcher>();
