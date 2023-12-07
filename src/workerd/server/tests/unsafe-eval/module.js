// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { default as UnsafeEval } from "internal:unsafe-eval";

export function doEval() {
  return UnsafeEval.eval("1 + 1");
}
