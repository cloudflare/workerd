// ============================================================================
// Agent Memory
//
// Public type surface for user Workers binding to an Agent Memory namespace.
// ============================================================================

/** Memory type — every memory is classified into exactly one. */
export type AgentMemoryMemoryType = "fact" | "event" | "instruction" | "task";

/** Search intensity for recall. */
export type AgentMemoryThinkingLevel = "low" | "medium" | "high";

/** Response verbosity for recall. */
export type AgentMemoryResponseLength = "short" | "medium" | "long";

/** A conversation message passed to ingest(). */
export interface AgentMemoryMessage {
  role: "system" | "user" | "assistant";
  content: string;
  /** Optional message timestamp. */
  timestamp?: Date;
}

/** Raw memory content passed to remember(). */
export interface AgentMemoryIncomingMemory {
  /** Raw memory content. The service classifies and summarizes automatically. */
  content: string;
  /** Optional session identifier to associate with this memory. */
  sessionId?: string | null | undefined;
}

/** A stored memory returned from remember(), get(), and delete(). */
export interface AgentMemoryMemory {
  /** Memory ID. */
  id: string;
  /** Memory type. */
  type: AgentMemoryMemoryType;
  /** Text summary. */
  summary: string;
  /** Memory text. */
  content: string;
  /** Session that created this memory. */
  sessionId: string | null;
  /** Memory creation time. */
  createdAt: Date;
  /** Memory last-update time. */
  updatedAt: Date;
}

/** Single entry in a list() response. Same shape as Memory minus full content. */
export type AgentMemoryMemoryListEntry = Omit<AgentMemoryMemory, "content">;

/** A scored memory candidate in a recall result. */
export interface AgentMemoryScoredCandidate {
  /** Candidate ID. */
  id: string;
  /** Text summary. */
  summary: string;
  /** Session that created this candidate, when known. */
  sessionId: string | null;
  /** Relevance score (higher is better). Comparable only within a single query. */
  score: number;
}

/** Options for the ingest() method. */
export interface AgentMemoryIngestOptions {
  /** Session identifier to associate with memories created during ingestion. */
  sessionId?: string | null | undefined;
}

/** Options for the getSummary() method. */
export interface AgentMemoryGetSummaryOptions {
  /** Session identifier to retrieve session summary for. */
  sessionId?: string | null | undefined;
}

/** Response from the getSummary() method. */
export interface AgentMemoryGetSummaryResponse {
  /** Markdown summary. */
  summary: string;
}

/**
 * Options for the recall() method.
 *
 * `referenceDate` accepts a Date object, an ISO-8601 date string
 * (YYYY-MM-DD), or a full ISO-8601 datetime string. When provided, this
 * date is used as "today" for resolving relative time references
 * ("how many days ago", "last week") instead of the server's wall-clock time.
 */
export interface AgentMemoryRecallOptions {
  /** Recall intensity: "low" (default), "medium", or "high". */
  thinkingLevel?: AgentMemoryThinkingLevel;
  /** Response verbosity: "short", "medium" (default), or "long". */
  responseLength?: AgentMemoryResponseLength;
  /** Temporal anchor for date arithmetic. */
  referenceDate?: Date | string;
}

/** Response from the recall() method. */
export interface AgentMemoryRecallResult {
  /** Number of memories retrieved. */
  count: number;
  /** LLM-generated answer synthesizing the matching memories. */
  answer: string;
  /** Matching memories ranked by relevance. */
  candidates: AgentMemoryScoredCandidate[];
}

/**
 * Options for the list() method.
 *
 * `cursor` is the opaque continuation token returned by the previous page;
 * pass it back unchanged to fetch the next page. `sessionId` and `type`
 * are exact-match filters; combining them is allowed.
 */
export interface AgentMemoryListMemoriesOptions {
  /** Maximum number of memories to return. Default 20, max 500. */
  limit?: number;
  /** Opaque cursor from a previous page. */
  cursor?: string;
  /** Exact-match session filter. */
  sessionId?: string;
  /** Exact-match memory-type filter. */
  type?: AgentMemoryMemoryType;
}

/** Response from the list() method. */
export interface AgentMemoryListMemoriesResult {
  memories: AgentMemoryMemoryListEntry[];
  /** Continuation cursor; absent when this page exhausted the result set. */
  cursor?: string;
}

/**
 * A single Agent Memory profile, scoped to a profile name.
 *
 * Returned by {@link AgentMemoryNamespace.getProfile}.
 */
export declare abstract class AgentMemoryProfile {
  /**
   * Retrieve a memory by ID.
   *
   * @param memoryId - ULID of the memory to retrieve.
   * @throws if the memory does not exist.
   */
  get(memoryId: string): Promise<AgentMemoryMemory>;

  /**
   * Delete a memory by ID.
   *
   * Removes the memory and any source messages linked by the memory's
   * source message IDs.
   *
   * @param memoryId - ULID of the memory to delete.
   * @throws if the memory does not exist.
   */
  delete(memoryId: string): Promise<AgentMemoryMemory>;

  /**
   * Store a memory in this profile. The content is automatically classified,
   * summarized, and indexed.
   *
   * @param memory - Raw memory content to persist.
   */
  remember(memory: AgentMemoryIncomingMemory): Promise<AgentMemoryMemory>;

  /**
   * Extract memories from a conversation.
   *
   * @param messages - Conversation messages to extract memories from.
   * @param options  - Optional ingest options.
   */
  ingest(
    messages: Iterable<AgentMemoryMessage>,
    options?: AgentMemoryIngestOptions,
  ): Promise<void>;

  /**
   * Get a profile summary.
   *
   * @param options - Optional getSummary options.
   */
  getSummary(
    options?: AgentMemoryGetSummaryOptions,
  ): Promise<AgentMemoryGetSummaryResponse>;

  /**
   * Recall memories in this profile.
   *
   * @param query   - Recall query matched against memory content and keywords.
   * @param options - Optional recall parameters.
   * @returns Matching memories with relevance scores and a synthesized answer.
   */
  recall(
    query: string,
    options?: AgentMemoryRecallOptions,
  ): Promise<AgentMemoryRecallResult>;

  /**
   * List active memories in this profile.
   *
   * Returns a paginated, filterable view of stored memories. Superseded
   * versions are excluded. Use the returned `cursor` (when present) to
   * fetch the next page.
   *
   * @param options - Optional pagination and filter options.
   */
  list(
    options?: AgentMemoryListMemoriesOptions,
  ): Promise<AgentMemoryListMemoriesResult>;

  /**
   * Soft-delete every memory and message in this profile that is tagged
   * with `sessionId`.
   *
   * Idempotent: deleting a sessionId that has no rows is a no-op.
   *
   * @param sessionId - Session to delete.
   */
  deleteSession(sessionId: string): Promise<void>;
}

/**
 * Namespace-level Agent Memory binding.
 *
 * Used as the type of an `env.MEMORY`-style binding backed by the Agent
 * Memory product.
 *
 * @example
 * ```ts
 * export default {
 *   async fetch(_request: Request, env: Env): Promise<Response> {
 *     const profile = await env.MEMORY.getProfile("wrangler-e2e");
 *     const summary = await profile.getSummary();
 *     return Response.json(summary);
 *   },
 * };
 * ```
 */
export declare abstract class AgentMemoryNamespace {
  /**
   * Get a memory profile by name. Profiles are isolated by namespace and
   * addressed by a compound key (namespaceId:profileName).
   *
   * @param profileName - Profile name (validated against naming rules).
   * @returns RPC target for interacting with the profile.
   */
  getProfile(profileName: string): Promise<AgentMemoryProfile>;

  /**
   * Soft-delete a profile and schedule deferred purge. Marks all
   * memories and messages as deleted.
   *
   * @param profileName - Name of the profile to delete.
   */
  deleteProfile(profileName: string): Promise<void>;
}
