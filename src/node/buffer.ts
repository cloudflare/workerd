// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import {
  constants,
  kMaxLength,
  kStringMaxLength,
  Buffer,
  SlowBuffer,
  isAscii,
  isUtf8,
  transcode,
} from 'node-internal:internal_buffer';

const atob = globalThis.atob;
const btoa = globalThis.btoa;
const Blob = globalThis.Blob;
const File = globalThis.File;

export {
  atob,
  btoa,
  constants,
  kMaxLength,
  kStringMaxLength,
  Blob,
  Buffer,
  File,
  SlowBuffer,
  isAscii,
  isUtf8,
  transcode,
};

export default {
  atob,
  btoa,
  constants,
  kMaxLength,
  kStringMaxLength,
  Blob,
  Buffer,
  File,
  SlowBuffer,
  isAscii,
  isUtf8,
  transcode,
};
