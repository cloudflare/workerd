// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import secret from "test-internal:internal-module";

export function openDoor(key) {
  if (key != secret.caveKey) throw new Error("Wrong key: " + key);
  return true;
}
