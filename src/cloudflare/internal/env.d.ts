// Get the current environment, if any
export function getCurrent(): object | undefined;
export function withEnv(newEnv: any, fn: () => any): any;
