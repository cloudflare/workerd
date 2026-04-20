// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as fs from 'node-internal:internal_fs_promises';
import { mkdtemp, rm } from 'node-internal:internal_fs_promises';

export * from 'node-internal:internal_fs_promises';

export interface DisposableMkdtempResult {
  path: string;
  remove(): Promise<void>;
  [Symbol.asyncDispose](): Promise<void>;
}

// Node.js fs.promises.mkdtempDisposable: mkdtemp + explicit-resource-mgmt
// wrapper that removes the directory on dispose.
export async function mkdtempDisposable(
  prefix: Parameters<typeof mkdtemp>[0],
  options?: Parameters<typeof mkdtemp>[1]
): Promise<DisposableMkdtempResult> {
  const path = await mkdtemp(prefix, options);
  let removed = false;
  const remove = async (): Promise<void> => {
    if (removed) return;
    removed = true;
    await rm(path, { recursive: true, force: true });
  };
  return {
    path,
    remove,
    [Symbol.asyncDispose]: remove,
  };
}

export default {
  ...fs,
  mkdtempDisposable,
};
