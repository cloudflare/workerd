// Temporarily disable linting for this file while it is a work in progress.
/* eslint-disable */
import { type TestRunnerConfig } from 'harness/harness';

const root = await navigator.storage.getDirectory();
const tmp = await root.getDirectoryHandle('tmp');
// The root is read-only, so we need to use a writable subdirectory
// to actually run the tests.
navigator.storage.getDirectory = async () => tmp;

export default {
  'FileSystemBaseHandle-buckets.https.any.js': {
    comment: 'StorageBuckets is not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-getUniqueId.https.any.js': {
    comment: '...',
    expectedFailures: [],
  },
  'FileSystemBaseHandle-IndexedDB.https.any.js': {
    comment: 'IndexedDB is not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-isSameEntry.https.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      'isSameEntry comparing two files pointing to the same path returns true',
      'isSameEntry comparing two directories pointing to the same path returns true',
      'isSameEntry comparing a file to a directory of the same path returns false',
    ],
  },
  'FileSystemBaseHandle-postMessage-BroadcastChannel.https.window.js': {
    comment: 'BroadcastChannel is not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-postMessage-Error.https.window.js': {
    comment: 'postMessage is not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-postMessage-frames.https.window.js': {
    comment: 'Frames are not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-postMessage-MessagePort-frames.https.window.js': {
    comment: 'postMessage and MessagePort are not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-postMessage-MessagePort-windows.https.window.js': {
    comment: 'postMessage and MessagePort are not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-postMessage-MessagePort-workers.https.window.js': {
    comment: 'postMessage and MessagePort are not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-postMessage-windows.https.window.js': {
    comment: 'postMessage is not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-postMessage-workers.https.window.js': {
    comment: 'postMessage is not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-remove.https.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      'remove() to remove a file',
      'remove() on an already removed file should fail',
      'remove() to remove an empty directory',
      'remove() on an already removed directory should fail',
      'remove() on a non-empty directory should fail',
      'remove() on a directory recursively should delete all sub-items',
      'remove() on a file should ignore the recursive option',
      'remove() while the file has an open writable fails',
      'can remove the root of a sandbox file system',
    ],
  },
  'FileSystemDirectoryHandle-getDirectoryHandle.https.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      'getDirectoryHandle(create=true) creates an empty directory',
      'getDirectoryHandle() with empty name',
      'getDirectoryHandle() with "." name',
      'getDirectoryHandle() with ".." name',
      'getDirectoryHandle(create=false) with a path separator when the directory exists',
      'getDirectoryHandle(create=true) with a path separator',
    ],
  },
  'FileSystemDirectoryHandle-getFileHandle.https.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      'getFileHandle() with empty name',
      'getFileHandle() with "." name',
      'getFileHandle() with ".." name',
      'getFileHandle(create=false) with a path separator when the file exists.',
      'getFileHandle(create=true) with a path separator',
    ],
  },
  'FileSystemDirectoryHandle-iteration.https.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      '@@asyncIterator: full iteration works',
      'entries: full iteration works',
      'iteration while iterator gets garbage collected',
    ],
  },
  'FileSystemDirectoryHandle-removeEntry.https.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      'removeEntry() to remove a file',
      'removeEntry() on an already removed file should fail',
      'removeEntry() to remove an empty directory',
      'removeEntry() on a non-empty directory should fail',
      'removeEntry() on a directory recursively should delete all sub-items',
      'removeEntry() with empty name should fail',
      'removeEntry() with "." name should fail',
      'removeEntry() with ".." name should fail',
      'removeEntry() with a path separator should fail.',
      'removeEntry() while the file has an open writable fails',
      'removeEntry() of a directory while a containing file has an open writable fails',
      'createWritable after removeEntry succeeds but doesnt recreate the file',
      'removeEntry() on a non-existent directory recursively should throw NotFoundError',
    ],
  },
  'FileSystemDirectoryHandle-resolve.https.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      'Resolve returns empty array for same directory',
      'Resolve returns correct path',
      'Resolve returns correct path with non-ascii characters',
      'Resolve returns null when entry is not a child',
    ],
  },
  'FileSystemFileHandle-create-sync-access-handle.https.window.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      'Attempt to create a sync access handle.',
    ],
  },
  'FileSystemFileHandle-cross-primitive-locking.https.tentative.worker.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'FileSystemFileHandle-getFile.https.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      'getFile() returns last modified time',
    ],
  },
  'FileSystemFileHandle-move.https.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      'move(name) to rename a file',
      'get a handle to a moved file',
      'move(name) to rename a file the same name',
      'move("") to rename a file fails',
      'move(name) can be called multiple times',
      'move(name) with a name with path separators should fail',
      'move(name) while the file has an open writable fails',
      'move(name) while the destination file has an open writable fails',
      'move(name) can overwrite an existing file',
      'move(dir, name) to rename a file',
      'move(dir, name) to rename a file the same name',
      'move(dir) to move a file to a new directory',
      'move(dir, "") to move a file to a new directory fails',
      'move(dir, name) to move a file to a new directory',
      'move(dir) can be called multiple times',
      'move(dir, name) can be called multiple times',
      'move(dir, name) with a name with invalid characters should fail',
      'move(dir) while the file has an open writable fails',
      'move(dir, name) while the file has an open writable fails',
      'move(dir) while the destination file has an open writable fails',
      'move(dir) can overwrite an existing file',
      'move(dir, name) while the destination file has an open writable fails',
      'move(dir, name) can overwrite an existing file',
      'FileSystemFileHandles are references, not paths',
    ],
  },
  'FileSystemFileHandle-sync-access-handle-back-forward-cache.https.tentative.window.js':
    {
      comment: '...',
      // TODO(node-fs): Inspect these tests
      disabledTests: true,
    },
  'FileSystemFileHandle-sync-access-handle-lock-modes.https.tentative.worker.js':
    {
      comment: '...',
      // TODO(node-fs): Inspect these tests
      disabledTests: true,
    },
  'FileSystemFileHandle-writable-file-stream-back-forward-cache.https.tentative.window.js':
    {
      comment: '...',
      // TODO(node-fs): Inspect these tests
      disabledTests: true,
    },
  'FileSystemFileHandle-writable-file-stream-lock-modes.https.tentative.worker.js':
    {
      comment: '...',
      // TODO(node-fs): Inspect these tests
      disabledTests: true,
    },
  'FileSystemObserver.https.tentative.any.js': {
    comment: 'FileSystemObserver is not implemented in workers',
    // TODO(node-fs): Implement FileSystemObserver?
    disabledTests: true,
  },
  'FileSystemObserver-sync-access-handle.https.tentative.worker.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'FileSystemObserver-unsupported-global.https.tentative.any.js': {
    comment: '...',
  },
  'FileSystemObserver-writable-file-stream.https.tentative.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): Fix these tests
      'Closing a FileSystemWritableFileStream that\'s modified the file produces a "modified" event',
      "All FileSystemWritableFileStream methods that aren't closed don't produce events",
    ],
  },
  'FileSystemSyncAccessHandle-close.https.worker.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'FileSystemSyncAccessHandle-flush.https.worker.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'FileSystemSyncAccessHandle-getSize.https.worker.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'FileSystemSyncAccessHandle-read-write.https.worker.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'FileSystemSyncAccessHandle-truncate.https.worker.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'FileSystemWritableFileStream.https.any.js': {
    comment: '...',
    expectedFailures: [
      'createWritable() fails when parent directory is removed',
      'createWritable() can be called on two handles representing the same file',
    ],
  },
  'FileSystemWritableFileStream-piped.https.any.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'FileSystemWritableFileStream-write.https.any.js': {
    comment: '...',
    expectedFailures: [
      'write() called with a string and a valid offset',
      'write() called with a string and a valid offset after seek',
      'write() called with a blob and a valid offset',
      'write() called with an offset beyond the end of the file',
      'atomic writes: write() after close() fails',
      'atomic writes: truncate() after close() fails',
      'getWriter() can be used',
      'WriteParams: truncate missing size param',
      'WriteParams: write missing data param',
      'WriteParams: write null data param',
      'WriteParams: seek missing position param',
      'write() with an invalid blob to an empty file should reject',
      'an errored writable stream releases its lock',
    ],
  },
  'idlharness.https.any.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'opaque-origin.https.window.js': {
    comment: 'Not relevant to workers',
    disabledTests: true,
  },
  'root-name.https.any.js': {
    comment: '...',
    expectedFailures: [
      'getDirectory returns a directory whose name is the empty string',
    ],
  },

  'script-tests/FileSystemFileHandle-getFile.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'script-tests/FileSystemFileHandle-move.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'script-tests/FileSystemObserver.js': {
    comment: 'FileSystemObserver is not implemented in workers',
    disabledTests: true,
  },
  'script-tests/FileSystemObserver-writable-file-stream.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'script-tests/FileSystemSyncAccessHandle-flush.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'script-tests/FileSystemWritableFileStream.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'script-tests/FileSystemWritableFileStream-piped.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'script-tests/FileSystemWritableFileStream-write.js': {
    comment: '...',
    // TODO(node-fs): Inspect these tests
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-buckets.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-getUniqueId.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-IndexedDB.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-isSameEntry.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-BroadcastChannel.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-Error.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-frames.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-MessagePort-frames.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-MessagePort-windows.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-MessagePort-workers.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-windows.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-workers.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-remove.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemDirectoryHandle-getDirectoryHandle.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemDirectoryHandle-getFileHandle.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemDirectoryHandle-iteration.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemDirectoryHandle-removeEntry.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemDirectoryHandle-resolve.js': {
    comment: '...',
    disabledTests: true,
  },
  'script-tests/FileSystemFileHandle-create-sync-access-handle.js': {
    comment: '...',
    disabledTests: true,
  },
} satisfies TestRunnerConfig;
