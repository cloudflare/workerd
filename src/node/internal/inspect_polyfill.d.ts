declare module "cloudflare-internal:inspect_polyfill" {
  interface InspectOptions {
    showHidden?: boolean;
    depth?: number | null;
    colors?: boolean;
    customInspect?: boolean;
    showProxy?: boolean;
    maxArrayLength?: number | null;
    maxStringLength?: number | null;
    breakLength?: number;
    compact?: boolean | number;
    sorted?: boolean | ((a: string, b: string) => number);
    getters?: "get" | "set" | boolean;
    numericSeparator?: boolean;
  }
  export function inspect(value: unknown, options: InspectOptions): string;
}
