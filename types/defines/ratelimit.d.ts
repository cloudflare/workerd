interface RateLimitOptions {
  key: string
}

interface RateLimitOutcome {
  success: boolean
}

interface RateLimit {
  /**
   * Rate limit a request based on the provided options.
   * @see https://developers.cloudflare.com/workers/runtime-apis/bindings/rate-limit/
   * @returns A promise that resolves with the outcome of the rate limit.
   */
  limit(options: RateLimitOptions): Promise<RateLimitOutcome>;
}
