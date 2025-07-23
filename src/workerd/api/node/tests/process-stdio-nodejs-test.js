import { ReadStream } from 'node:fs';
import { writeSync } from 'node:fs';
import { Buffer } from 'node:buffer';
import { Readable, Writable } from 'node:stream';
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

    assert.strictEqual(process.stdout.fd, 1, 'stdout should have fd 1');
    assert.strictEqual(
      process.stdout.readable,
      false,
      'stdout should not be readable'
    );
    assert.strictEqual(
      process.stdout._type,
      'fs',
      'stdout should have _type "fs"'
    );
    assert.strictEqual(
      process.stdout._isStdio,
      true,
      'stdout should have _isStdio true'
    );
    assert(
      process.stdout instanceof Writable,
      'stdout should be instance of Writable'
    );

    assert.strictEqual(process.stderr.fd, 2, 'stderr should have fd 2');
    assert.strictEqual(
      process.stderr.readable,
      false,
      'stderr should not be readable'
    );
    assert.strictEqual(
      process.stderr._type,
      'fs',
      'stderr should have _type "fs"'
    );
    assert.strictEqual(
      process.stderr._isStdio,
      true,
      'stderr should have _isStdio true'
    );
    assert(
      process.stderr instanceof Writable,
      'stderr should be instance of Writable'
    );
  },
};

export const processStdioWriteTest = {
  async test() {
    process.stdout.write('Test string write to stdout\n');
    process.stderr.write('Test string write to stderr\n');

    const bufferData = Buffer.from('Test buffer write\n');
    process.stdout.write(bufferData);
    process.stderr.write(bufferData);

    const uint8Array = new Uint8Array([72, 101, 108, 108, 111, 10]); // "Hello\n"
    process.stdout.write(uint8Array);
    process.stderr.write(uint8Array);

    process.stdout.write('Test UTF-8: café\n', 'utf8');
    console.error('console.error');
    console.log('console.log');
    process.stdout.write('Test base64: ', 'utf8');
    process.stdout.write('SGVsbG8gV29ybGQh', 'base64'); // "Hello World!"
    process.stdout.write('\n');

    // these get ignored!
    console.dir('test');
    console.table([
      { a: 'a', b: 'b' },
      { a: 'c', b: 'd' },
    ]);
    console.log('console.log interleaves');
    console.error('console.error interleaves');

    process.stdout.write('1');
    process.stderr.write('2');
    process.stdout.write('3');
    process.stderr.write('4');

    process.stdout.write('\uD83D');
    await new Promise((resolve) => setTimeout(resolve, 1));
    process.stdout.write('\uDE00');
    process.stderr.write('😀');

    // should buffer until the newline, supporting the partial emoji above
    process.stdout.write('\n');
    // stderr doesn't get final newline -> should still be a flush before exit
  },
};

export const processStdioCallbackTest = {
  async test() {
    await new Promise((resolve, reject) => {
      process.stdout.write('Test with callback\n', (err) => {
        if (err) {
          reject(err);
        } else {
          resolve();
        }
      });
    });

    await new Promise((resolve, reject) => {
      process.stdout.write(
        'Test with encoding and callback\n',
        'utf8',
        (err) => {
          if (err) {
            reject(err);
          } else {
            resolve();
          }
        }
      );
    });
  },
};

export const processStdioStreamMethodsTest = {
  test() {
    assert(
      typeof process.stdout.end === 'function',
      'stdout should have end method'
    );
    assert(
      typeof process.stdout.cork === 'function',
      'stdout should have cork method'
    );
    assert(
      typeof process.stdout.uncork === 'function',
      'stdout should have uncork method'
    );
    assert(
      typeof process.stdout.destroy === 'function',
      'stdout should have destroy method'
    );

    process.stdout.cork();
    process.stdout.write('Corked write 1\n');
    process.stdout.write('Corked write 2\n');
    process.stdout.uncork(); // Should flush the buffered writes
  },
};

export const processStdinTest = {
  async test() {
    assert.strictEqual(process.stdin.fd, 0, 'stdin should have fd 0');
    assert(process.stdin instanceof Readable, 'stdin should be Readable');

    let dataReceived = false;
    let endReceived = false;

    process.stdin.on('data', (chunk) => {
      dataReceived = true;
      console.log('Received data from stdin:', chunk);
    });

    await new Promise((resolve) =>
      process.stdin.on('end', () => {
        endReceived = true;
        resolve();
      })
    );

    assert.strictEqual(dataReceived, false);
    assert.strictEqual(endReceived, true);
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

export const lineBufferTruncationTest = {
  async test() {
    process.stdout.write(Buffer.alloc(4096, 'AB'));
    process.stdout.write('X');
    await new Promise((resolve) => setTimeout(resolve, 1));
    process.stdout.write('AFTER_FLUSH\n');
    process.stdout.write(Buffer.alloc(3000, 'C'));
    process.stdout.write(Buffer.alloc(2000, 'D'));
    await new Promise((resolve) => setTimeout(resolve, 1));
    process.stdout.write('END_ROLLING\n');
  },
};

export const multipleNewlinesStdoutTest = {
  test() {
    process.stdout.write('Line 1\nLine 2\nLine 3\n');
    process.stdout.write('First\n\nSecond with empty line before\n');
    process.stdout.write('Multiple trailing newlines\n\n\n');
    process.stdout.write('Start\nMiddle1\nMiddle2\nEnd');
    process.stdout.write('\n');
  },
};

export const multipleNewlinesStderrTest = {
  test() {
    process.stderr.write('Error Line 1\nError Line 2\nError Line 3\n');
    process.stderr.write('Error First\n\nError Second with empty line\n');
    process.stderr.write('Error with trailing newlines\n\n\n');
    process.stderr.write('Error Start\nError Mid1\nError Mid2\nError End');
    process.stderr.write('\n');
  },
};
