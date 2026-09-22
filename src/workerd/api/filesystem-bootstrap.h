// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/util/strong-bool.h>

namespace workerd::api {

// Whether the temporary file starts as a copy of the file's current contents
// (YES) or empty (NO). This is the spelling of FileSystemCreateWritableOptions'
// keepExistingData that the bootstrap can see; the option bag itself is read on
// the TypeScript side, which is what replaces createWritable() under the flag.
WD_STRONG_BOOL(KeepExistingData);

// Creates the native write context that backs the TypeScript
// FileSystemWritableFileStream. `fileHandle` must be a FileSystemFileHandle; the
// result exposes write/seek/truncate/commit/discard to JavaScript, see
// FileSystemWriteContextHandle in filesystem.h for their semantics.
//
// Acquires the file's VFS lock, held until the returned context is committed or
// discarded, so a context that is created must eventually be finished.
//
// Throws the same DOMException the C++ createWritable() rejects with -- a
// NotFoundError for a missing file, a TypeMismatchError for a directory or
// symlink -- and a TypeError if `fileHandle` is not a FileSystemFileHandle.
//
// This declaration deliberately mentions no filesystem types, so the per-isolate
// bootstrap can reach the factory without depending on the filesystem headers.
jsg::JsValue createFileSystemWriteContext(
    jsg::Lock& js, jsg::JsValue fileHandle, KeepExistingData keepExistingData);

}  // namespace workerd::api
