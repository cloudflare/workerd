// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { existsSync } from "node:fs";
import path from "node:path";

// When building types from the upstream repo all paths need to be prepended by
// external/+dep_workerd+workerd/
export function getFilePath(f: string): string {
  if (existsSync("external/+dep_workerd+workerd")) {
    return path.join("external", "+dep_workerd+workerd", f);
  } else {
    return f;
  }
}
