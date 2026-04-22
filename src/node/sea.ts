// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Single Executable Applications API. Workerd is never a SEA, so this module
// provides stub implementations that return "no SEA" sentinels. Asset lookup
// methods throw because there are never any assets to look up.

export function isSea(): boolean {
  return false;
}

export function getAssetKeys(): string[] {
  return [];
}

export function getAsset(
  key: string,
  _encoding?: string
): ArrayBuffer | string {
  throw new Error(`Asset "${key}" not found (workerd is not a SEA)`);
}

export function getAssetAsBlob(
  key: string,
  _options?: { type?: string }
): Blob {
  throw new Error(`Asset "${key}" not found (workerd is not a SEA)`);
}

export function getRawAsset(key: string): ArrayBuffer {
  throw new Error(`Asset "${key}" not found (workerd is not a SEA)`);
}

export default {
  isSea,
  getAssetKeys,
  getAsset,
  getAssetAsBlob,
  getRawAsset,
};
