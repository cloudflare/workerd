// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
import {
  cpSync,
  existsSync,
  mkdirSync,
  readFileSync,
  readdirSync,
  renameSync,
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

// The destination path reaches into the source through a symbolic link, so
// the lexical check on the two paths does not see that it is inside the
// source.
export const FsCpIntoSubdirectoryThroughSymlink = {
  test() {
    mkdirSync(new URL('file:///tmp/loop-src/child'), { recursive: true });
    writeFileSync(new URL('file:///tmp/loop-src/file.txt'), 'test');
    // /tmp/loop-link --> /tmp/loop-src
    symlinkSync(
      new URL('file:///tmp/loop-src'),
      new URL('file:///tmp/loop-link')
    );

    throws(
      () =>
        cpSync(
          new URL('file:///tmp/loop-src'),
          new URL('file:///tmp/loop-link/dest'),
          { recursive: true }
        ),
      {
        code: 'ERR_FS_CP_EINVAL',
        message:
          /Cannot copy '\/tmp\/loop-src' to a subdirectory of itself, '\/tmp\/loop-link\/dest'/,
      }
    );
    // The refused copy left nothing behind in the source.
    strictEqual(existsSync(new URL('file:///tmp/loop-src/dest')), false);
  },
};

// With dereference, an existing destination symlink that points into the
// source is followed, so the copy would write into the source's own subtree.
export const FsCpDereferencedDestinationInsideSource = {
  test() {
    mkdirSync(new URL('file:///tmp/deref-src/inner'), { recursive: true });
    writeFileSync(new URL('file:///tmp/deref-src/file.txt'), 'test');
    writeFileSync(new URL('file:///tmp/deref-src/inner/file.txt'), 'inner');
    // /tmp/deref-dest --> /tmp/deref-src/inner
    symlinkSync(
      new URL('file:///tmp/deref-src/inner'),
      new URL('file:///tmp/deref-dest')
    );

    throws(
      () =>
        cpSync(
          new URL('file:///tmp/deref-src'),
          new URL('file:///tmp/deref-dest'),
          { recursive: true, dereference: true }
        ),
      {
        code: 'ERR_FS_CP_EINVAL',
        message:
          /Cannot copy '\/tmp\/deref-src' to a subdirectory of itself, '\/tmp\/deref-dest'/,
      }
    );
    // The refused copy did not overwrite the source's own file.
    strictEqual(
      readFileSync(new URL('file:///tmp/deref-src/inner/file.txt'), 'utf8'),
      'inner'
    );
  },
};

// A symbolic link inside the destination points into the source's subtree,
// so the cycle only appears partway through the recursive copy.
export const FsCpNestedDestinationSymlinkIntoSource = {
  test() {
    mkdirSync(new URL('file:///tmp/nested-src/child/inner'), {
      recursive: true,
    });
    mkdirSync(new URL('file:///tmp/nested-dest'));
    // /tmp/nested-dest/child --> /tmp/nested-src/child/inner
    symlinkSync(
      new URL('file:///tmp/nested-src/child/inner'),
      new URL('file:///tmp/nested-dest/child')
    );

    throws(
      () =>
        cpSync(
          new URL('file:///tmp/nested-src'),
          new URL('file:///tmp/nested-dest'),
          { recursive: true, dereference: true }
        ),
      { code: 'ERR_FS_CP_EINVAL' }
    );
  },
};

// A symbolic link inside the destination points at a sibling of the directory
// copied through it. That writes into the source, but not into the directory
// being copied, so the copy is valid.
export const FsCpNestedDestinationSymlinkToSibling = {
  test() {
    mkdirSync(new URL('file:///tmp/sibling-src/a'), { recursive: true });
    mkdirSync(new URL('file:///tmp/sibling-src/b'));
    writeFileSync(new URL('file:///tmp/sibling-src/a/file.txt'), 'a');
    mkdirSync(new URL('file:///tmp/sibling-dest'));
    // /tmp/sibling-dest/a --> /tmp/sibling-src/b
    symlinkSync(
      new URL('file:///tmp/sibling-src/b'),
      new URL('file:///tmp/sibling-dest/a')
    );

    cpSync(
      new URL('file:///tmp/sibling-src'),
      new URL('file:///tmp/sibling-dest'),
      { recursive: true, dereference: true }
    );
    strictEqual(
      readFileSync(new URL('file:///tmp/sibling-src/b/file.txt'), 'utf8'),
      'a'
    );
    strictEqual(existsSync(new URL('file:///tmp/sibling-dest/b')), true);
  },
};

// The fs API refuses paths of more than 48 segments, but renaming one
// directory into another builds a deeper tree, and cp spent a native stack
// frame on each of its levels.
export const FsCpDeepTree = {
  test() {
    const segments = 'd/'.repeat(40);
    mkdirSync(new URL('file:///tmp/deep/tree'), { recursive: true });
    // Each round puts the tree 41 levels deeper; 60 rounds make 2,461.
    for (let i = 0; i < 60; i++) {
      mkdirSync(new URL(`file:///tmp/deep/next/${segments}`), {
        recursive: true,
      });
      renameSync(
        new URL('file:///tmp/deep/tree'),
        new URL(`file:///tmp/deep/next/${segments}tree`)
      );
      renameSync(
        new URL('file:///tmp/deep/next'),
        new URL('file:///tmp/deep/tree')
      );
    }

    throws(
      () =>
        cpSync(
          new URL('file:///tmp/deep/tree'),
          new URL('file:///tmp/deep-copy'),
          { recursive: true }
        ),
      { code: 'ENAMETOOLONG' }
    );
  },
};

// A tree as deep as a path can reach still copies.
export const FsCpDeepestPathTree = {
  test() {
    const segments = 'd/'.repeat(45);
    mkdirSync(new URL(`file:///tmp/tall/${segments}`), { recursive: true });
    writeFileSync(new URL(`file:///tmp/tall/${segments}leaf.txt`), 'leaf');

    cpSync(new URL('file:///tmp/tall'), new URL('file:///tmp/tall-copy'), {
      recursive: true,
    });
    strictEqual(
      readFileSync(
        new URL(`file:///tmp/tall-copy/${segments}leaf.txt`),
        'utf8'
      ),
      'leaf'
    );
  },
};
