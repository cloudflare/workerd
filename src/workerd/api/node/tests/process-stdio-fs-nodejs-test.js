import { writeSync, ReadStream } from 'node:fs';
import { Buffer } from 'node:buffer';
import { Readable } from 'node:stream';
import assert from 'node:assert';

export const processStdioPropertiesTest = {
  test() {
    assert.strictEqual(process.stdin.fd, 0, 'stdin should have fd 0');
    assert(
      process.stdin instanceof Readable,
      'stdin should be instance of Readable'
    );
    assert(
      process.stdin instanceof ReadStream,
      'stdin should be instance of ReadStream'
    );
  },
};

export const fdBasedOperationsTest = {
  test() {
    const message = 'Direct writeSync to stdout\n';
    const buffer = Buffer.from(message);
    const bytesWritten = writeSync(1, buffer);
    assert.strictEqual(
      bytesWritten,
      buffer.length,
      'writeSync should return correct byte count'
    );

    const errorMessage = 'Direct writeSync to stderr\n';
    const errorBuffer = Buffer.from(errorMessage);
    const errorBytesWritten = writeSync(2, errorBuffer);
    assert.strictEqual(
      errorBytesWritten,
      errorBuffer.length,
      'writeSync to stderr should return correct byte count'
    );
  },
};

export const largeWriteTruncationTest = {
  test() {
    const largeBuffer = Buffer.alloc(4001 * 5, 'A'.repeat(4000) + '\n');
    largeBuffer[0] = 66;
    largeBuffer[4001 * 5 - 1] = 67;
    const bytesWritten = writeSync(1, largeBuffer);

    assert.strictEqual(
      bytesWritten,
      16 * 1024,
      'Direct fd write should be truncated to 16KiB'
    );
  },
};
