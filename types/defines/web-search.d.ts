// ============ Web Search Request Types ============

/**
 * Options for a Web Search query.
 */
export type WebSearchSearchOptions = {
  /** The search query. */
  query: string;
  /**
   * Maximum number of results to return. Defaults to 10, capped at 20.
   * The actual count may be lower if fewer matches exist.
   */
  limit?: number;
};

// ============ Web Search Response Types ============

/**
 * A single Web Search result.
 *
 * Web Search is discovery-only -- results carry catalog metadata about a page
 * but never the page body. To read a result's content the caller invokes the
 * global `fetch()` API against the result's `url`, at which point the
 * destination's own access controls apply (including Cloudflare Pay-per-Crawl).
 */
export type WebSearchResult = {
  /** Canonical URL. */
  url: string;
  /** Page title. */
  title: string;
  /** Page-level description. May be absent. */
  description?: string;
  /**
   * Last-modified date for the page, when known. Naive (no timezone)
   * ISO-8601 datetime, e.g. `"2025-11-30T04:39:48"`.
   */
  lastModifiedDate?: string;
  /**
   * Page meta image URL (typically the `og:image`). May be absent.
   */
  imageUrl?: string;
  /** Optional favicon URL for UI hints. */
  faviconUrl?: string;
};

/**
 * Per-response metadata for a Web Search query. Carries operational
 * fields useful for support and debugging.
 */
export type WebSearchResponseMetadata = {
  /** The query that was executed. */
  query: string;
  /** Opaque request identifier used for support and debugging. */
  requestId: string;
  /** End-to-end latency for this search request, in milliseconds. */
  latencyMs: number;
};

/**
 * Response from a Web Search query.
 */
export type WebSearchSearchResponse = {
  items: WebSearchResult[];
  metadata: WebSearchResponseMetadata;
};

// ============ Web Search Binding Class ============

/**
 * Cloudflare Web Search binding.
 *
 * Discovery-only primitive for agents and Workers. Returns URLs and catalog
 * metadata for a query; never returns page content or excerpts. To read a
 * result's body, fetch the URL with the global `fetch()` API.
 *
 * Declared in wrangler with a single object (there is exactly one corpus, the
 * public web, so there is no name, namespace, or instance to specify):
 *
 * ```jsonc
 * { "web_search": { "binding": "WEBSEARCH" } }
 * ```
 *
 * @example
 * ```ts
 * const { items, metadata } = await env.WEBSEARCH.search({
 *   query: "Cloudflare Workers",
 * });
 *
 * const top = items[0];
 * console.log(top.url, top.title, metadata.latencyMs);
 *
 * // Read content yourself; pay-per-crawl and other publisher
 * // controls apply at the fetch site, not at search time.
 * const page = await fetch(top.url);
 * ```
 */
export declare abstract class WebSearch {
  /**
   * Run a Web Search query.
   * @param options Search options. Only `query` is required.
   * @returns The matching results plus per-response metadata.
   */
  search(options: WebSearchSearchOptions): Promise<WebSearchSearchResponse>;
}
