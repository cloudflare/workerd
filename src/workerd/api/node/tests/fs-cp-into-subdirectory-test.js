// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
  cpSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  symlinkSync,
  writeFileSync,
} from 'node:fs';
import { strictEqual, throws } from 'node:assert';

export const FsCpIntoSubdirectory = {
  test() {
    mkdirSync(new URL('file:///tmp/src'), { recursive: true });
    writeFileSync(new URL('file:///tmp/src/file.txt'), 'test');

    throws(
      () =>
        cpSync(new URL('file:///tmp/src'), new URL('file:///tmp/src/dest'), {
          recursive: true,
        }),
      {
        code: 'ERR_FS_CP_EINVAL',
      }
    );
  },
};

export const FsCpDestinationSymlinkToSource = {
  test() {
    const src = new URL('file:///tmp/src');
    const dest = new URL('file:///tmp/dest');

    mkdirSync(new URL('file:///tmp/src/child'), { recursive: true });
    writeFileSync(new URL('file:///tmp/src/child/payload.txt'), 'payload');
    writeFileSync(new URL('file:///tmp/src/a.txt'), 'a');
    strictEqual(readdirSync(src).length, 2);

    mkdirSync(dest);
    // /tmp/dest/child --> /tmp/src
    symlinkSync(src, new URL('file:///tmp/dest/child'));

    cpSync(src, dest, { recursive: true, dereference: true });
    strictEqual(readFileSync(new URL('file:///tmp/dest/a.txt'), 'utf8'), 'a');
    strictEqual(
      readFileSync(new URL('file:///tmp/src/payload.txt'), 'utf8'),
      'payload'
    );
    strictEqual(
      readFileSync(new URL('file:///tmp/src/child/payload.txt'), 'utf8'),
      'payload'
    );
  },
};
