// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  constants,
  kMaxLength,
  kStringMaxLength,
  Buffer,
  SlowBuffer,
} from 'node-internal:internal_buffer';

// eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
const atob = globalThis.atob;
// eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
const btoa = globalThis.btoa;
// eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
const Blob = globalThis.Blob;

export {
  atob,
  btoa,
  constants,
  kMaxLength,
  kStringMaxLength,
  Blob,
  Buffer,
  SlowBuffer,
};

export default {
  // eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
  atob,
  // eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
  btoa,
  constants,
  kMaxLength,
  kStringMaxLength,
  // eslint-disable-next-line @typescript-eslint/no-unsafe-assignment
  Blob,
  Buffer,
  SlowBuffer,
};
