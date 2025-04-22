#pragma once

#include <workerd/io/worker-fs.h>
#include <workerd/jsg/iterator.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Implements the *internal* JavaScript file system API. This is not exposed
// directly to users but provides the internal module that underlies the
// node:fs API implementation.

class Blob;

struct Stat final {
  kj::StringPtr type;
  size_t size;
  kj::Date lastModified;
  kj::Date created;
  bool writable;
  JSG_STRUCT(type, size, lastModified, created, writable);
  Stat(const workerd::Stat& stat);
};

class DirectoryHandle;

class FileHandle final: public jsg::Object {
 public:
  FileHandle(kj::Rc<workerd::File> inner);

  static jsg::Ref<FileHandle> constructor(jsg::Lock& js, jsg::Optional<size_t> size);
  Stat getStat(jsg::Lock& js);
  void setLastModified(jsg::Lock& js, kj::Date date);
  jsg::JsString readAllText(jsg::Lock& js);
  jsg::BufferSource readAllBytes(jsg::Lock& js);
  jsg::Ref<Blob> readAllAsBlob(jsg::Lock& js);
  size_t read(jsg::Lock& js, size_t offset, jsg::BufferSource buffer);
  size_t writeAll(jsg::Lock& js, kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String> data);
  size_t write(
      jsg::Lock& js, size_t offset, kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String> data);
  void fill(jsg::Lock& js, size_t offset, kj::byte val);
  void resize(jsg::Lock& js, size_t size);
  bool equals(jsg::Lock& js, jsg::Ref<FileHandle> other) {
    return inner.get() == other->inner.get();
  }

  JSG_RESOURCE_TYPE(FileHandle) {
    // Do not make this a lazy property because we need to call it
    // multiple times to get current values.
    JSG_READONLY_PROTOTYPE_PROPERTY(stat, getStat);
    JSG_METHOD(setLastModified);
    JSG_METHOD(readAllText);
    JSG_METHOD(readAllBytes);
    JSG_METHOD(readAllAsBlob);
    JSG_METHOD(read);
    JSG_METHOD(writeAll);
    JSG_METHOD(write);
    JSG_METHOD(fill);
    JSG_METHOD(resize);
    JSG_METHOD(equals);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("inner", *inner.get());
  }

  kj::Rc<workerd::File> getInner() {
    return inner.addRef();
  }

 private:
  kj::Rc<workerd::File> inner;
};

class SymbolicLinkHandle final: public jsg::Object {
 public:
  SymbolicLinkHandle(kj::Rc<workerd::SymbolicLink> inner);

  Stat getStat(jsg::Lock& js);
  kj::String getTargetPath(jsg::Lock& js);
  kj::Maybe<kj::OneOf<jsg::Ref<FileHandle>, jsg::Ref<DirectoryHandle>>> resolve(jsg::Lock& js);

  bool equals(jsg::Lock& js, jsg::Ref<SymbolicLinkHandle> other) {
    return inner.get() == other->inner.get();
  }

  JSG_RESOURCE_TYPE(SymbolicLinkHandle) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(stat, getStat);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(targetPath, getTargetPath);
    JSG_METHOD(resolve);
    JSG_METHOD(equals);
  }

  kj::Rc<workerd::SymbolicLink> getInner() {
    return inner.addRef();
  }

 private:
  kj::Rc<workerd::SymbolicLink> inner;
};

class DirectoryHandle final: public jsg::Object {
 public:
  using EntryType =
      kj::OneOf<jsg::Ref<FileHandle>, jsg::Ref<DirectoryHandle>, jsg::Ref<SymbolicLinkHandle>>;
  struct Entry {
    kj::String name;
    EntryType value;

    JSG_STRUCT(name, value);
  };

 private:
  template <typename Type>
  struct IteratorState final {
    jsg::Ref<DirectoryHandle> parent;
    // Our iterator state preserves the parent directory state at the time
    // the iterator was created in order to avoid the iterator being invalidated
    // if the directory is modified.
    kj::Vector<Type> entries;
    uint index = 0;

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(parent);
    }

    JSG_MEMORY_INFO(IteratorState) {
      tracker.trackField("parent", parent);
      if constexpr (kj::isSameType<Type, Entry>()) {
        for (auto& entry: entries) {
          KJ_SWITCH_ONEOF(entry.value) {
            KJ_CASE_ONEOF(file, jsg::Ref<FileHandle>) {
              tracker.trackField("file", file);
            }
            KJ_CASE_ONEOF(dir, jsg::Ref<DirectoryHandle>) {
              tracker.trackField("directory", dir);
            }
            KJ_CASE_ONEOF(link, jsg::Ref<SymbolicLinkHandle>) {
              tracker.trackField("symlink", link);
            }
          }
        }
      }
    }
  };

 public:
  DirectoryHandle(kj::Rc<workerd::Directory> inner);

  static jsg::Ref<DirectoryHandle> constructor(jsg::Lock& js);

  Stat getStat(jsg::Lock& js);

  size_t getCount(jsg::Lock& js, jsg::Optional<kj::String> typeFilter);

  kj::Maybe<kj::OneOf<jsg::Ref<FileHandle>, jsg::Ref<DirectoryHandle>>> open(
      jsg::Lock& js, kj::String path, jsg::Optional<kj::String> createAs);

  struct RemoveOptions {
    bool recursive = false;
    JSG_STRUCT(recursive);
  };

  bool remove(jsg::Lock& js, kj::String path, jsg::Optional<RemoveOptions> options);

  void add(jsg::Lock& js,
      kj::String name,
      kj::OneOf<jsg::Ref<FileHandle>, jsg::Ref<DirectoryHandle>, jsg::Ref<SymbolicLinkHandle>>
          entry);

  JSG_ITERATOR(EntryIterator, entries, Entry, IteratorState<Entry>, entryNext);
  JSG_ITERATOR(KeyIterator, names, kj::String, IteratorState<kj::String>, nameNext);

  void forEach(jsg::Lock& js,
      jsg::Function<void(EntryType, kj::StringPtr, jsg::Ref<DirectoryHandle>)> callback,
      jsg::Optional<jsg::Value> thisArg);

  bool equals(jsg::Lock& js, jsg::Ref<DirectoryHandle> other) {
    return inner.get() == other->inner.get();
  }

  JSG_RESOURCE_TYPE(DirectoryHandle) {
    JSG_READONLY_PROTOTYPE_PROPERTY(stat, getStat);
    JSG_METHOD(getCount);
    JSG_METHOD(open);
    JSG_METHOD(remove);
    JSG_METHOD(forEach);
    JSG_METHOD(names);
    JSG_METHOD(entries);
    JSG_ITERABLE(entries);
    JSG_METHOD(equals);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
    tracker.trackField("inner", *inner.get());
  }

  kj::Rc<workerd::Directory> getInner() {
    return inner.addRef();
  }

 private:
  kj::Rc<workerd::Directory> inner;

  static kj::Maybe<kj::String> nameNext(jsg::Lock& js, IteratorState<kj::String>& state);
  static kj::Maybe<Entry> entryNext(jsg::Lock& js, IteratorState<Entry>& state);
};

class FileSystemModule final: public jsg::Object {
 public:
  FileSystemModule() = default;
  FileSystemModule(jsg::Lock&, const jsg::Url&) {}

  kj::Maybe<jsg::Ref<SymbolicLinkHandle>> symlink(jsg::Lock& js, kj::String targetPath);

  kj::Maybe<jsg::Ref<DirectoryHandle>> getRoot(jsg::Lock& js);

  kj::Maybe<jsg::Ref<DirectoryHandle>> getTmp(jsg::Lock& js);

  kj::Maybe<kj::OneOf<jsg::Ref<FileHandle>, jsg::Ref<DirectoryHandle>>> open(
      jsg::Lock& js, kj::String path);

  kj::Maybe<Stat> stat(jsg::Lock& js, kj::String path);

  JSG_RESOURCE_TYPE(FileSystemModule) {
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(root, getRoot);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(tmp, getTmp);
    JSG_METHOD(open);
    JSG_METHOD(stat);
    JSG_METHOD(symlink);
  }
};

#define EW_FILESYSTEM_ISOLATE_TYPES                                                                \
  workerd::api::FileSystemModule, workerd::api::FileHandle, workerd::api::DirectoryHandle,         \
      workerd::api::SymbolicLinkHandle, workerd::api::Stat,                                        \
      workerd::api::DirectoryHandle::RemoveOptions, workerd::api::DirectoryHandle::Entry
}  // namespace workerd::api
