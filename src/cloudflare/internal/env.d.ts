// Get the current environment, if any
export function getCurrent(): object | undefined;
export function withEnv(newEnv: unknown, fn: () => unknown): unknown;
