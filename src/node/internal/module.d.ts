// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

export function createRequire(path: string): (specifier: string) => unknown;
export function isBuiltin(specifier: string): boolean;
