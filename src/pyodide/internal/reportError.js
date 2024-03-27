export function reportError(e) {
  e.stack.split("\n").forEach((s) => console.warn(s));
  throw e;
}

