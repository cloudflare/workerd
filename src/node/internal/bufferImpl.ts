// NOTE: this file is a temporary placeholder to test ts/workerd integration.
// It will be rewritten/replaced with a real one eventually.

import * as buffer from "node:buffer";

export function toString(buf: buffer.Buffer): string {
  return `Buffer[${buf}]`;
}
