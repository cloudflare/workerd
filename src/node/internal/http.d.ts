export const portMapper: Map<
  number,
  { fetch: (request: Request) => Promise<Response> }
>;
