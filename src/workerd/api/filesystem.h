#pragma once

#include <workerd/api/streams/writable.h>
#include <workerd/io/worker-fs.h>
#include <workerd/jsg/iterator.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class Blob;
class File;

class URL;  // Legacy URL class impl
namespace url {
class URL;
}  // namespace url

// Metadata about a file, directory, or link.
struct Stat {
  kj::StringPtr type;  // One of either "file", "directory", or "symlink"
  uint32_t size;
  uint64_t lastModified;
  uint64_t created;
  bool writable;
  bool device;
  JSG_STRUCT(type, size, lastModified, created, writable, device);

  Stat(const workerd::Stat& stat);
};

// A simple RAII handle for a file descriptor. When the instance is
// destroyed, the file descriptor is closed if it hasn't already
// been closed.
class FileFdHandle final: public jsg::Object {
 public:
  FileFdHandle(jsg::Lock& js, const workerd::VirtualFileSystem& vfs, int fd);
  ~FileFdHandle() noexcept;

  static jsg::Ref<FileFdHandle> constructor(jsg::Lock& js, int fd);

  void close(jsg::Lock&);

  JSG_RESOURCE_TYPE(FileFdHandle) {
    JSG_METHOD(close);
  }

 private:
  kj::Maybe<kj::Own<void>> fdHandle;
};

class FileSystemModule final: public jsg::Object {
 public:
  using FilePath = kj::OneOf<jsg::Ref<URL>, jsg::Ref<url::URL>>;

  struct StatOptions {
    jsg::Optional<bool> followSymlinks;
    JSG_STRUCT(followSymlinks);
  };

  kj::Maybe<Stat> stat(jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd, StatOptions options);
  void setLastModified(
      jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd, kj::Date lastModified, StatOptions options);
  void truncate(jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd, uint32_t size);

  struct ReadLinkOptions {
    // The readLink method is used for both realpath and readlink. The readlink
    // API will throw an error if the path is not a symlink. The realpath API
    // will return the resolved path either way.
    bool failIfNotSymlink = false;
    JSG_STRUCT(failIfNotSymlink);
  };
  kj::String readLink(jsg::Lock& js, FilePath path, ReadLinkOptions options);

  struct LinkOptions {
    bool symbolic;
    JSG_STRUCT(symbolic);
  };

  void link(jsg::Lock& js, FilePath from, FilePath to, LinkOptions options);

  void unlink(jsg::Lock& js, FilePath path);

  struct OpenOptions {
    // File is opened to supprt reads.
    bool read;
    // File is opened to support writes.
    bool write;
    // File is opened in append mode. Ignored if write is false.
    bool append;
    // If the exclusive option is set, throw if the file already exists.
    bool exclusive;
    // If the followSymlinks option is set, follow symbolic links.
    bool followSymlinks = true;
    JSG_STRUCT(read, write, append, exclusive, followSymlinks);
  };

  int open(jsg::Lock& js, FilePath path, OpenOptions options);
  void close(jsg::Lock& js, int fd);

  struct WriteOptions {
    kj::Maybe<uint64_t> position;
    JSG_STRUCT(position);
  };

  uint32_t write(jsg::Lock& js, int fd, kj::Array<jsg::BufferSource> data, WriteOptions options);
  uint32_t read(jsg::Lock& js, int fd, kj::Array<jsg::BufferSource> data, WriteOptions options);

  jsg::BufferSource readAll(jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd);

  struct WriteAllOptions {
    bool exclusive;
    bool append;
    JSG_STRUCT(exclusive, append);
  };

  uint32_t writeAll(jsg::Lock& js,
      kj::OneOf<int, FilePath> pathOrFd,
      jsg::BufferSource data,
      WriteAllOptions options);

  struct RenameOrCopyOptions {
    bool copy;
    JSG_STRUCT(copy);
  };

  void renameOrCopy(jsg::Lock& js, FilePath src, FilePath dest, RenameOrCopyOptions options);

  struct MkdirOptions {
    bool recursive = false;
    // When temp is true, the directory name will be augmented with an
    // additional random string to make it unique and the directory name
    // will be returned.
    bool tmp = false;
    JSG_STRUCT(recursive, tmp);
  };
  jsg::Optional<kj::String> mkdir(jsg::Lock& js, FilePath path, MkdirOptions options);

  struct RmOptions {
    bool recursive = false;
    bool force = false;
    bool dironly = false;
    JSG_STRUCT(recursive, force, dironly);
  };
  void rm(jsg::Lock& js, FilePath path, RmOptions options);

  struct DirEntHandle {
    kj::String name;
    kj::String parentPath;
    int type;
    JSG_STRUCT(name, parentPath, type);
  };
  struct ReadDirOptions {
    bool recursive = false;
    JSG_STRUCT(recursive);
  };
  kj::Array<DirEntHandle> readdir(jsg::Lock& js, FilePath path, ReadDirOptions options);

  FileSystemModule() = default;
  FileSystemModule(jsg::Lock&, const jsg::Url&) {}

  jsg::Ref<FileFdHandle> getFdHandle(jsg::Lock& js, int fd) {
    return FileFdHandle::constructor(js, fd);
  }

  struct CpOptions {
    bool deferenceSymlinks;
    bool recursive;
    bool force;
    bool errorOnExist;
    JSG_STRUCT(deferenceSymlinks, recursive, force, errorOnExist);
  };

  void cp(jsg::Lock& js, FilePath src, FilePath dest, CpOptions options);

  JSG_RESOURCE_TYPE(FileSystemModule) {
    JSG_METHOD(stat);
    JSG_METHOD(setLastModified);
    JSG_METHOD(truncate);
    JSG_METHOD(readLink);
    JSG_METHOD(link);
    JSG_METHOD(unlink);
    JSG_METHOD(open);
    JSG_METHOD(close);
    JSG_METHOD(write);
    JSG_METHOD(read);
    JSG_METHOD(readAll);
    JSG_METHOD(writeAll);
    JSG_METHOD(renameOrCopy);
    JSG_METHOD(mkdir);
    JSG_METHOD(rm);
    JSG_METHOD(readdir);
    JSG_METHOD(getFdHandle);
    JSG_METHOD(cp);
  }

 private:
  // Rather than relying on random generation of temp directory names we
  // use a simple counter. Once the counter reaches the max value we will
  // refuse to create any more temp directories. This is a bit of a hack
  // to allow temp dirs to be created outside of an IoContext.
  uint32_t tmpFileCounter = 0;
};

// Create a Node.js-style "UVException". The UVException is an
// ordinary Error object with additional properties like code,
// syscall, path, and destination properties. It is primarily
// used to represent file system API errors.
jsg::JsValue createUVException(jsg::Lock& js,
    int errorno,
    kj::StringPtr syscall,
    kj::StringPtr message = nullptr,
    kj::StringPtr path = nullptr,
    kj::StringPtr dest = nullptr);

// Throw a Node.js-style "UVException". The UVException is an
// ordinary Error object with additional properties like code,
// syscall, path, and destination properties. It is primarily
// used to represent file system API errors.
[[noreturn]] void throwUVException(jsg::Lock& js,
    int errorno,
    kj::StringPtr syscall,
    kj::StringPtr message = nullptr,
    kj::StringPtr path = nullptr,
    kj::StringPtr dest = nullptr);

// This is an intentionally truncated list of the error codes that Node.js/libuv
// uses. We won't need all of the error codes that libuv uses.
#define UV_ERRNO_MAP(XX)                                                                           \
  XX(EACCES, "permission denied")                                                                  \
  XX(EBADF, "bad file descriptor")                                                                 \
  XX(EEXIST, "file already exists")                                                                \
  XX(EFBIG, "file too large")                                                                      \
  XX(EINVAL, "invalid argument")                                                                   \
  XX(EISDIR, "illegal operation on a directory")                                                   \
  XX(ELOOP, "too many symbolic links encountered")                                                 \
  XX(EMFILE, "too many open files")                                                                \
  XX(ENAMETOOLONG, "name too long")                                                                \
  XX(ENFILE, "file table overflow")                                                                \
  XX(ENOBUFS, "no buffer space available")                                                         \
  XX(ENODEV, "no such device")                                                                     \
  XX(ENOENT, "no such file or directory")                                                          \
  XX(ENOMEM, "not enough memory")                                                                  \
  XX(ENOSPC, "no space left on device")                                                            \
  XX(ENOSYS, "function not implemented")                                                           \
  XX(ENOTDIR, "not a directory")                                                                   \
  XX(ENOTEMPTY, "directory not empty")                                                             \
  XX(EPERM, "operation not permitted")                                                             \
  XX(EMLINK, "too many links")                                                                     \
  XX(EIO, "input/output error")

#define XX(code, _) constexpr int UV_##code = -code;
UV_ERRNO_MAP(XX)
#undef XX

// ======================================================================================
// An implementation of the WHATWG Web File System API (https://fs.spec.whatwg.org/)
// All of the classes in this part of the impl are defined by the spec.

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
  const jsg::USVString& getName(jsg::Lock& js) {
    return name;
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
    jsg::Optional<bool> keepExistingData;
    JSG_STRUCT(keepExistingData);
  };

  jsg::Promise<jsg::Ref<File>> getFile(
      jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);

  jsg::Promise<jsg::Ref<FileSystemWritableFileStream>> createWritable(jsg::Lock& js,
      jsg::Optional<FileSystemCreateWritableOptions> options,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);

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

  JSG_ASYNC_ITERATOR(EntryIterator,
      entries,
      EntryIteratorType,
      IteratorState,
      iteratorNext<EntryIteratorType>,
      iteratorReturn<EntryIteratorType>);
  JSG_ASYNC_ITERATOR(KeyIterator,
      keys,
      KeyIteratorType,
      IteratorState,
      iteratorNext<KeyIteratorType>,
      iteratorReturn<KeyIteratorType>);
  JSG_ASYNC_ITERATOR(ValueIterator,
      values,
      ValueIteratorType,
      IteratorState,
      iteratorNext<ValueIteratorType>,
      iteratorReturn<ValueIteratorType>);

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
    JSG_ASYNC_ITERABLE(entries);
    JSG_METHOD(forEach);
  }

 private:
  kj::Rc<workerd::Directory> inner;

  template <typename Type>
  static jsg::Promise<kj::Maybe<Type>> iteratorNext(jsg::Lock& js, IteratorState& state) {
    if (state.index >= state.entries.size()) {
      return js.resolvedPromise<kj::Maybe<Type>>(kj::none);
    }

    auto& entry = state.entries[state.index++];

    if constexpr (kj::isSameType<Type, EntryIteratorType>()) {
      return js.resolvedPromise<kj::Maybe<Type>>(
          EntryType(js.accountedUSVString(entry->getName(js)), entry.addRef()));
    } else if constexpr (kj::isSameType<Type, KeyIteratorType>()) {
      return js.resolvedPromise<kj::Maybe<Type>>(js.accountedUSVString(entry->getName(js)));
    } else if constexpr (kj::isSameType<Type, ValueIteratorType>()) {
      return js.resolvedPromise<kj::Maybe<Type>>(entry.addRef());
    } else {
      static_assert(false, "invalid iterator type");
    }
  }

  template <typename Type>
  static jsg::Promise<void> iteratorReturn(
      jsg::Lock& js, IteratorState& state, jsg::Optional<Type>& value) {
    return js.resolvedPromise();
  }
};

class FileSystemWritableFileStream: public WritableStream {
 public:
  struct State: public kj::Refcounted {
    // The FileSystemWritableFileStream is a bit transactional in nature.
    // All writes are done to a temporary file. When the stream is closed
    // without an error, the original file data is replaced with the data
    // in the temporary buffer. If the stream is aborted, the temporary file
    // is deleted and the original contents are left intact.
    // The temporary file is not visible to the user and is not included in
    // any directory. It is only used to hold the data until the stream
    // is closed.
    kj::Maybe<kj::Rc<workerd::File>> temp;

    // The file we will be writing to.
    kj::Maybe<kj::Rc<workerd::File>> file;

    // A note on file position and sizes. In the spec, file sizes and positions
    // are defined in terms of unsigned long long, or 64-bits. We are using
    // unsigned long (uint32_t) for these instead. This is largely because
    // the underlying implementation is holding the file in memory and 4 GB
    // is WAY more than our isolate heap limit. A uint32_t is more than enough
    // for our purposes.
    uint32_t position = 0;
    State(kj::Rc<workerd::File> file, kj::Rc<workerd::File> temp)
        : temp(kj::mv(temp)),
          file(kj::mv(file)) {}

    void clear() {
      temp = kj::none;
      file = kj::none;
      position = 0;
    }
  };

  FileSystemWritableFileStream(
      kj::Own<WritableStreamController> controller, kj::Rc<State> sharedState);

  static jsg::Ref<FileSystemWritableFileStream> constructor() = delete;

  struct WriteParams {
    kj::String type;  // one of: write, seek, truncate
    jsg::Optional<uint32_t> size;
    jsg::Optional<uint32_t> position;
    jsg::Optional<kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String>> data;
    JSG_STRUCT(type, size, position, data);
  };

  jsg::Promise<void> write(jsg::Lock& js,
      kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String, WriteParams> data,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);
  jsg::Promise<void> seek(jsg::Lock& js,
      uint32_t position,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);
  jsg::Promise<void> truncate(
      jsg::Lock& js, uint32_t size, const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);

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
    jsg::Optional<uint32_t> at;
    JSG_STRUCT(at);
  };

  uint32_t read(
      jsg::Lock& js, jsg::BufferSource buffer, jsg::Optional<FileSystemReadWriteOptions> options);
  uint32_t write(jsg::Lock& js,
      jsg::BufferSource buffer,
      jsg::Optional<FileSystemReadWriteOptions> options,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);

  void truncate(jsg::Lock& js,
      uint32_t newSize,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);
  uint32_t getSize(jsg::Lock& js);
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
  uint32_t position = 0;
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
  workerd::api::FileSystemModule, workerd::api::Stat, workerd::api::FileSystemModule::StatOptions, \
      workerd::api::FileSystemModule::ReadLinkOptions,                                             \
      workerd::api::FileSystemModule::LinkOptions, workerd::api::FileSystemModule::OpenOptions,    \
      workerd::api::FileSystemModule::WriteOptions,                                                \
      workerd::api::FileSystemModule::WriteAllOptions,                                             \
      workerd::api::FileSystemModule::RenameOrCopyOptions,                                         \
      workerd::api::FileSystemModule::MkdirOptions, workerd::api::FileSystemModule::RmOptions,     \
      workerd::api::FileSystemModule::DirEntHandle,                                                \
      workerd::api::FileSystemModule::ReadDirOptions, workerd::api::FileFdHandle,                  \
      workerd::api::FileSystemModule::CpOptions

}  // namespace workerd::api
