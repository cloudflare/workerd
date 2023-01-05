import * as buffer from "node:buffer";

export function toString(buf: buffer.Buffer): string {
  return `Buffer[${buf}]`;
}
