interface RateLimitOptions {
  key: string
  /**
   * Amount to add to the counter for this call. Must be a positive integer;
   * defaults to `1`. The call is allowed while the counter is below the limit,
   * even if `delta` takes it past the limit.
   */
  delta?: number
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
