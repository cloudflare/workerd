/**
 * Evaluation context for targeting rules.
 * Keys are attribute names (e.g. "userId", "country"), values are the attribute values.
 */
export type EvaluationContext = Record<string, string | number | boolean>;

export interface EvaluationDetails<T> {
  flagKey: string;
  value: T;
  variant?: string | undefined;
  reason?: string | undefined;
  errorCode?: string | undefined;
  errorMessage?: string | undefined;
}

export interface FlagEvaluationError extends Error {}

/**
 * Feature flags binding for evaluating feature flags from a Cloudflare Workers script.
 *
 * @example
 * ```typescript
 * // Get a boolean flag value with a default
 * const enabled = await env.FLAGS.getBooleanValue('my-feature', false);
 *
 * // Get a flag value with evaluation context for targeting
 * const variant = await env.FLAGS.getStringValue('experiment', 'control', {
 *   userId: 'user-123',
 *   country: 'US',
 * });
 *
 * // Get full evaluation details including variant and reason
 * const details = await env.FLAGS.getBooleanDetails('my-feature', false);
 * console.log(details.variant, details.reason);
 * ```
 */
export declare abstract class Flags {
  /**
   * Get a flag value without type checking.
   * @param flagKey The key of the flag to evaluate.
   * @param defaultValue Optional default value returned when evaluation fails.
   * @param context Optional evaluation context for targeting rules.
   */
  get(
    flagKey: string,
    defaultValue?: unknown,
    context?: EvaluationContext
  ): Promise<unknown>;

  /**
   * Get a boolean flag value.
   * @param flagKey The key of the flag to evaluate.
   * @param defaultValue Default value returned when evaluation fails or the flag type does not match.
   * @param context Optional evaluation context for targeting rules.
   */
  getBooleanValue(
    flagKey: string,
    defaultValue: boolean,
    context?: EvaluationContext
  ): Promise<boolean>;

  /**
   * Get a string flag value.
   * @param flagKey The key of the flag to evaluate.
   * @param defaultValue Default value returned when evaluation fails or the flag type does not match.
   * @param context Optional evaluation context for targeting rules.
   */
  getStringValue(
    flagKey: string,
    defaultValue: string,
    context?: EvaluationContext
  ): Promise<string>;

  /**
   * Get a number flag value.
   * @param flagKey The key of the flag to evaluate.
   * @param defaultValue Default value returned when evaluation fails or the flag type does not match.
   * @param context Optional evaluation context for targeting rules.
   */
  getNumberValue(
    flagKey: string,
    defaultValue: number,
    context?: EvaluationContext
  ): Promise<number>;

  /**
   * Get an object flag value.
   * @param flagKey The key of the flag to evaluate.
   * @param defaultValue Default value returned when evaluation fails or the flag type does not match.
   * @param context Optional evaluation context for targeting rules.
   */
  getObjectValue<T extends object>(
    flagKey: string,
    defaultValue: T,
    context?: EvaluationContext
  ): Promise<T>;

  /**
   * Get a boolean flag value with full evaluation details.
   * @param flagKey The key of the flag to evaluate.
   * @param defaultValue Default value returned when evaluation fails or the flag type does not match.
   * @param context Optional evaluation context for targeting rules.
   */
  getBooleanDetails(
    flagKey: string,
    defaultValue: boolean,
    context?: EvaluationContext
  ): Promise<EvaluationDetails<boolean>>;

  /**
   * Get a string flag value with full evaluation details.
   * @param flagKey The key of the flag to evaluate.
   * @param defaultValue Default value returned when evaluation fails or the flag type does not match.
   * @param context Optional evaluation context for targeting rules.
   */
  getStringDetails(
    flagKey: string,
    defaultValue: string,
    context?: EvaluationContext
  ): Promise<EvaluationDetails<string>>;

  /**
   * Get a number flag value with full evaluation details.
   * @param flagKey The key of the flag to evaluate.
   * @param defaultValue Default value returned when evaluation fails or the flag type does not match.
   * @param context Optional evaluation context for targeting rules.
   */
  getNumberDetails(
    flagKey: string,
    defaultValue: number,
    context?: EvaluationContext
  ): Promise<EvaluationDetails<number>>;

  /**
   * Get an object flag value with full evaluation details.
   * @param flagKey The key of the flag to evaluate.
   * @param defaultValue Default value returned when evaluation fails or the flag type does not match.
   * @param context Optional evaluation context for targeting rules.
   */
  getObjectDetails<T extends object>(
    flagKey: string,
    defaultValue: T,
    context?: EvaluationContext
  ): Promise<EvaluationDetails<T>>;
}

export {
  Flags as Flagship,
  FlagEvaluationError as FlagshipEvaluationError,
  EvaluationContext as FlagshipEvaluationContext,
  EvaluationDetails as FlagshipEvaluationDetails,
};
