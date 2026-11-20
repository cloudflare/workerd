interface SecretsStoreSecret {
  /**
   * Get a secret from the Secrets Store, returning a string of the secret value
   * if it exists, or throws an error if it does not exist
   */
  get(): Promise<string>;

  /**
   * Get multiple secrets from the same Secrets Store in a single request.
   * Returns a record mapping secret names to their values.
   * Only secrets that are found are included in the result.
   * Throws an error if the request fails.
   */
  getMulti(secretNames: string[]): Promise<Record<string, string>>;
}
