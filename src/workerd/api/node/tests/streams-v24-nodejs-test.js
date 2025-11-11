// Test for Node.js v24 streams compatibility
// These tests specifically verify the v24 behavior changes work correctly

import { Readable, Writable } from 'node:stream';
import { strictEqual } from 'node:assert';

// Regression test for v24 compat: large stream piping with backpressure
// This tests that needDrain is calculated AFTER write() completes, not before.
//
// Bug context: When needDrain was calculated before _write(), synchronous writes that modified
// state.length would cause the backpressure flag to be set based on stale buffer state. This
// created a deadlock where the stream would never emit 'drain', causing pipes to hang.
//
// Symptoms if broken:
// - Test will timeout (stream never completes)
// - totalWritten will be stuck at approximately 2.3MB (around the buffer threshold)
// - The stream hangs waiting for a 'drain' event that never fires
// - Worker will show "code had hung and would never generate a response" error
export const testLargeStreamPipeBackpressureV24 = {
  async test() {
    const { promise, resolve, reject } = Promise.withResolvers();

    // Create a 6MB buffer to ensure we exceed highWaterMark multiple times
    const chunkSize = 64 * 1024; // 64KB chunks
    const numChunks = 100; // ~6.4MB total
    const expectedTotal = chunkSize * numChunks;

    let totalWritten = 0;
    let writesCompleted = 0;

    const readable = new Readable({
      read() {
        // Push data in chunks
        if (writesCompleted < numChunks) {
          const chunk = Buffer.alloc(chunkSize, writesCompleted % 256);
          this.push(chunk);
          writesCompleted++;
        } else {
          this.push(null); // End the stream
        }
      },
    });

    const writable = new Writable({
      // Set highWaterMark low to trigger backpressure
      highWaterMark: 128 * 1024, // 128KB
      write(chunk, encoding, callback) {
        totalWritten += chunk.length;
        // Simulate async write to ensure backpressure logic is exercised
        setImmediate(callback);
      },
    });

    writable.on('finish', () => {
      try {
        strictEqual(
          totalWritten,
          expectedTotal,
          `Expected ${expectedTotal} bytes but got ${totalWritten}`
        );
        resolve();
      } catch (err) {
        reject(err);
      }
    });

    writable.on('error', reject);
    readable.on('error', reject);

    // Pipe with backpressure handling
    readable.pipe(writable);

    // Add timeout to catch the hang condition if the bug regresses
    const timeout = setTimeout(() => {
      reject(
        new Error(
          `Stream hung after ${totalWritten} bytes (expected ${expectedTotal}). ` +
            `This indicates needDrain backpressure calculation is broken.`
        )
      );
    }, 10000);

    try {
      await promise;
    } finally {
      clearTimeout(timeout);
    }
  },
};
