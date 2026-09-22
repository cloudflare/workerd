// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The capnp::SchemaFile implementations behind compiling a workerd config written as a Cap'n
// Proto schema file: files on disk (with import resolution and --watch registration) and the
// schemas built into the binary.

#include <kj-rs-io/async-io.h>

#include <capnp/schema-parser.h>
#include <kj/filesystem.h>

namespace workerd::server {

// The schemas built into the binary, by their import path: "/capnp/c++.capnp" and
// "/workerd/workerd.capnp". None for any other path.
kj::Maybe<kj::Own<capnp::SchemaFile>> tryImportBulitin(kj::StringPtr name);

// Callbacks for capnp::SchemaFileLoader. Implementing this interface lets us control import
// resolution, which we want to do mainly so that we can set watches on all imported files.
//
// These callbacks also give us more control over error reporting, in particular the ability
// to not throw an exception on the first error seen.
class SchemaFileImpl final: public capnp::SchemaFile {
 public:
  class ErrorReporter {
   public:
    virtual void reportParsingError(
        kj::StringPtr file, SourcePos start, SourcePos end, kj::StringPtr message) = 0;
  };

  SchemaFileImpl(const kj::Directory& root,
      kj::PathPtr current,
      kj::Path fullPathParam,
      kj::PathPtr basePath,
      kj::ArrayPtr<const kj::Path> importPath,
      kj::Own<const kj::ReadableFile> fileParam,
      kj::Maybe<kj_rs_io::FileWatcher&> watcher,
      ErrorReporter& errorReporter)
      : root(root),
        current(current),
        fullPath(kj::mv(fullPathParam)),
        basePath(basePath),
        importPath(importPath),
        file(kj::mv(fileParam)),
        watcher(watcher),
        errorReporter(errorReporter) {
    if (fullPath.startsWith(current)) {
      // Simplify display name by removing current directory prefix.
      displayName = fullPath.slice(current.size(), fullPath.size()).toNativeString();
    } else {
      // Use full path.
      displayName = fullPath.toNativeString(true);
    }

    KJ_IF_SOME(w, watcher) {
      w.watch(fullPath);
    }
  }

  kj::StringPtr getDisplayName() const override {
    return displayName;
  }

  kj::Array<const char> readContent() const override {
    uint64_t size = file->stat().size;
    if (!size) {
      return nullptr;
    }
    return file->mmap(0, file->stat().size).releaseAsChars();
  }

  kj::Maybe<kj::Own<SchemaFile>> import(kj::StringPtr target) const override {
    if (target.startsWith("/")) {
      auto parsedPath = kj::Path::parse(target.slice(1));
      for (auto& candidate: importPath) {
        auto newFullPath = candidate.append(parsedPath);

        KJ_IF_SOME(newFile, root.tryOpenFile(newFullPath)) {
          return kj::implicitCast<kj::Own<SchemaFile>>(kj::heap<SchemaFileImpl>(root, current,
              kj::mv(newFullPath), candidate, importPath, kj::mv(newFile), watcher, errorReporter));
        }
      }
      // No matching file found. Check if we have a builtin.
      return tryImportBulitin(target);
    } else {
      auto relativeTo = fullPath.slice(basePath.size(), fullPath.size());
      auto parsed = relativeTo.parent().eval(target);
      auto newFullPath = basePath.append(parsed);

      KJ_IF_SOME(newFile, root.tryOpenFile(newFullPath)) {
        return kj::implicitCast<kj::Own<SchemaFile>>(kj::heap<SchemaFileImpl>(root, current,
            kj::mv(newFullPath), basePath, importPath, kj::mv(newFile), watcher, errorReporter));
      } else {
        return kj::none;
      }
    }
  }

  bool operator==(const SchemaFile& other) const override {
    if (auto downcasted = dynamic_cast<const SchemaFileImpl*>(&other)) {
      return fullPath == downcasted->fullPath;
    } else {
      return false;
    }
  }

  size_t hashCode() const override {
    return kj::hashCode(fullPath);
  }

  void reportError(SourcePos start, SourcePos end, kj::StringPtr message) const override {
    errorReporter.reportParsingError(displayName, start, end, message);
  }

 private:
  const kj::Directory& root;
  kj::PathPtr current;

  // Full path from root of filesystem to the file.
  kj::Path fullPath;

  // If this file was reached by scanning `importPath`, `basePath` is the particular import path
  // directory that was used, otherwise it is empty. `basePath` is always a prefix of `fullPath`.
  kj::PathPtr basePath;

  // Paths to search for absolute imports.
  kj::ArrayPtr<const kj::Path> importPath;

  kj::Own<const kj::ReadableFile> file;
  kj::String displayName;

  // Mutable because the SchemaParser interface forces us to make all our methods `const` so that
  // parsing can happen on multiple threads, but we do not actually use multiple threads for
  // parsing, so we're good.
  mutable kj::Maybe<kj_rs_io::FileWatcher&> watcher;

  ErrorReporter& errorReporter;
};

// A schema file whose text is embedded into the binary for convenience.
//
// TODO(someday): Could `capnp::SchemaParser` be updated such that it can use the compiled-in
//   schema nodes rather than re-parse the file from scratch? This is tricky as some information
//   is lost after compilation which is needed to compile dependents, e.g. aliases are erased.
class BuiltinSchemaFileImpl final: public capnp::SchemaFile {
 public:
  BuiltinSchemaFileImpl(kj::StringPtr name, kj::StringPtr content): name(name), content(content) {}

  kj::StringPtr getDisplayName() const override {
    return name;
  }

  kj::Array<const char> readContent() const override {
    return kj::Array<const char>(content.begin(), content.size(), kj::NullArrayDisposer::instance);
  }

  kj::Maybe<kj::Own<SchemaFile>> import(kj::StringPtr target) const override {
    return tryImportBulitin(target);
  }

  bool operator==(const SchemaFile& other) const override {
    if (auto downcasted = dynamic_cast<const BuiltinSchemaFileImpl*>(&other)) {
      return downcasted->name == name;
    } else {
      return false;
    }
  }

  size_t hashCode() const override {
    return kj::hashCode(name);
  }

  void reportError(SourcePos start, SourcePos end, kj::StringPtr message) const override {
    KJ_FAIL_ASSERT("parse error in built-in schema?", start.line, start.column, message);
  }

 private:
  kj::StringPtr name;
  kj::StringPtr content;
};

}  // namespace workerd::server
