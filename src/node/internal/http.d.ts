export const portMapper: Map<
  number,
  { fetch: (request: Request, env: unknown, ctx: unknown) => Promise<Response> }
>;
