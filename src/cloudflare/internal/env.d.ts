// Get the current environment, if any
export function getCurrent(): object | undefined;
// Arrange to have the given newEnv propagated along the async context.
export function withEnv(newEnv: unknown, fn: () => unknown): unknown;
