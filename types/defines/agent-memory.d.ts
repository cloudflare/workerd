// ============================================================================
// Agent Memory — SCAFFOLD
//
// This file declares the minimum type surface required for user Workers to
// reference the `AgentMemoryNamespace` binding without a type error. It was
// introduced to unblock `wrangler types` output in workers-sdk PR
// https://github.com/cloudflare/workers-sdk/pull/13610.
//
// The real runtime API is owned by the Agent Memory product team. They are
// expected to extend this scaffold with the complete method surface, accurate
// return types, and any supporting option / result interfaces — or replace it
// entirely with generated types driven from a C++ JSG resource.
//
// Until that happens, consumers should treat every declaration in this file
// as intentionally incomplete. Method signatures have been kept deliberately
// narrow and only cover what is exercised by the workers-sdk e2e fixture at
// `packages/wrangler/e2e/remote-binding/workers/agent-memory.js`.
// ============================================================================

/**
 * Summary of an Agent Memory context.
 *
 * The concrete shape is owned by the Agent Memory product team and has not
 * been declared here yet. It is modelled as an open record so user code that
 * reads from a summary can compile, at the cost of losing property-level type
 * checking until the real interface lands.
 *
 * @todo Replace with the real summary shape once defined by the Agent Memory team.
 */
export interface AgentMemoryContextSummary {
  [key: string]: unknown;
}

/**
 * A single Agent Memory context, scoped to a context id.
 *
 * Returned by {@link AgentMemoryNamespace.getContext}. This scaffold only
 * declares the methods exercised by the workers-sdk e2e fixture; the full API
 * surface will be added by the Agent Memory team.
 */
export declare abstract class AgentMemoryContext {
  /**
   * Fetch a summary of this context.
   *
   * @returns A promise resolving to the context summary. See
   *   {@link AgentMemoryContextSummary} — the concrete shape is not yet
   *   declared and will be filled in by the Agent Memory team.
   */
  getSummary(): Promise<AgentMemoryContextSummary>;
}

/**
 * Namespace-level Agent Memory binding.
 *
 * Used as the type of an `env.MEMORY`-style binding backed by the Agent
 * Memory product. The only method known to workers-sdk today is
 * {@link AgentMemoryNamespace.getContext}; the Agent Memory team is expected
 * to extend this class with the full API.
 *
 * @example
 * ```ts
 * export default {
 *   async fetch(_request: Request, env: Env): Promise<Response> {
 *     const context = env.MEMORY.getContext("wrangler-e2e");
 *     const summary = await context.getSummary();
 *     return Response.json(summary);
 *   },
 * };
 * ```
 */
export declare abstract class AgentMemoryNamespace {
  /**
   * Get a handle to a specific Agent Memory context within this namespace.
   *
   * @param contextId The context identifier.
   * @returns An {@link AgentMemoryContext} scoped to `contextId`.
   */
  getContext(contextId: string): AgentMemoryContext;
}
