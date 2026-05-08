// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// TOCTOU between buffer snapshot and ToUint32 coercion in CompressionStream::write[Sync] allowed a
// guest to resize or detach the underlying storage between the buffer snapshot and the bounds
// check, causing zlib to write into PROT_NONE pages.
import { rejects, throws } from 'node:assert';
import zlib from 'node:zlib';

const N = 1 << 16;

function createHandle() {
  return zlib.createDeflate()._handle;
}

export const regression_AUTOVULN_CLOUDFLARE_WORKERD_295_writeSync_output = {
  test() {
    const handle = createHandle();

    const input = new Uint8Array(N).fill(0x41);
    const rab = new ArrayBuffer(N, { maxByteLength: N });
    const out = new Uint8Array(rab);

    // valueOf() shrinks the resizable ArrayBuffer to 0 bytes
    // after JSG has already snapshotted the output buffer size.
    const evil = {
      valueOf() {
        rab.resize(0);
        return N;
      },
    };

    throws(
      () => {
        handle.writeSync(4, input, 0, N, out, 0, evil);
      },
      {
        name: 'Error',
        message: 'Output access is not within bounds',
      }
    );
  },
};

export const regression_AUTOVULN_CLOUDFLARE_WORKERD_295_write_output = {
  async test() {
    const handle = createHandle();

    const input = new Uint8Array(N).fill(0x41);
    const rab = new ArrayBuffer(N, { maxByteLength: N });
    const out = new Uint8Array(rab);

    const evil = {
      valueOf() {
        rab.resize(0);
        return N;
      },
    };

    await rejects(
      async () => {
        handle.write(4, input, 0, N, out, 0, evil);
      },
      {
        name: 'Error',
        message: 'Output access is not within bounds',
      }
    );
  },
};

export const regression_AUTOVULN_CLOUDFLARE_WORKERD_295_detached_input = {
  test() {
    const handle = createHandle();

    const inputBuffer = new ArrayBuffer(N);
    const input = new Uint8Array(inputBuffer).fill(0x41);
    const out = new Uint8Array(N);

    const evil = {
      valueOf() {
        structuredClone(null, { transfer: [inputBuffer] });
        return N;
      },
    };

    throws(
      () => {
        handle.writeSync(4, input, 0, N, out, 0, evil);
      },
      {
        name: 'Error',
        message: 'Input access is not within bounds',
      }
    );
  },
};

export const regression_AUTOVULN_CLOUDFLARE_WORKERD_295_writeSync_input = {
  test() {
    const handle = createHandle();

    const rab = new ArrayBuffer(N, { maxByteLength: N });
    const input = new Uint8Array(rab).fill(0x41);
    const out = new Uint8Array(N);

    const evil = {
      valueOf() {
        rab.resize(0);
        return N;
      },
    };

    throws(
      () => {
        handle.writeSync(4, input, 0, N, out, 0, evil);
      },
      {
        name: 'Error',
        message: 'Input access is not within bounds',
      }
    );
  },
};

export const regression_AUTOVULN_CLOUDFLARE_WORKERD_295_non_zero_byte_offset = {
  test() {
    const handle = createHandle();

    const offset = N;
    const input = new Uint8Array(N).fill(0x41);
    const rab = new ArrayBuffer(N + offset, { maxByteLength: N + offset });
    const out = new Uint8Array(rab, offset, N);

    const evil = {
      valueOf() {
        rab.resize(N);
        return N;
      },
    };

    throws(
      () => {
        handle.writeSync(4, input, 0, N, out, 0, evil);
      },
      {
        name: 'Error',
        message: 'Output access is not within bounds',
      }
    );
  },
};
