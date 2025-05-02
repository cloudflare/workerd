#pragma once

#include <workerd/api/streams/writable.h>
#include <workerd/io/worker-fs.h>
#include <workerd/jsg/iterator.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

// Implements the *internal* JavaScript file system API. This is not exposed
// directly to users but provides the internal module that underlies the
// node:fs API implementation.

class Blob;
class File;

// The JS view of the workerd::Stat metadata struct. The only real difference
// here is that type is a string instead of an enum.
struct Stat final {
  kj::StringPtr type;  // One of: "file", "directory", "symlink"
  size_t size;
  kj::Date lastModified;
  kj::Date created;
  bool writable;
  JSG_STRUCT(type, size, lastModified, created, writable);
  Stat(const workerd::Stat& stat);
};

class DirectoryHandle;

// The wrapper for the workerd::File class.
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

// The wrapper for the workerd::SymbolicLink class.
class SymbolicLinkHandle final: public jsg::Object {
 public:
  SymbolicLinkHandle(kj::Rc<workerd::SymbolicLink> inner);

  // Returns the stat for the symbolic link itself, not the target.
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

// The wrapper for the workerd::Directory class.
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

  // Note that iterators capture the state of the directory at the time they are created.
  // If the directory is modified, the iterator will not be invalidated or updated.
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

// The implementation of the `cloudflare-internal:filesystem` built-in module.
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

// ======================================================================================
// An implementation of the WHATWG Web File System API (https://fs.spec.whatwg.org/)

class FileSystemSyncAccessHandle;
class FileSystemWritableFileStream;

class FileSystemHandle: public jsg::Object {
  // TODO(node-fs): The spec defines FileSystemHandle objects as being
  // serializable, meaning that they should work with structured cloning.
  // In workers, this would suggest also that they should work with jsrpc.
  // We don't yet implement any form of serialization for these objects.
  // Since each worker has its own file system, it might be a bit weird
  // as the received file on the other side would not actually be in that
  // destination worker's file system... that is, it would be an orphaned
  // file handle. It would still likely be useful tho. As a follow up
  // step, we will implement serialization/deserialization of these objects.
 public:
  FileSystemHandle(jsg::USVString name): name(kj::mv(name)) {}
  jsg::USVString getName(jsg::Lock& js) {
    return jsg::USVString(kj::str(name));
  }

  virtual kj::StringPtr getKind(jsg::Lock& js) {
    // Implemented by subclasses.
    KJ_UNIMPLEMENTED("getKind() not implemented");
  }
  virtual jsg::Promise<bool> isSameEntry(jsg::Lock& js, jsg::Ref<FileSystemHandle> other) {
    // Implemented by subclasses.
    KJ_UNIMPLEMENTED("isSameEntry() not implemented");
  }

  JSG_RESOURCE_TYPE(FileSystemHandle) {
    JSG_READONLY_PROTOTYPE_PROPERTY(kind, getKind);
    JSG_READONLY_PROTOTYPE_PROPERTY(name, getName);
    JSG_METHOD(isSameEntry);
  }

 private:
  jsg::USVString name;
};

class FileSystemFileHandle: public FileSystemHandle {
 public:
  FileSystemFileHandle(jsg::USVString name, kj::Rc<workerd::File> inner);

  kj::StringPtr getKind(jsg::Lock& js) override {
    return "file"_kj;
  }

  jsg::Promise<bool> isSameEntry(jsg::Lock& js, jsg::Ref<FileSystemHandle> other) override;

  struct FileSystemCreateWritableOptions {
    bool keepExistingData = false;
    JSG_STRUCT(keepExistingData);
  };

  jsg::Promise<jsg::Ref<File>> getFile(jsg::Lock& js);

  jsg::Promise<jsg::Ref<FileSystemWritableFileStream>> createWritable(
      jsg::Lock& js, jsg::Optional<FileSystemCreateWritableOptions> options);

  jsg::Promise<jsg::Ref<FileSystemSyncAccessHandle>> createSyncAccessHandle(jsg::Lock& js);

  JSG_RESOURCE_TYPE(FileSystemFileHandle) {
    JSG_INHERIT(FileSystemHandle);
    JSG_METHOD(getFile);
    JSG_METHOD(createWritable);
    JSG_METHOD(createSyncAccessHandle);
  }

 private:
  kj::Rc<workerd::File> inner;
};

class FileSystemDirectoryHandle final: public FileSystemHandle {
  // TODO(node-fs): Per the spec, FileSystemDirectoryHandler objects are
  // expected to be async iterables. Here we implement it as a sync iterable.
  // The effect ends up being largely the same but it's obviously better to
  // use the asyn iterable API instead. We should implement this before we
  // ship this feature.
 public:
  struct EntryType {
    jsg::USVString key;
    jsg::Ref<FileSystemHandle> value;
    JSG_STRUCT(key, value);
    EntryType(jsg::USVString key, jsg::Ref<FileSystemHandle> value)
        : key(kj::mv(key)),
          value(kj::mv(value)) {}
  };

 private:
  using EntryIteratorType = EntryType;
  using KeyIteratorType = jsg::USVString;
  using ValueIteratorType = jsg::Ref<FileSystemHandle>;

  struct IteratorState final {
    jsg::Ref<FileSystemDirectoryHandle> parent;
    kj::Array<jsg::Ref<FileSystemHandle>> entries;
    uint index = 0;

    IteratorState(
        jsg::Ref<FileSystemDirectoryHandle> parent, kj::Array<jsg::Ref<FileSystemHandle>> entries)
        : parent(kj::mv(parent)),
          entries(kj::mv(entries)) {}

    void visitForGc(jsg::GcVisitor& visitor) {
      visitor.visit(parent);
      visitor.visitAll(entries);
    }

    JSG_MEMORY_INFO(IteratorState) {
      tracker.trackField("parent", parent);
      for (auto& entry: entries) {
        tracker.trackField("entry", entry);
      }
    }
  };

 public:
  FileSystemDirectoryHandle(jsg::USVString name, kj::Rc<workerd::Directory> inner);

  kj::StringPtr getKind(jsg::Lock& js) override {
    return "directory"_kj;
  }

  jsg::Promise<bool> isSameEntry(jsg::Lock& js, jsg::Ref<FileSystemHandle> other) override;

  struct FileSystemGetFileOptions {
    bool create = false;
    JSG_STRUCT(create);
  };

  struct FileSystemGetDirectoryOptions {
    bool create = false;
    JSG_STRUCT(create);
  };

  struct FileSystemRemoveOptions {
    bool recursive = false;
    JSG_STRUCT(recursive);
  };

  jsg::Promise<jsg::Ref<FileSystemFileHandle>> getFileHandle(jsg::Lock& js,
      jsg::USVString name,
      jsg::Optional<FileSystemGetFileOptions> options,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception);

  jsg::Promise<jsg::Ref<FileSystemDirectoryHandle>> getDirectoryHandle(jsg::Lock& js,
      jsg::USVString name,
      jsg::Optional<FileSystemGetDirectoryOptions> options,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception);

  jsg::Promise<void> removeEntry(jsg::Lock& js,
      jsg::USVString name,
      jsg::Optional<FileSystemRemoveOptions> options,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception);

  // TODO(node-fs): We are not currently implementing the resolve() method
  // as defined in the spec. This is a bit tricky as it requires us to work
  // backwards through the parent directories to find the path to the file.
  // However, our file node's do not currently have a parent pointer, nor do
  // they know through which path they are accessible through. Support for this
  // is not critical so we likely won't implement this before we ship the feature
  // but it is something we should consider doing in the future.
  jsg::Promise<kj::Array<jsg::USVString>> resolve(
      jsg::Lock& js, jsg::Ref<FileSystemHandle> possibleDescendant);

  JSG_ITERATOR(
      EntryIterator, entries, EntryIteratorType, IteratorState, iteratorNext<EntryIteratorType>);
  JSG_ITERATOR(KeyIterator, keys, KeyIteratorType, IteratorState, iteratorNext<KeyIteratorType>);
  JSG_ITERATOR(
      ValueIterator, values, ValueIteratorType, IteratorState, iteratorNext<ValueIteratorType>);

  void forEach(jsg::Lock& js,
      jsg::Function<void(
          jsg::USVString, jsg::Ref<FileSystemHandle>, jsg::Ref<FileSystemDirectoryHandle>)>
          callback,
      jsg::Optional<jsg::Value> thisArg);

  JSG_RESOURCE_TYPE(FileSystemDirectoryHandle) {
    JSG_INHERIT(FileSystemHandle);
    JSG_METHOD(getFileHandle);
    JSG_METHOD(getDirectoryHandle);
    JSG_METHOD(removeEntry);
    JSG_METHOD(resolve);
    JSG_METHOD(entries);
    JSG_METHOD(keys);
    JSG_METHOD(values);
    JSG_ITERABLE(entries);
    JSG_METHOD(forEach);
  }

 private:
  kj::Rc<workerd::Directory> inner;

  template <typename Type>
  static kj::Maybe<Type> iteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.entries.size()) {
      return kj::none;
    }

    auto& entry = state.entries[state.index++];

    if constexpr (kj::isSameType<Type, EntryIteratorType>()) {
      return EntryType(js.accountedUSVString(entry->getName(js)), entry.addRef());
    } else if constexpr (kj::isSameType<Type, KeyIteratorType>()) {
      return js.accountedUSVString(entry->getName(js));
    } else if constexpr (kj::isSameType<Type, ValueIteratorType>()) {
      return entry.addRef();
    } else {
      KJ_UNREACHABLE;
    }
  }
};

class FileSystemWritableFileStream: public WritableStream {
  // TODO(node-fs): The spec defines FileSystemWritableFileStream objects such
  // that any changes to the file are not actually committed until the stream
  // is closed. We're not yet implementing this. All writes modify the source
  // file in memory and would be immediately visible to other handles pointing
  // to the same file. This should be updated before this feature is unflagged
  // and made stable.
 public:
  struct State: public kj::Refcounted {
    kj::Maybe<kj::Rc<workerd::File>> file;
    size_t position = 0;
    State(kj::Rc<workerd::File> file): file(kj::mv(file)) {}
  };

  FileSystemWritableFileStream(
      kj::Own<WritableStreamController> controller, kj::Rc<State> sharedState);

  static jsg::Ref<FileSystemWritableFileStream> constructor() = delete;

  struct WriteParams {
    kj::String type;  // one of: write, seek, truncate
    jsg::Optional<double> size;
    jsg::Optional<double> position;
    jsg::Optional<kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String>> data;
    JSG_STRUCT(type, size, position, data);
  };

  jsg::Promise<void> write(
      jsg::Lock& js, kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String, WriteParams> data);
  jsg::Promise<void> seek(jsg::Lock& js, double position);
  jsg::Promise<void> truncate(jsg::Lock& js, double size);

  JSG_RESOURCE_TYPE(FileSystemWritableFileStream) {
    JSG_INHERIT(WritableStream);
    JSG_METHOD(write);
    JSG_METHOD(seek);
    JSG_METHOD(truncate);
  }

 private:
  kj::Rc<State> sharedState;
};

class FileSystemSyncAccessHandle final: public jsg::Object {
 public:
  FileSystemSyncAccessHandle(kj::Rc<workerd::File> inner);

  struct FileSystemReadWriteOptions {
    jsg::Optional<double> at;
    JSG_STRUCT(at);
  };

  double read(
      jsg::Lock& js, jsg::BufferSource buffer, jsg::Optional<FileSystemReadWriteOptions> options);
  double write(
      jsg::Lock& js, jsg::BufferSource buffer, jsg::Optional<FileSystemReadWriteOptions> options);

  void truncate(jsg::Lock& js, double newSize);
  double getSize(jsg::Lock& js);
  void flush(jsg::Lock& js);
  void close(jsg::Lock& js);

  JSG_RESOURCE_TYPE(FileSystemSyncAccessHandle) {
    JSG_METHOD(read);
    JSG_METHOD(write);
    JSG_METHOD(truncate);
    JSG_METHOD(getSize);
    JSG_METHOD(flush);
    JSG_METHOD(close);
  }

 private:
  kj::Maybe<kj::Rc<workerd::File>> inner;
  size_t position = 0;
};

class StorageManager final: public jsg::Object {
 public:
  jsg::Promise<jsg::Ref<FileSystemDirectoryHandle>> getDirectory(
      jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception);

  JSG_RESOURCE_TYPE(StorageManager) {
    JSG_METHOD(getDirectory);
  }
};

#define EW_WEB_FILESYSTEM_ISOLATE_TYPE                                                             \
  workerd::api::FileSystemHandle, workerd::api::FileSystemFileHandle,                              \
      workerd::api::FileSystemDirectoryHandle, workerd::api::FileSystemWritableFileStream,         \
      workerd::api::FileSystemSyncAccessHandle, workerd::api::StorageManager,                      \
      workerd::api::FileSystemFileHandle::FileSystemCreateWritableOptions,                         \
      workerd::api::FileSystemDirectoryHandle::FileSystemGetFileOptions,                           \
      workerd::api::FileSystemDirectoryHandle::FileSystemGetDirectoryOptions,                      \
      workerd::api::FileSystemDirectoryHandle::FileSystemRemoveOptions,                            \
      workerd::api::FileSystemSyncAccessHandle::FileSystemReadWriteOptions,                        \
      workerd::api::FileSystemWritableFileStream::WriteParams,                                     \
      workerd::api::FileSystemDirectoryHandle::EntryType,                                          \
      workerd::api::FileSystemDirectoryHandle::EntryIterator,                                      \
      workerd::api::FileSystemDirectoryHandle::KeyIterator,                                        \
      workerd::api::FileSystemDirectoryHandle::ValueIterator,                                      \
      workerd::api::FileSystemDirectoryHandle::EntryIterator::Next,                                \
      workerd::api::FileSystemDirectoryHandle::KeyIterator::Next,                                  \
      workerd::api::FileSystemDirectoryHandle::ValueIterator::Next

#define EW_FILESYSTEM_ISOLATE_TYPES                                                                \
  workerd::api::FileSystemModule, workerd::api::FileHandle, workerd::api::DirectoryHandle,         \
      workerd::api::SymbolicLinkHandle, workerd::api::Stat,                                        \
      workerd::api::DirectoryHandle::RemoveOptions, workerd::api::DirectoryHandle::Entry,          \
      workerd::api::DirectoryHandle::EntryIterator, workerd::api::DirectoryHandle::KeyIterator,    \
      workerd::api::DirectoryHandle::EntryIterator::Next,                                          \
      workerd::api::DirectoryHandle::KeyIterator::Next

}  // namespace workerd::api
