// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import * as promises from 'node-internal:internal_fs_promises';
import * as constants from 'node-internal:internal_fs_constants';
import * as syncMethods from 'node-internal:internal_fs_sync';
import * as callbackMethods from 'node-internal:internal_fs_callback';
import { WriteStream, ReadStream } from 'node-internal:internal_fs_streams';
import { Dirent, Dir } from 'node-internal:internal_fs';

export * from 'node-internal:internal_fs_callback';
export * from 'node-internal:internal_fs_sync';
export { constants, promises, Dirent, Dir, WriteStream, ReadStream };

export default {
  constants,
  promises,
  Dirent,
  Dir,
  WriteStream,
  ReadStream,
  ...syncMethods,
  ...callbackMethods,
};
