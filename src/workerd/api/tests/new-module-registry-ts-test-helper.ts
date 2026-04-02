// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
// A simple TypeScript module with type annotations that will be stripped
// by the transpiler. This exercises the code path where EsModule source
// is owned by a rust::String from the transpiler output.

export function add(a: number, b: number): number {
  return a + b;
}

export function greet(name: string): string {
  return `hello, ${name}`;
}

export const PI: number = 3.14159;
