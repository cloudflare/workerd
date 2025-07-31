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

// ======================================================================================
// An implementation of the WHATWG Web File System API (https://fs.spec.whatwg.org/)
// All of the classes in this part of the impl are defined by the spec.

class FileSystemWritableFileStream;

// This provides access to a file or directory in the virutual file system via a "standardized"
// API. We use quotes around standardized because the API is still very much a work in progress
// as a Web standard and is not fully baked yet, at least not within a single specification
// document. There are bits and pieces of the API defined in various places, including the WHATWG
// spec, WICG proposals, Web Platform Tests, etc. The Web Platform Tests include tests for parts of
// the API that are not yet fully defined with the tests not actually marked as tentative, etc. We
// implement what is defined so far using the Web Platform Tests and the specific characteristics
// of the underlying virtual file system in workers as a guide.
//
// There are two kinds of handles, FileSystemFileHandle and FileSystemDirectoryHandle. Both of these
// extend from FileSystemHandle. A FileSystemHandle holds the locator, which in our implementation
// is a file URL pointing to a location in the worker's virtual file system. When the
// FileSystemHandle is created, we validate that the locator points to a valid location in the
// VFS, however the file may be modified, deleted, etc after the handle is created. This means
// that the handle may become invalid after creation but then may become valid again. The actual
// underlying file or directory is checked each time an operation is performed on the handle.
//
// Two handles are considered the same if they point to the same location and have the same type.
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
  FileSystemHandle(const workerd::VirtualFileSystem& vfs, jsg::Url&& locator, jsg::USVString name);

  const jsg::USVString& getName(jsg::Lock& js) {
    return name;
  }

  virtual kj::StringPtr getKind(jsg::Lock& js) {
    // Implemented by subclasses.
    KJ_UNIMPLEMENTED("getKind() not implemented");
  }

  jsg::Promise<bool> isSameEntry(jsg::Lock& js, jsg::Ref<FileSystemHandle> other);

  struct RemoveOptions {
    jsg::Optional<bool> recursive;
    JSG_STRUCT(recursive);
  };
  jsg::Promise<void> remove(jsg::Lock& js,
      jsg::Optional<RemoveOptions> options,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);

  jsg::Promise<kj::StringPtr> getUniqueId(
      jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);

  JSG_RESOURCE_TYPE(FileSystemHandle) {
    JSG_READONLY_PROTOTYPE_PROPERTY(kind, getKind);
    JSG_READONLY_PROTOTYPE_PROPERTY(name, getName);
    JSG_METHOD(isSameEntry);
    JSG_METHOD(getUniqueId);
    JSG_METHOD(remove);
  }

 protected:
  const jsg::Url& getLocator() const {
    return locator;
  }
  const workerd::VirtualFileSystem& getVfs() const {
    return vfs;
  }

  bool canBeModifiedCurrently(jsg::Lock&) const;

 private:
  const workerd::VirtualFileSystem& vfs;
  const jsg::Url locator;
  jsg::USVString name;
};

struct FileSystemFileWriteParams {
  kj::String type;  // one of: write, seek, truncate
  jsg::Optional<uint32_t> size;
  jsg::Optional<uint32_t> position;
  // Yes, wrapping the kj::Maybe with a jsg::Optional is intentional here. We need to
  // be able to accept null or undefined values and handle them per the spec.
  jsg::Optional<kj::Maybe<kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String>>> data;
  JSG_STRUCT(type, size, position, data);
};

using FileSystemWritableData =
    kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String, FileSystemFileWriteParams>;

class FileSystemFileHandle final: public FileSystemHandle {
 public:
  FileSystemFileHandle(
      const workerd::VirtualFileSystem& vfs, jsg::Url locator, jsg::USVString name);

  kj::StringPtr getKind(jsg::Lock& js) override {
    return "file"_kj;
  }

  struct FileSystemCreateWritableOptions {
    jsg::Optional<bool> keepExistingData;
    JSG_STRUCT(keepExistingData);
  };

  jsg::Promise<jsg::Ref<File>> getFile(
      jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);

  jsg::Promise<jsg::Ref<FileSystemWritableFileStream>> createWritable(jsg::Lock& js,
      jsg::Optional<FileSystemCreateWritableOptions> options,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler,
      const jsg::TypeHandler<FileSystemWritableData>& dataHandler);

  JSG_RESOURCE_TYPE(FileSystemFileHandle) {
    JSG_INHERIT(FileSystemHandle);
    JSG_METHOD(getFile);
    JSG_METHOD(createWritable);
  }

 private:
  mutable size_t writableCount = 0;
  friend class FileSystemWritableFileStream;
};

class FileSystemDirectoryHandle final: public FileSystemHandle {
 private:
  // The actual entry type is an array of two elements, the first being the key,
  // the second being the value.
  using EntryType = kj::OneOf<jsg::JsRef<jsg::JsString>, jsg::Ref<FileSystemHandle>>;
  using EntryIteratorType = kj::Array<EntryType>;
  using KeyIteratorType = jsg::USVString;
  using ValueIteratorType = jsg::Ref<FileSystemHandle>;

  struct IteratorState final {
    struct Listing {
      jsg::Ref<FileSystemDirectoryHandle> parent;
      kj::Array<jsg::Ref<FileSystemHandle>> entries;
      uint index = 0;
    };
    using Errored = jsg::JsRef<jsg::JsValue>;
    kj::OneOf<Listing, Errored> state;

    IteratorState(
        jsg::Ref<FileSystemDirectoryHandle> parent, kj::Array<jsg::Ref<FileSystemHandle>> entries)
        : state(Listing{
            .parent = kj::mv(parent),
            .entries = kj::mv(entries),
          }) {}
    IteratorState(jsg::JsRef<jsg::JsValue> exception): state(kj::mv(exception)) {}

    void visitForGc(jsg::GcVisitor& visitor) {
      KJ_SWITCH_ONEOF(state) {
        KJ_CASE_ONEOF(listing, Listing) {
          visitor.visit(listing.parent);
          visitor.visitAll(listing.entries);
        }
        KJ_CASE_ONEOF(exception, jsg::JsRef<jsg::JsValue>) {
          visitor.visit(exception);
        }
      }
    }

    JSG_MEMORY_INFO(IteratorState) {
      KJ_SWITCH_ONEOF(state) {
        KJ_CASE_ONEOF(listing, Listing) {
          tracker.trackField("parent", listing.parent);
          for (auto& entry: listing.entries) {
            tracker.trackField("entry", entry);
          }
        }
        KJ_CASE_ONEOF(exception, jsg::JsRef<jsg::JsValue>) {
          tracker.trackField("exception", exception);
        }
      }
    }
  };

 public:
  FileSystemDirectoryHandle(
      const workerd::VirtualFileSystem& vfs, jsg::Url locator, jsg::USVString name);

  kj::StringPtr getKind(jsg::Lock& js) override {
    return "directory"_kj;
  }

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
  // as defined in the spec.
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
      jsg::Optional<jsg::Value> thisArg,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception);

  JSG_RESOURCE_TYPE(FileSystemDirectoryHandle) {
    JSG_INHERIT(FileSystemHandle);
    JSG_METHOD(getFileHandle);
    JSG_METHOD(getDirectoryHandle);
    JSG_METHOD(removeEntry);
    JSG_METHOD(resolve);
    JSG_METHOD(entries);
    JSG_METHOD(keys);
    JSG_METHOD(values);
    JSG_METHOD(forEach);
    JSG_ASYNC_ITERABLE(entries);

    JSG_TS_OVERRIDE({
      entries(): AsyncIterableIterator<[string, FileSystemHandle]>;
      [Symbol.asyncIterator]() : AsyncIterableIterator<[string, FileSystemHandle]>;
    });
  }

 private:
  template <typename Type>
  static jsg::Promise<kj::Maybe<Type>> iteratorNext(jsg::Lock& js, IteratorState& state) {
    KJ_SWITCH_ONEOF(state.state) {
      KJ_CASE_ONEOF(listing, IteratorState::Listing) {
        if (listing.index >= listing.entries.size()) {
          return js.resolvedPromise<kj::Maybe<Type>>(kj::none);
        }

        auto& entry = listing.entries[listing.index++];

        if constexpr (kj::isSameType<Type, EntryIteratorType>()) {
          EntryIteratorType result = kj::arr(
              EntryType(jsg::JsRef(js, js.str(entry->getName(js)))), EntryType(entry.addRef()));
          return js.resolvedPromise<kj::Maybe<Type>>(kj::mv(result));
        } else if constexpr (kj::isSameType<Type, KeyIteratorType>()) {
          return js.resolvedPromise<kj::Maybe<Type>>(js.accountedUSVString(entry->getName(js)));
        } else if constexpr (kj::isSameType<Type, ValueIteratorType>()) {
          return js.resolvedPromise<kj::Maybe<Type>>(entry.addRef());
        } else {
          static_assert(false, "invalid iterator type");
        }
      }
      KJ_CASE_ONEOF(exception, jsg::JsRef<jsg::JsValue>) {
        return js.rejectedPromise<kj::Maybe<Type>>(exception.getHandle(js));
      }
    }
    KJ_UNREACHABLE;
  }

  template <typename Type>
  static jsg::Promise<void> iteratorReturn(
      jsg::Lock& js, IteratorState& state, jsg::Optional<Type>& value) {
    return js.resolvedPromise();
  }
};

class FileSystemWritableFileStream final: public WritableStream {
 public:
  struct State final: public kj::Refcounted {
    const workerd::VirtualFileSystem& vfs;

    // The FileSystemWritableFileStream is a bit transactional in nature.
    // All writes are done to a temporary file. When the stream is closed
    // without an error, the original file data is replaced with the data
    // in the temporary buffer. If the stream is aborted, the temporary file
    // is deleted and the original contents are left intact.
    // The temporary file is not visible to the user and is not included in
    // any directory. It is only used to hold the data until the stream
    // is closed.
    kj::Maybe<kj::Rc<workerd::File>> temp;

    // The file handle we are writing to
    jsg::Ref<FileSystemFileHandle> file;

    kj::Maybe<kj::Own<void>> lock;

    // A note on file position and sizes. In the spec, file sizes and positions
    // are defined in terms of unsigned long long, or 64-bits. We are using
    // unsigned long (uint32_t) for these instead. This is largely because
    // the underlying implementation is holding the file in memory and 4 GB
    // is WAY more than our isolate heap limit. A uint32_t is more than enough
    // for our purposes.
    uint32_t position = 0;
    State(jsg::Lock& js,
        const workerd::VirtualFileSystem& vfs,
        jsg::Ref<FileSystemFileHandle> file,
        kj::Rc<workerd::File> temp)
        : vfs(vfs),
          temp(kj::mv(temp)),
          file(kj::mv(file)),
          lock(vfs.lock(js, this->file->getLocator())) {}

    void clear() {
      temp = kj::none;
      position = 0;
      lock = kj::none;
    }
  };

  FileSystemWritableFileStream(
      kj::Own<WritableStreamController> controller, kj::Rc<State> sharedState);

  static jsg::Ref<FileSystemWritableFileStream> constructor() = delete;

  using WriteParams = FileSystemFileWriteParams;

  jsg::Promise<void> write(jsg::Lock& js,
      FileSystemWritableData data,
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

  static jsg::Promise<void> writeImpl(jsg::Lock& js,
      FileSystemWritableData data, State& state,
      const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler);

 private:
  kj::Rc<State> sharedState;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(sharedState->file);
  }
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
      workerd::api::StorageManager,                                                                \
      workerd::api::FileSystemFileHandle::FileSystemCreateWritableOptions,                         \
      workerd::api::FileSystemDirectoryHandle::FileSystemGetFileOptions,                           \
      workerd::api::FileSystemDirectoryHandle::FileSystemGetDirectoryOptions,                      \
      workerd::api::FileSystemDirectoryHandle::FileSystemRemoveOptions,                            \
      workerd::api::FileSystemWritableFileStream::WriteParams,                                     \
      workerd::api::FileSystemDirectoryHandle::EntryIterator,                                      \
      workerd::api::FileSystemDirectoryHandle::KeyIterator,                                        \
      workerd::api::FileSystemDirectoryHandle::ValueIterator,                                      \
      workerd::api::FileSystemDirectoryHandle::EntryIterator::Next,                                \
      workerd::api::FileSystemDirectoryHandle::KeyIterator::Next,                                  \
      workerd::api::FileSystemDirectoryHandle::ValueIterator::Next,                                \
      workerd::api::FileSystemHandle::RemoveOptions

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
