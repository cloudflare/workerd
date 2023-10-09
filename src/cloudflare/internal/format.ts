import util from "cloudflare-internal:inspect_polyfill";

export function formatArgs(...args: unknown[]): string {
  const inspectOptions = { colors: args.pop() };

  // TODO: call `util.formatWithOptions(inspectOptions, ...args)` here
  const stringified: string[] = [];
  for (const arg of args) {
    if (typeof arg === "string") {
      stringified.push(arg);
    } else {
      // eslint-disable-next-line @typescript-eslint/no-unsafe-member-access, @typescript-eslint/no-unsafe-call
      stringified.push(util.inspect(arg, inspectOptions) as string);
    }
  }
  return stringified.join(" ");
}
