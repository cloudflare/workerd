import { existsSync } from "node:fs";
import path from "node:path";

// When building types from the upstream repo all paths need to be prepended by external/workerd/
export function getFilePath(f: string): string {
  if (existsSync("external/workerd")) {
    return path.join("external", "workerd", f);
  } else {
    return f;
  }
}
