// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "wal-reporting-directory.h"

namespace workerd {

namespace {

// A WAL file: each write, once it succeeds, goes to the observer.
class WalFile final: public kj::File {
 public:
  WalFile(kj::Own<const kj::File> inner, kj::String path, kj::Arc<WalObserver> observer)
      : inner(kj::mv(inner)),
        path(kj::mv(path)),
        observer(kj::mv(observer)) {}

  Metadata stat() const override {
    return inner->stat();
  }
  void sync() const override {
    inner->sync();
  }
  void datasync() const override {
    inner->datasync();
  }
  kj::Own<const FsNode> cloneFsNode() const override {
    return kj::heap<WalFile>(inner->clone(), kj::str(path), observer.addRef());
  }
  size_t read(uint64_t offset, kj::ArrayPtr<kj::byte> buffer) const override {
    return inner->read(offset, buffer);
  }
  kj::Array<const kj::byte> mmap(uint64_t offset, uint64_t size) const override {
    return inner->mmap(offset, size);
  }
  kj::Array<kj::byte> mmapPrivate(uint64_t offset, uint64_t size) const override {
    return inner->mmapPrivate(offset, size);
  }
  void write(uint64_t offset, kj::ArrayPtr<const kj::byte> data) const override {
    inner->write(offset, data);
    observer->walWrite(path, offset, data);
  }
  void zero(uint64_t offset, uint64_t size) const override {
    inner->zero(offset, size);
  }
  void truncate(uint64_t size) const override {
    inner->truncate(size);
  }
  kj::Own<const kj::WritableFileMapping> mmapWritable(
      uint64_t offset, uint64_t size) const override {
    return inner->mmapWritable(offset, size);
  }

 private:
  kj::Own<const kj::File> inner;
  kj::String path;
  kj::Arc<WalObserver> observer;
};

class WalReportingDirectory final: public kj::Directory {
 public:
  WalReportingDirectory(kj::Own<const kj::Directory> inner, kj::Arc<WalObserver> observer)
      : inner(kj::mv(inner)),
        observer(kj::mv(observer)) {}

  Metadata stat() const override {
    return inner->stat();
  }
  void sync() const override {
    inner->sync();
  }
  void datasync() const override {
    inner->datasync();
  }
  kj::Own<const FsNode> cloneFsNode() const override {
    return kj::heap<WalReportingDirectory>(inner->clone(), observer.addRef());
  }
  kj::Array<kj::String> listNames() const override {
    return inner->listNames();
  }
  kj::Array<Entry> listEntries() const override {
    return inner->listEntries();
  }
  bool exists(kj::PathPtr path) const override {
    return inner->exists(path);
  }
  kj::Maybe<Metadata> tryLstat(kj::PathPtr path) const override {
    return inner->tryLstat(path);
  }
  kj::Maybe<kj::Own<const kj::ReadableFile>> tryOpenFile(kj::PathPtr path) const override {
    return inner->tryOpenFile(path);
  }
  kj::Maybe<kj::Own<const kj::ReadableDirectory>> tryOpenSubdir(kj::PathPtr path) const override {
    return inner->tryOpenSubdir(path);
  }
  kj::Maybe<kj::String> tryReadlink(kj::PathPtr path) const override {
    return inner->tryReadlink(path);
  }
  kj::Maybe<kj::Own<const kj::File>> tryOpenFile(
      kj::PathPtr path, kj::WriteMode mode) const override {
    auto name = path.toString();
    return inner->tryOpenFile(path, mode)
        .map([&](kj::Own<const kj::File>&& file) -> kj::Own<const kj::File> {
      if (!name.endsWith("-wal")) return kj::mv(file);
      return kj::heap<WalFile>(kj::mv(file), kj::mv(name), observer.addRef());
    });
  }
  kj::Own<Replacer<kj::File>> replaceFile(kj::PathPtr path, kj::WriteMode mode) const override {
    return inner->replaceFile(path, mode);
  }
  kj::Own<const kj::File> createTemporary() const override {
    return inner->createTemporary();
  }
  kj::Maybe<kj::Own<kj::AppendableFile>> tryAppendFile(
      kj::PathPtr path, kj::WriteMode mode) const override {
    return inner->tryAppendFile(path, mode);
  }
  kj::Maybe<kj::Own<const kj::Directory>> tryOpenSubdir(
      kj::PathPtr path, kj::WriteMode mode) const override {
    return inner->tryOpenSubdir(path, mode);
  }
  kj::Own<Replacer<kj::Directory>> replaceSubdir(
      kj::PathPtr path, kj::WriteMode mode) const override {
    return inner->replaceSubdir(path, mode);
  }
  bool trySymlink(kj::PathPtr linkpath, kj::StringPtr content, kj::WriteMode mode) const override {
    return inner->trySymlink(linkpath, content, mode);
  }
  bool tryTransfer(kj::PathPtr toPath,
      kj::WriteMode toMode,
      const kj::Directory& fromDirectory,
      kj::PathPtr fromPath,
      kj::TransferMode mode) const override {
    return inner->tryTransfer(toPath, toMode, fromDirectory, fromPath, mode);
  }
  bool tryRemove(kj::PathPtr path) const override {
    return inner->tryRemove(path);
  }

 private:
  kj::Own<const kj::Directory> inner;
  kj::Arc<WalObserver> observer;
};

}  // namespace

kj::Own<const kj::Directory> newWalReportingDirectory(
    kj::Own<const kj::Directory> inner, kj::Arc<WalObserver> observer) {
  return kj::heap<WalReportingDirectory>(kj::mv(inner), kj::mv(observer));
}

}  // namespace workerd
