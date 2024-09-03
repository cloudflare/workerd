export class NonRetryableError extends Error {
  // `__brand` is needed for Workflows' engine to validate if the user returned a NonRetryableError
  // this provides better DX because they can just extend NonRetryableError for their own Errors
  // and override name.
  // This needs to be a public field because it's serialized over RPC to the Workflows' engine
  public readonly __brand: string = 'NonRetryableError';

  public constructor(message: string, name = 'NonRetryableError') {
    super(message);
    this.name = name;
  }
}
