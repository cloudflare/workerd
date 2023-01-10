import * as bufferImpl from 'node-internal:bufferImpl';

export class Buffer {
  public toString(): string {
    return bufferImpl.toString(this);
  }
}
