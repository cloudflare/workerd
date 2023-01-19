// NOTE: this file is a temporary placeholder to test ts/workerd integration.
// It will be rewritten/replaced with a real one eventually.

import * as bufferImpl from 'node-internal:bufferImpl';

export class Buffer {
  constructor(public readonly str: string) { }

  public toString(): string {
    return bufferImpl.toString(this);
  }
}
