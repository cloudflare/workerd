#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>

#include <kj/common.h>
#include <kj/refcount.h>
#include <kj/time.h>

// Every worker instance will have its own root directory (/). In this root
// directory we will have at least three special directories, the "bundle" root,
// the "dev" root, and the "temp" root. More special directories can be added
// later.
//
// The bundle root is where all of the modules that are included in the worker
// bundle will be accessible. Everything in this directory will be strictly
// read-only. The contents are populated by the worker configuration bundle.
// By default, the bundle root will be /bundle but this can be overridden
// using the FsMap API defined below.
//
// The dev root is where special "device" files will be accessible. For example,
// the /dev/null, /dev/zero, /dev/full, and /dev/random files will be here.
// By default these are in /dev but the location can be overridden using the
// FsMap API.
//
// The temp root is where we will allow temporary files to be created.
// Everything in this directory is read-write but will be transient. By default,
// the temp root will be /tmp but this can also be overridden.
//
// Let's imagine the following simple workerd configuration:
//
// ```
// const helloWorld :Workerd.Worker = (
//   modules = [
//     (name = "worker",
//      esModule = embed "worker.js"),
//     (name = "foo", text = "Hello World!"),
//   ],
//   compatibilityDate = "2023-02-28",
// );
// ```
//
// Given this configuration, the worker fs will initially have the following
// structure:
//
// /
// ├── bundle
// │   ├── worker
// │   └── foo
// ├── dev
// │   ├── null
// │   ├── zero
// │   ├── full
// │   └── random
// └── tmp
//
// We can access the filesystem using VirtualFileSystem::current():
//
// ```cpp
// jsg::Lock& js = ...
// auto& vfs = VirtualFileSystem::current(js);
// // Resolve the root of the file system
// KJ_IF_SOME(node, vfs.resolve(js, "file:///path/to/thing"_url)) {
//   KJ_SWITCH_ONEOF(node) {
//     KJ_CASE_ONEOF(file, kj::Rc<File>) {
//       // ...
//     }
//     KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
//       // ...
//     }
//     KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
//       // ...
//     }
//   }
// }
// ```
//
// The temporary file directory is a bit special in that the contents are fully
// transient based on whether there is an active IoContext or not. We use a
// special RAII TmpDirStoreScope scope to manage the contents of the temporary
// directory. For example,
//
// ```cpp
// KJ_IF_SOME(node, vfs.resolve("file:///tmp"_url)) {
//   auto& dir = KJ_ASSERT_NONNULL(node.tryGet<kj::Rc<Directory>>());
//   TmpDirStoreScope temp_dir_scope;
//   kj::Path path("a/b/c/foo.txt")
//   auto tmpFile = dir.tryOpen(js, path, { FsType::FILE });
//   KJ_ASSERT(tmpFile.write(js, 0, "Hello World!"_kjb) == 12);
//   // The temp dir scope is destructed and the file is deleted
// }
// ```
//
// If there is an active IoContext, the temporary file will instead be created
// within that IoContext's TmpDirStoreScope, and will be deleted when the
// IoContext is destructed. This allows us to have a single virtual file
// system whose temporary directories are either deleted immediately as soon
// as the execution scope is exited, or are specific to the IoContext and are
// deleted when the IoContext is destructed. This mechanism allows us to have
// multiple IoContexts active at the same time while still having a single
// virtual file system whose contents correctly reflect the current IoContext.
//
// Note that operations on files and directories require having a jsg::Lock&.
// This is used for several purposes. One, for writable files it is used to
// provide access to the memory accounting systen, ensuring that the memory
// held by writable files is appropriately accounted for towards to isolate
// heap limit. Second, use of the jsg::Lock& ensures that file system mutations
// are performed in a thread-safe manner -- specifically, we use it to ensure
// that only one thread is allowed to access the file system at a time since
// only one thread at a time can hold the jsg::Lock.
//
// The design here is intended to be extensible. We can add new root directories
// in the future with different semantics and implementations. For example, to
// support python workers, for instance, we can introduce a new root directory
// that is backed by a tar/zip file containing the python standard library, etc.
//
// To support the implementation of node:fs the virtual file system needs a
// concept of file descriptors that map somewhat cleanly to the posix notion
// of file descriptors. This is a bit tricky because fds are a finite resource.
// The VFS implementation uses a notion of file descriptors that are scoped
// specifically to the worker... that is, fd 1 in one worker is not the same as
// fd 1 in another. It will be a requirement that fd's are closed explicitly or
// they may still "leak" beyond the scope where they are used, but when the
// worker is torn down, all associated file descriptors will be automatically
// destroyed/closed rather than leaking for the entire process. We use just a
// simple in-memory table to track the file descriptors and their associated
// files/directories.

namespace workerd {

// TODO(node-fs): Currently, all files and directories use a fixed last
// modified time set to the Unix epoch. This is temporary.

enum class FsType {
  FILE,
  DIRECTORY,
  SYMLINK,
};

// Metadata about this filesystem node
struct Stat final {
  FsType type = FsType::FILE;

  // The size of the node in bytes. For directories, this value will
  // always be 0. Note that we are intentionally limiting the size of
  // files to max(uint32_t) or 4GB. This is well above the maximum
  // isolate heap limit so in-memory files should generally never
  // get this large.
  uint32_t size = 0;

  // The last modified time of the node.
  kj::Date lastModified = kj::UNIX_EPOCH;

  // The creation time of the node.
  kj::Date created = kj::UNIX_EPOCH;

  // Indicates if the node is writable.
  bool writable = false;

  // Indicates if the node is a device. Certain operations
  // may behave differently on devices.
  bool device = false;
};

class SymbolicLink;

enum class FsError {
  // Path segment is not a directory
  NOT_DIRECTORY,
  // Directory is not empty
  NOT_EMPTY,
  // Node is read-only
  READ_ONLY,
  // Not permitted
  NOT_PERMITTED,
  // Not permitted on directory
  NOT_PERMITTED_ON_DIRECTORY,
  // Already exists
  ALREADY_EXISTS,
  // Too many open files
  TOO_MANY_OPEN_FILES,
  // Operation failed
  FAILED,
  // Not supported
  NOT_SUPPORTED,
  // Invalid path
  INVALID_PATH,
  // Exceeds file size limit
  FILE_SIZE_LIMIT_EXCEEDED,
  // Symlink depth exceeded
  SYMLINK_DEPTH_EXCEEDED,
};

// A file in the virtual file system. If the file is read-only, then the
// mutation methods will throw an exception.
//
// A file is writable if it owns its contents. In such cases, the memory
// allocation is tracked by the corresponding isolate memory accounting.
class File: public kj::Refcounted {
 public:
  // Attempt to set the last modified time of this node. If the node is
  // read-only, this will be a non-op.
  virtual kj::Maybe<FsError> setLastModified(jsg::Lock& js, kj::Date date) = 0;

  // Returns the metadata for this node.
  virtual Stat stat(jsg::Lock& js) KJ_WARN_UNUSED_RESULT = 0;

  // Reads all the contents of the file as a string.
  kj::OneOf<FsError, jsg::JsString> readAllText(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;

  // Reads all the contents of the file as a Uint8Array.
  kj::OneOf<FsError, jsg::BufferSource> readAllBytes(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;

  // Reads data from the file at the given offset into the given buffer.
  virtual uint32_t read(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<kj::byte> buffer) const = 0;

  // Replaces the full contents of the file with the given data.
  // Equivalent to resize(js, data.size()) followed by write(js, 0, data).
  kj::OneOf<FsError, uint32_t> writeAll(
      jsg::Lock& js, kj::ArrayPtr<const kj::byte> data) KJ_WARN_UNUSED_RESULT {
    KJ_IF_SOME(err, resize(js, data.size())) {
      return err;
    }
    return write(js, 0, data);
  }

  // Replaces the full contents of the file with the given data.
  // Equivalent to resize(js, data.size()) followed by write(js, 0, data).
  kj::OneOf<FsError, uint32_t> writeAll(jsg::Lock& js, kj::StringPtr data) KJ_WARN_UNUSED_RESULT {
    return writeAll(js, data.asBytes());
  }

  // Writes data to the file at the given offset. Returns the number of bytes
  // written. Returns the number of bytes written. If the file is not writable,
  // this will throw an exception. If the offset is greater than the current
  // size of the file, the file may be resized to accommodate the new data or
  // an exception may be thrown (depending on the underlying implementation).
  virtual kj::OneOf<FsError, uint32_t> write(
      jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> data) KJ_WARN_UNUSED_RESULT = 0;

  kj::OneOf<FsError, uint32_t> write(
      jsg::Lock& js, uint32_t offset, kj::StringPtr data) KJ_WARN_UNUSED_RESULT {
    return write(js, offset, data.asBytes());
  }

  // Fill the file with the given value from the given offset. This is more
  // efficient that writing the same value as it does not require any allocations.
  virtual kj::Maybe<FsError> fill(
      jsg::Lock& js, kj::byte val, kj::Maybe<uint32_t> offset = kj::none) KJ_WARN_UNUSED_RESULT = 0;

  // Resize the file allocation. Note that this is potentially an expensive
  // operation as it requires allocating a new internal buffer and copying
  // the data. If the size is smaller than the current size, the contents of
  // the fill will be truncated. If the size is larger than the current size,
  // the new contents of the file will be filled with zeroes.
  virtual kj::Maybe<FsError> resize(jsg::Lock& js, uint32_t size) KJ_WARN_UNUSED_RESULT = 0;

  // Creates a new readable/writable in-memory file. This file will not be
  // initially included in a directory. To add it to a directory, use the
  // add method. If the file is not added, it will be deleted
  // when the handle is dropped. If size is given, the file will be initially
  // filled with zeroes up to the given size, otherwise the file will be empty.
  // The contents of the file will be tracked and counted towards the isolate
  // external memory usage.
  // If size is not given, the file will be empty. If the intent is to perform
  // multiple writes to the file, it is recommended to specify a size up front
  // or to resize the file as appropriate to account for the expected writes.
  // This will avoid each individual write causing a reallocation of the
  // internal buffer to accommodate the new data.
  static kj::Rc<File> newWritable(
      jsg::Lock& js, kj::Maybe<uint32_t> size = kj::none) KJ_WARN_UNUSED_RESULT;

  // Creates a new readable in-memory file wrapping the given data. The file
  // does not take ownership of the data and the data must remain valid for the
  // lifetime of the file. The file will be read-only. It will not be initially
  // included in a directory. The contents of the file will not be tracked and
  // will not count towards the isolate external memory usage.
  static kj::Rc<File> newReadable(kj::ArrayPtr<const kj::byte> data) KJ_WARN_UNUSED_RESULT;

  virtual kj::StringPtr jsgGetMemoryName() const = 0;
  virtual size_t jsgGetMemorySelfSize() const = 0;
  virtual void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const = 0;

  // Creates a copy of this file.
  virtual kj::OneOf<FsError, kj::Rc<File>> clone(jsg::Lock& js) KJ_WARN_UNUSED_RESULT = 0;

  // Replaces the contents of this file with the given file if possible.
  // If this file is read-only, an exception will be thrown.
  virtual kj::Maybe<FsError> replace(jsg::Lock& js, kj::Rc<File> file) KJ_WARN_UNUSED_RESULT = 0;

  // Returns a UUID that uniquely identifies this fs node. The value stable across
  // multiple calls to getUniqueId() on the same node, but is not guaranteed to
  // be stable across worker restarts. This is primarily useful for implementing
  // the FileSystemHandle.getUniqueId() API, which is not yet fully standardized
  // but is being implemented and has Web Platform Tests.
  virtual kj::StringPtr getUniqueId(jsg::Lock&) const = 0;
};

// A directory in the virtual file system. If the directory is read-only,
// then the mutation methods will throw an exception.
class Directory: public kj::Refcounted, public kj::EnableAddRefToThis<Directory> {
 public:
  // Returns the metadata for this node.
  virtual kj::Maybe<kj::OneOf<FsError, Stat>> stat(
      jsg::Lock& js, kj::PathPtr ptr) KJ_WARN_UNUSED_RESULT = 0;
  Stat stat(jsg::Lock& js) KJ_WARN_UNUSED_RESULT {
    // In this case, stat is guaranteed to succeed, so skip the error checking.
    return KJ_ASSERT_NONNULL(stat(js, nullptr)).get<Stat>();
  }

  // Return the number of entries in this directory. If typeFilter is provided,
  // only entries matching the given type will be counted.
  virtual size_t count(
      jsg::Lock& js, kj::Maybe<FsType> typeFilter = kj::none) KJ_WARN_UNUSED_RESULT = 0;

  using Item = kj::OneOf<kj::Rc<File>, kj::Rc<Directory>, kj::Rc<SymbolicLink>>;
  using Entry = kj::HashMap<kj::String, Item>::Entry;
  virtual Entry* begin() = 0;
  virtual Entry* end() = 0;
  virtual const Entry* begin() const = 0;
  virtual const Entry* end() const = 0;

  struct OpenOptions {
    // When set, if the path does not exist, we will attempt to create the
    // node as the specified type. Type must be one of either FILE or DIRECTORY.
    // Specifying SYMBOLICLINK as the type will throw an exception.
    kj::Maybe<FsType> createAs;

    // If the path points to a symbolic link, by default the link will be followed
    // such that if the link points to a valid node, that node will be returned.
    // If followLinks is false, however, the symbolic link itself will be returned.
    bool followLinks = true;
  };

  // Tries opening the file or directory at the given path. If the node does
  // not exist, and createAs is provided specifying a create mode, the node
  // will be created as the specified type; otherwise kj::none is returned
  // if the node does not exist. If the directory is read only and create is
  // specified, an exception will be thrown.
  // The path must be relative to the this directory.
  // Note that when creating files using this method, the newly created file
  // will have a size of 0 bytes. The file will be writable but each write
  // could end up causing a new allocation. It the intent is to perform
  // multiple writes, it is recommended to resize the file to the expected
  // size up front or to use the newWritable method to create a file with the
  // expected size and add it into the target directory.
  //
  // Note that tryOpen doubles as both an existence check and a stat operation.
  // When the createAs parameter is not specified, the method will return
  // kj::none if the file or directory does not exist.
  //
  // If the path identifies a symbolic link, the symbolic link will be resolved
  // and the target, if it exists, will be returned. If the target does not exist,
  // kj::none will be returned.
  virtual kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>, kj::Rc<SymbolicLink>>>
  tryOpen(jsg::Lock& js,
      kj::PathPtr path,
      OpenOptions options = {kj::none, true}) KJ_WARN_UNUSED_RESULT = 0;

  // Attempts to move the given file or directory this directory with the given
  // name. If the name already exists an exception will be thrown. The name must
  // not contain any path separators. If this directory is read only, an exception
  // will be thrown.
  virtual kj::Maybe<FsError> add(
      jsg::Lock& js, kj::StringPtr name, Item entry) KJ_WARN_UNUSED_RESULT = 0;

  struct RemoveOptions {
    // If true and the node is a directory, remove all entries in the directory.
    bool recursive = false;
  };

  // Tries to remove the file, directory, or symlink at the given path. If the
  // node does not exist, true will be returned. If the node is a directory and
  // the recursive option is not set, an exception will be thrown if the directory
  // is not empty. If the directory is read only, an exception will be thrown.
  // If the node is a file, it will be removed regardless of the recursive option
  // if the directory is not read only.
  // If the node is a symlink, the symlink will be removed but the target will
  // be left intact.
  // The path must be relative to the current directory.
  // Note that this method will only remove the node from this directory. If the
  // node has references in other directories, those will not be removed.
  // If the target is a symbolic link, the symbolic link will be removed but
  // the target will not be removed.
  virtual kj::OneOf<FsError, bool> remove(
      jsg::Lock& js, kj::PathPtr path, RemoveOptions options = {false}) KJ_WARN_UNUSED_RESULT = 0;

  virtual kj::StringPtr jsgGetMemoryName() const = 0;
  virtual size_t jsgGetMemorySelfSize() const = 0;
  virtual void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const = 0;

  // Creates a new readable/writable in-memory directory. This directory will
  // not be initially included in a directory. To add it to a directory, use the
  // add method. If the directory is not added, it will be deleted
  // when the handle is dropped.
  static kj::Rc<Directory> newWritable() KJ_WARN_UNUSED_RESULT;

  // Used to build a new read-only directory. All files and directories added
  // may or may not be writable. The directory will not be initially included
  // in a directory. To add it to a directory, use the add method.
  class Builder {
   public:
    Builder() = default;
    KJ_DISALLOW_COPY_AND_MOVE(Builder);

    // Adds a file or directory to the directory. The name must not contain any
    // path separators. If the name already exists an exception is thrown.
    void add(kj::StringPtr name, kj::OneOf<kj::Rc<File>, kj::Rc<Directory>> fileOrDirectory);

    // Adds a directory builder to the directory. This is used to incrementally build
    // up read-only directories. When finish is called, the directory will be
    // finalized. The name must not contain any path separators.
    void add(kj::StringPtr name, kj::Own<Directory::Builder> dir);

    // Adds a file or directory at the given path. The path may contain path separators.
    // Subdirectories will be created as needed (as read-only directories).
    void addPath(kj::PathPtr path, kj::OneOf<kj::Rc<File>, kj::Rc<Directory>> fileOrDirectory);

    // Finalizes and returns the directory. The directory will not be writable.
    // The directory will not be initially included in a directory. To add it
    // to a directory, use the add method.
    kj::Rc<Directory> finish() KJ_WARN_UNUSED_RESULT;

    using Map = kj::HashMap<kj::String,
        kj::OneOf<kj::Rc<File>, kj::Rc<Directory>, kj::Own<Directory::Builder>>>;
    using Entry = Map::Entry;

   private:
    Map entries;
  };

  // Returns a UUID that uniquely identifies this fs node. The value stable across
  // multiple calls to getUniqueId() on the same node, but is not guaranteed to
  // be stable across worker restarts. This is primarily useful for implementing
  // the FileSystemHandle.getUniqueId() API, which is not yet fully standardized
  // but is being implemented and has Web Platform Tests.
  virtual kj::StringPtr getUniqueId(jsg::Lock&) const = 0;
};

// The equivalent to a symbolic link. A symlink holds a reference to the
// virtual file system and a target path. The target node may or may not
// exist, can be deleted and recreated at any time and the symlink will
// remain valid.
class SymbolicLink final: public kj::Refcounted {
 public:
  SymbolicLink(kj::Rc<Directory> root, kj::Path targetPath)
      : root(kj::mv(root)),
        targetPath(kj::mv(targetPath)) {}
  KJ_DISALLOW_COPY_AND_MOVE(SymbolicLink);

  // Gets the stat for the symbolic link itself.
  Stat stat(jsg::Lock& js) KJ_WARN_UNUSED_RESULT;

  // Resolves the symbolic link into a file or directory.
  kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>> resolve(
      jsg::Lock& js) KJ_WARN_UNUSED_RESULT;

  // Returns the target path of the symbolic link.
  kj::PathPtr getTargetPath() const KJ_WARN_UNUSED_RESULT {
    return targetPath;
  }

  jsg::Url getTargetUrl() const KJ_WARN_UNUSED_RESULT;

  // Returns a UUID that uniquely identifies this fs node. The value stable across
  // multiple calls to getUniqueId() on the same node, but is not guaranteed to
  // be stable across worker restarts. This is primarily useful for implementing
  // the FileSystemHandle.getUniqueId() API, which is not yet fully standardized
  // but is being implemented and has Web Platform Tests.
  kj::StringPtr getUniqueId(jsg::Lock&) const;

 private:
  kj::Rc<Directory> root;
  kj::Path targetPath;
  mutable kj::Maybe<kj::String> maybeUniqueId;
};

using FsNode = kj::OneOf<kj::Rc<File>, kj::Rc<Directory>, kj::Rc<SymbolicLink>>;
using FsNodeWithError = kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>, kj::Rc<SymbolicLink>>;
class FsMap;

// The virtual file system interface. This is the main entry point for accessing the vfs.
class VirtualFileSystem {
 public:
  virtual ~VirtualFileSystem() noexcept(false) {}

  // The observer interface is used to allow the runtime to observe opening
  // or closing of file descriptors in order to allow the runtime to keep
  // an eye on the number of open file descriptors and take action if needed.
  class Observer {
   public:
    virtual ~Observer() = default;

    // openFds is the number of currently open file descriptors, including
    // the one that was just opened. totalFds is the total number of file
    // descriptors that have been opened total.
    virtual void onOpen(size_t openFds, size_t totalFds) const {
      // By default, do nothing.
    }

    // openFds is the number of currently open file descriptors, excluding
    // the one that was just closed. totalFds is the total number of file
    // descriptors that have been opened total.
    virtual void onClose(size_t openFdCount, size_t totalFds) const {
      // By default, do nothing.
    }

    // Called when the total maximum number of file descriptors the worker
    // is allowed to open is reached. After this point, the worker will no
    // longer be allowed to open any new file descriptors.
    virtual void onMaxFds(size_t openFdCount) const {
      // By default, do nothing.
    }
  };

  // The root of the virtual file system.
  virtual kj::Rc<Directory> getRoot(jsg::Lock& js) const KJ_WARN_UNUSED_RESULT = 0;

  struct ResolveOptions {
    bool followLinks = true;
  };

  // Resolves the given file URL into a file or directory.
  kj::Maybe<FsNodeWithError> resolve(jsg::Lock& js,
      const jsg::Url& url,
      ResolveOptions options = {true}) const KJ_WARN_UNUSED_RESULT;

  // Resolves the given file URL into metadata for a file or directory.
  kj::Maybe<kj::OneOf<FsError, Stat>> resolveStat(
      jsg::Lock& js, const jsg::Url& url) const KJ_WARN_UNUSED_RESULT;

  // Creates a new symbolic link to the given target path. The target path
  // does not need to exist.
  kj::Rc<SymbolicLink> newSymbolicLink(
      jsg::Lock& js, const jsg::Url& url) const KJ_WARN_UNUSED_RESULT;

  // Return the configured root paths for the bundle, temp, and dev directories.
  virtual const jsg::Url& getBundleRoot() const KJ_WARN_UNUSED_RESULT = 0;
  virtual const jsg::Url& getTmpRoot() const KJ_WARN_UNUSED_RESULT = 0;
  virtual const jsg::Url& getDevRoot() const KJ_WARN_UNUSED_RESULT = 0;

  // Get the current virtual file system for the current isolate lock.
  static const VirtualFileSystem& current(jsg::Lock&) KJ_WARN_UNUSED_RESULT;

  // ==========================================================================
  // File Descriptor support

  struct OpenOptions {
    // Open the file descriptor for reading.
    bool read = true;
    // Open the file descriptor for writing.
    bool write = false;
    // Open the file descriptor for appending. Ignored if write is false.
    bool append = false;

    // If true, opening the path will fail if it already exists.
    bool exclusive = false;

    // If true, and the destination is a symbolic link, the link will be
    // followed such that the file descriptor is opened on the target
    // of the symbolic link. If false, the file descriptor will be opened
    // on the symbolic link itself.
    bool followLinks = true;
  };

  // Represents an opened file descriptor.
  struct OpenedFile: public kj::Refcounted {
    // The file descriptor for the opened file.
    int fd;
    // The file descriptor was opened for reading.
    bool read;
    // The file descriptor was opened for writing.
    bool write;
    // The file descriptor was opened for appending (ignored if write is false).
    bool append;
    // The actual file, directory, or symlink that was opened.
    FsNode node;

    OpenedFile(int fd, bool read, bool write, bool append, FsNode node)
        : fd(fd),
          read(read),
          write(write),
          append(append),
          node(kj::mv(node)) {}

    // When reading from or writing to the file, if an offset is not
    // explicitly given then the offset will be set to the current
    // position in the file.
    uint32_t position = 0;
  };

  // Attempts to open a file descriptor for the given file URL. It's critical
  // to understand that the file descriptor table is shared for the entire
  // worker. This means that if a file descriptor is opened, it will remain
  // open until it is closed or the worker is terminated. If too many file
  // descriptors are left open the worker may run out of file descriptors and
  // fail to open new files. Additionally, opening too many file descriptors
  // may cause a production worker to be condemned in the system. There is a
  // strict upper limit on the number of file descriptors that can be opened
  // total. Once that limit is reached, all subsequent attempts to open a
  // file descriptor will fail.
  //
  // If the file cannot be opened or created, an exception will be thrown.
  virtual kj::OneOf<FsError, kj::Rc<OpenedFile>> openFd(jsg::Lock& js,
      const jsg::Url& url,
      OpenOptions options = {true, false, false, false, true}) const KJ_WARN_UNUSED_RESULT = 0;

  // Closes the given file descriptor. This is a no-op if the file descriptor is not open.
  // Using an int fd is not super nice but it is the most compatible with the node:fs
  // API and posix in general.
  virtual void closeFd(jsg::Lock& js, int fd) const = 0;

  // Returns an opaque RAII handle that wraps a file descriptor.
  // When it is dropped, the file descriptor will be closed. The handle
  // holds a weak reference to the VirtualFileSystem so that, on the off
  // chance the VirtualFileSystem is destroyed while the handle is still
  // alive, destruction of the handle will become a no-op.
  virtual kj::Own<void> wrapFd(jsg::Lock& js, int fd) const KJ_WARN_UNUSED_RESULT = 0;

  // Attempts to get the opened file, directory, or symlink for the given file descriptor.
  // Returns kj::none if the fd is not opened/known.
  virtual kj::Maybe<kj::Rc<OpenedFile>> tryGetFd(
      jsg::Lock& js, int fd) const KJ_WARN_UNUSED_RESULT = 0;
};

kj::Own<VirtualFileSystem> newVirtualFileSystem(kj::Own<FsMap> fsMap,
    kj::Rc<Directory>&& root,
    kj::Own<VirtualFileSystem::Observer> observer = kj::heap<VirtualFileSystem::Observer>())
    KJ_WARN_UNUSED_RESULT;

// The FsMap is a configurable mapping of built-in "known" file system
// paths to user-configurable locations. It is used to allow user-specified
// "mount" points for certain built-in file system paths. For example, the
// WorkerFileSystem exposes a built-in bundle path that is located at /bundle
// by default. The FsMap allows the user to specify a different name for this
// path, such as /mybundle, or even nested paths like /mybundle/a/b/c/modules/.
// To add a new known root, add the name to the KNOWN_VFS_ROOTS macro and
// specify the default path. The relevant API methods on the FsMap class
// will be generated via the template.
#define KNOWN_VFS_ROOTS(V)                                                                         \
  V(Bundle, "file:///bundle/")                                                                     \
  V(Temp, "file:///tmp/")                                                                          \
  V(Dev, "file:///dev/")

class FsMap final {
 public:
  FsMap() = default;
  KJ_DISALLOW_COPY_AND_MOVE(FsMap);

#define DEFINE_METHODS_FOR_ROOTS(name, _)                                                          \
  static const jsg::Url kDefault##name##Path;                                                      \
  const jsg::Url& get##name##Root() const {                                                        \
    return maybe##name##Root.orDefault(kDefault##name##Path);                                      \
  }                                                                                                \
  const kj::Path get##name##Path() const {                                                         \
    auto path = kj::str(get##name##Root().getPathname().slice(1));                                 \
    return kj::Path::parse(path);                                                                  \
  }                                                                                                \
  void set##name##Root(jsg::Url url) {                                                             \
    KJ_REQUIRE(url.getProtocol() == "file:"_kj, "url must be a file URL");                         \
    maybe##name##Root = kj::mv(url);                                                               \
  }                                                                                                \
  void set##name##Root(kj::StringPtr path) {                                                       \
    auto url = KJ_REQUIRE_NONNULL(jsg::Url::tryParse(path, "file:///"_kj), "invalid path");        \
    set##name##Root(kj::mv(url));                                                                  \
  }

  KNOWN_VFS_ROOTS(DEFINE_METHODS_FOR_ROOTS)

#undef DEFINE_METHODS_FOR_ROOTS

 private:
#define DEFINE_FIELDS_FOR_ROOTS(name, _) kj::Maybe<jsg::Url> maybe##name##Root;
  KNOWN_VFS_ROOTS(DEFINE_FIELDS_FOR_ROOTS)
#undef DEFINE_FIELDS_FOR_ROOTS
};

// An RAII object that stores the temporary directory items.
// Instances can either live on the stack (in which case they
// will be set in a thread-local and hasCurrent() will return
// true, or they can be created on the heap and held (e.g. in
// IoContext).
class TmpDirStoreScope final {
 public:
  static bool hasCurrent();
  static TmpDirStoreScope& current();
  TmpDirStoreScope(kj::Maybe<kj::Badge<TmpDirStoreScope>> guard = kj::none);
  KJ_DISALLOW_COPY_AND_MOVE(TmpDirStoreScope);
  ~TmpDirStoreScope() noexcept(false);

  static kj::Own<TmpDirStoreScope> create();

  kj::Rc<Directory> getDirectory() const {
    return dir.addRef();
  }

  kj::PathPtr getCwd() const {
    return kj::PathPtr(cwd);
  }

  void setCwd(kj::Path newCwd) {
    cwd = kj::mv(newCwd);
  }

 private:
  mutable kj::Rc<Directory> dir;
  kj::Path cwd;
  bool onStack = false;
};

// A scope utility that is used to guard against infinite recursion when
// resolving symbolic links. This should only ever be stack allocated and
// should never be shared outside of the current execution scope.
class SymbolicLinkRecursionGuardScope final {
 public:
  SymbolicLinkRecursionGuardScope();
  ~SymbolicLinkRecursionGuardScope() noexcept(false);
  KJ_DISALLOW_COPY_AND_MOVE(SymbolicLinkRecursionGuardScope);

  // Whenever a symbolic link is resolved, it needs to be checked against
  // the recursion guard. If the link has already been seen, an exception
  // will be thrown. If the link has not been seen, it will be recorded
  // as seen so that it can be checked against in the future.
  static kj::Maybe<FsError> checkSeen(SymbolicLink* link) KJ_WARN_UNUSED_RESULT;

 private:
  kj::HashSet<SymbolicLink*> linksSeen;
};

// Every Worker instance has its own virtual filesystem. At a minimum, this
// filesystem contains the worker's own bundled modules/files and a temporary
// in-memory directory for the worker to use. The filesystem is not shared
// between workers. The bundle delegate is a virtual directory delegate that
// provides the directory structure for the worker's bundle.
kj::Own<VirtualFileSystem> newWorkerFileSystem(kj::Own<FsMap> fsMap,
    kj::Rc<Directory> bundleDirectory,
    kj::Own<VirtualFileSystem::Observer> observer = kj::heap<VirtualFileSystem::Observer>())
    KJ_WARN_UNUSED_RESULT;

// Exposed only for testing purposes.
kj::Rc<Directory> getTmpDirectoryImpl() KJ_WARN_UNUSED_RESULT;

// Returns a directory that is lazily loaded on first access.
kj::Rc<Directory> getLazyDirectoryImpl(
    kj::Function<kj::Rc<Directory>()> func) KJ_WARN_UNUSED_RESULT;

kj::Rc<File> getDevNull() KJ_WARN_UNUSED_RESULT;
kj::Rc<File> getDevZero() KJ_WARN_UNUSED_RESULT;
kj::Rc<File> getDevFull() KJ_WARN_UNUSED_RESULT;
kj::Rc<File> getDevRandom() KJ_WARN_UNUSED_RESULT;
kj::Rc<Directory> getDevDirectory() KJ_WARN_UNUSED_RESULT;

// Helper functions for current working directory management
kj::PathPtr getCurrentWorkingDirectory() KJ_WARN_UNUSED_RESULT;
bool setCurrentWorkingDirectory(kj::Path newCwd) KJ_WARN_UNUSED_RESULT;

}  // namespace workerd
