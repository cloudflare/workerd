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
