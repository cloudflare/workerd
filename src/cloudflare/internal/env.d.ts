// Get the current environment, if any
export function getCurrent(): Record<string, unknown> | undefined;
export function withEnv(newEnv: unknown, fn: () => unknown): unknown;
