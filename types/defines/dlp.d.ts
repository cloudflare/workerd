// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/** The HTTP request in which DLP found a match. */
interface DlpValidationRequestInfo {
  method: string;
  url: string;
}

/** A DLP match for a validation Worker to score. */
interface DlpValidationInput {
  entryId: string;
  accountId: string;
  userId?: string;
  /** Start index of the DLP match in `excerpt`. */
  start: number;
  /** End index of the DLP match in `excerpt`. */
  end: number;
  /** Base64-encoded excerpt containing the match and surrounding context. */
  excerpt: string;
  requestInfo: DlpValidationRequestInfo;
}

/**
 * A confidence score from -1 to 1, or `"veto"` to reject the match. A score of 0 does not affect
 * confidence; positive scores increase it and negative scores decrease it.
 */
type DlpValidationVerdict = number | 'veto';

interface DlpValidationResult {
  score: DlpValidationVerdict;
}

/**
 * The contract of a DLP validation entrypoint. Implement it on a `WorkerEntrypoint` and select
 * that export as the validator in the DLP profile.
 *
 * @example
 * ```ts
 * import { WorkerEntrypoint } from "cloudflare:workers";
 *
 * export class TrustedDomains extends WorkerEntrypoint implements DlpValidation {
 *   async validate(input: DlpValidationInput): Promise<DlpValidationResult> {
 *     const hostname = new URL(input.requestInfo.url).hostname;
 *     if (await this.env.TRUSTED_DOMAINS.get(hostname)) {
 *       return { score: "veto" };
 *     }
 *     return { score: 0 };
 *   }
 * }
 * ```
 */
interface DlpValidation {
  validate(
    input: DlpValidationInput
  ): DlpValidationResult | Promise<DlpValidationResult>;
}
