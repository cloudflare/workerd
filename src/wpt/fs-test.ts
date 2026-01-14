// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import type { TestRunnerConfig } from 'harness/harness'

const root = await navigator.storage.getDirectory()
const tmp = await root.getDirectoryHandle('tmp')
// The root is read-only, so we need to use a writable subdirectory
// to actually run the tests.
navigator.storage.getDirectory =
  async (): Promise<FileSystemDirectoryHandle> => {
    // Let's create a new random tmp subdirectory for each test run to avoid interference
    return tmp.getDirectoryHandle(crypto.randomUUID(), { create: true })
  }

export default {
  'FileSystemBaseHandle-IndexedDB.https.any.js': {
    comment: 'IndexedDB is not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-buckets.https.any.js': {
    comment: 'StorageBuckets is not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-getUniqueId.https.any.js': {
    comment: '...',
    expectedFailures: [],
  },
  'FileSystemBaseHandle-isSameEntry.https.any.js': {
    comment: '...',
    expectedFailures: [],
  },
  'FileSystemBaseHandle-postMessage-BroadcastChannel.https.window.js': {
    comment: 'BroadcastChannel is not implemented in workers',
    disabledTests: true,
  },
  'FileSystemBaseHandle-postMessage-Error.https.window.js': {
    comment: 'postMessage is not implemented in workers',
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
  'FileSystemBaseHandle-postMessage-frames.https.window.js': {
    comment: 'Frames are not implemented in workers',
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
    disabledTests: [
      // Intentionally unsupported
      'can remove the root of a sandbox file system',
    ],
  },
  'FileSystemDirectoryHandle-getDirectoryHandle.https.any.js': {
    comment: '...',
    expectedFailures: [],
  },
  'FileSystemDirectoryHandle-getFileHandle.https.any.js': {
    comment: '...',
    expectedFailures: [],
  },
  'FileSystemDirectoryHandle-iteration.https.any.js': {
    comment: '...',
    expectedFailures: [],
  },
  'FileSystemDirectoryHandle-removeEntry.https.any.js': {
    comment: '...',
    expectedFailures: [],
  },
  'FileSystemDirectoryHandle-resolve.https.any.js': {
    comment:
      'We currently do not implement the resolve() method on directories',
    disabledTests: [
      'Resolve returns empty array for same directory',
      'Resolve returns correct path',
      'Resolve returns correct path with non-ascii characters',
      'Resolve returns null when entry is not a child',
    ],
  },
  'FileSystemFileHandle-create-sync-access-handle.https.window.js': {
    comment: 'We do not yet implement sync access handles',
    disabledTests: ['Attempt to create a sync access handle.'],
  },
  'FileSystemFileHandle-getFile.https.any.js': {
    comment: '...',
    expectedFailures: [
      // TODO(node-fs): We currently do not implement time stamp modification
      // of files via webfs so this test is expected to fail. This is temporary
      // until we fully implement these bits, so leaving this as "expectedFailures".
      'getFile() returns last modified time',
    ],
  },
  'FileSystemFileHandle-move.https.any.js': {
    comment:
      'We do not currently implement the move() method on file system handles',
    expectedFailures: [
      // TODO(node-fs): Implement move and enable these tests
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
  'FileSystemSyncAccessHandle-close.https.worker.js': {
    comment: 'We currently do not implement sync access handles',
    disabledTests: true,
  },
  'FileSystemSyncAccessHandle-flush.https.worker.js': {
    comment: 'We currently do not implement sync access handles',
    disabledTests: true,
  },
  'FileSystemSyncAccessHandle-getSize.https.worker.js': {
    comment: 'We currently do not implement sync access handles',
    disabledTests: true,
  },
  'FileSystemSyncAccessHandle-read-write.https.worker.js': {
    comment: 'We currently do not implement sync access handles',
    disabledTests: true,
  },
  'FileSystemSyncAccessHandle-truncate.https.worker.js': {
    comment: 'We currently do not implement sync access handles',
    disabledTests: true,
  },
  'FileSystemWritableFileStream-piped.https.any.js': {
    comment: 'The test requires streams/resources/recording-streams.js',
    // TODO(node-fs): These tests require a resource file from the streams wpt
    // tests...
    disabledTests: true,
  },
  'FileSystemWritableFileStream-write.https.any.js': {
    comment: '...',
    disabledTests: [
      // We don't support this case. In the test a Blob is created from a
      // file that is then removed, then that Blob is written to a file.
      // Because our blob contains a copy of the original file's data
      // and is not a live-reference, the expected error does not occur.
      'write() with an invalid blob to an empty file should reject',
    ],
  },
  'FileSystemWritableFileStream.https.any.js': {
    comment:
      'There is currently a bug in the web platform tests that causes this to fail',
    expectedFailures: [
      'createWritable() can be called on two handles representing the same file',
    ],
  },
  'idlharness.https.any.js': {
    comment:
      'Workers expose globals differently than browsers - these interface tests fail',
    expectedFailures: [
      'FileSystemHandle interface: existence and properties of interface object',
      'FileSystemHandle interface object length',
      'FileSystemHandle interface object name',
      'FileSystemHandle interface: existence and properties of interface prototype object',
      'FileSystemHandle interface: existence and properties of interface prototype object\'s "constructor" property',
      "FileSystemHandle interface: existence and properties of interface prototype object's @@unscopables property",
      'FileSystemHandle interface: attribute kind',
      'FileSystemHandle interface: attribute name',
      'FileSystemHandle interface: operation isSameEntry(FileSystemHandle)',
      'FileSystemFileHandle interface: existence and properties of interface object',
      'FileSystemFileHandle interface object length',
      'FileSystemFileHandle interface object name',
      'FileSystemFileHandle interface: existence and properties of interface prototype object',
      'FileSystemFileHandle interface: existence and properties of interface prototype object\'s "constructor" property',
      "FileSystemFileHandle interface: existence and properties of interface prototype object's @@unscopables property",
      'FileSystemFileHandle interface: operation getFile()',
      'FileSystemFileHandle interface: operation createWritable(optional FileSystemCreateWritableOptions)',
      'FileSystemDirectoryHandle interface: existence and properties of interface object',
      'FileSystemDirectoryHandle interface object length',
      'FileSystemDirectoryHandle interface object name',
      'FileSystemDirectoryHandle interface: existence and properties of interface prototype object',
      'FileSystemDirectoryHandle interface: existence and properties of interface prototype object\'s "constructor" property',
      "FileSystemDirectoryHandle interface: existence and properties of interface prototype object's @@unscopables property",
      'FileSystemDirectoryHandle interface: async iterable<USVString, FileSystemHandle>',
      'FileSystemDirectoryHandle interface: operation getFileHandle(USVString, optional FileSystemGetFileOptions)',
      'FileSystemDirectoryHandle interface: operation getDirectoryHandle(USVString, optional FileSystemGetDirectoryOptions)',
      'FileSystemDirectoryHandle interface: operation removeEntry(USVString, optional FileSystemRemoveOptions)',
      'FileSystemDirectoryHandle interface: operation resolve(FileSystemHandle)',
      'FileSystemWritableFileStream interface: existence and properties of interface object',
      'FileSystemWritableFileStream interface object length',
      'FileSystemWritableFileStream interface object name',
      'FileSystemWritableFileStream interface: existence and properties of interface prototype object',
      'FileSystemWritableFileStream interface: existence and properties of interface prototype object\'s "constructor" property',
      "FileSystemWritableFileStream interface: existence and properties of interface prototype object's @@unscopables property",
      'FileSystemWritableFileStream interface: operation write(FileSystemWriteChunkType)',
      'FileSystemWritableFileStream interface: operation seek(unsigned long long)',
      'FileSystemWritableFileStream interface: operation truncate(unsigned long long)',
      'StorageManager interface: operation getDirectory()',
    ],
  },
  'opaque-origin.https.window.js': {
    comment: 'Not relevant to workers',
    disabledTests: true,
  },
  'root-name.https.any.js': {
    comment:
      'Our test harness does not actually expose the root directory to the tests',
    disabledTests: [
      'getDirectory returns a directory whose name is the empty string',
    ],
  },

  'script-tests/FileSystemBaseHandle-IndexedDB.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-buckets.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-getUniqueId.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-isSameEntry.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-BroadcastChannel.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-Error.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-MessagePort-frames.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-MessagePort-windows.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-MessagePort-workers.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-frames.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-windows.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-postMessage-workers.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemBaseHandle-remove.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemDirectoryHandle-getDirectoryHandle.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemDirectoryHandle-getFileHandle.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemDirectoryHandle-iteration.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemDirectoryHandle-removeEntry.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemDirectoryHandle-resolve.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemFileHandle-create-sync-access-handle.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemFileHandle-getFile.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemFileHandle-move.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemObserver-writable-file-stream.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemObserver.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemSyncAccessHandle-flush.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemWritableFileStream-piped.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemWritableFileStream-write.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
  'script-tests/FileSystemWritableFileStream.js': {
    comment: 'script-test files are imported and tested above',
    disabledTests: true,
  },
} satisfies TestRunnerConfig
