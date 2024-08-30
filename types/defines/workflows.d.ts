/**
 * NonRetryableError allows for a Workflow to throw a "fatal" error as in,
 * an error that makes the instance fail immediately without triggering a retry.
 */
export class NonRetryableError extends Error {
  // __brand has been explicity omitted because it's a internal brand used for
  // the Workflows' engine and user's shouldn't be able to override it
  // (at least, in a direct way)

  public constructor(message: string, name?: string);
}
