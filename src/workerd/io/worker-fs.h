#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>

#include <kj/common.h>
#include <kj/refcount.h>
#include <kj/time.h>

// Every worker instance will have its own root directory (/). In this root
// directory we will have at least two special directories, the "bundle" root
// and the "temp" root. More special directories can be added later.
//
// The bundle root is where all of the modules that are included in the worker
// bundle will be accessible. Everything in this directory will be strictly
// read-only. The contents are populated by the worker configuration bundle.
// By default, the bundle root will be /bundle but this can be overridden
// using the FsMap API defined below.
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
// Given this configuration, the worker fs will initially have the following structure:
//
// /
// ├── bundle
// │   ├── worker
// │   └── foo
// └── tmp
//
// We can access the filesystem using VirtualFileSystem::tryGetCurrent():
//
// ```cpp
// jsg::Lock& js = ...
// KJ_IF_SOME(vfs, VirtualFileSystem::tryGetCurrent(js)) {
// // Resolve the root of the file system
// KJ_IF_SOME(node, vfs.resolve(js, "file:///"_url)) {
//   KJ_SWITCH_ONEOF(node) {
//     KJ_CASE_ONEOF(file, kj::Rc<File>) {
//       // ...
//     }
//     KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
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
//   auto tmpFile = dir.tryOpen(js, path, FsType::FILE);
//   KJ_ASSERT(tmpFile.write(js, 0, "Hello World!"_kjb) == 12);
//   // The temp dir scope is destructed and the file is deleted
// }
// ```
//
// Note that operations on files and directories require having a jsg::Lock&.
// This is used for several purposes. One, for writable files it is used to
// provide access to the memory accounting systen, ensuring that the memory
// held by writable files is appropriately accounted for towards to isolate
// heap limit. Second, use of the jsg::Lock& ensures that file system mutations
// are performed in a thread-safe manner.
//
// However, if there is an active IoContext, the temporary file will instead be
// created within that IoContext's TmpDirStoreScope, and will be deleted when
// the IoContext is destructed. This allows us to have a single virtual file
// system whose temporary directories are either deleted immediately as soon
// as the execution scope is exited, or are specific to the IoContext and are
// deleted when the IoContext is destructed. This mechanism allows us to have
// multiple IoContexts active at the same time while still having a single
// virtual file system whose contents correctly reflect the current IoContext.
//
// Temporary files can be created, copied, etc only within the temporary
// directory. All other directories are read-only.
//
// The design here is intended to be extensible. We can add new root directories
// in the future with different semantics and implementations. For example, to
// support python workers, for instance, we can introduce a new root directory
// that is backed by a tar/zip file containing the python standard library, etc.
//
// TODO(node-fs): Implementing "file descriptors" is still currently an open
// question.

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
  // always be 0.
  size_t size = 0;

  // The last modified time of the node.
  kj::Date lastModified = kj::UNIX_EPOCH;

  // The creation time of the node.
  kj::Date created = kj::UNIX_EPOCH;

  // Indicates if the node is writable.
  bool writable = false;
};

class SymbolicLink;

// A file in the virtual file system. If the file is read-only, then the
// mutation methods will throw an exception.
//
// A file is writable if it owns its contents. In such cases, the memory
// allocation is tracked by the corresponding isolate memory accounting.
class File: public kj::Refcounted {
 public:
  // Attempt to set the last modified time of this node. If the node is
  // read-only, this will be a non-op.
  virtual void setLastModified(jsg::Lock& js, kj::Date date) = 0;

  // Returns the metadata for this node.
  virtual Stat stat(jsg::Lock& js) = 0;

  // Reads all the contents of the file as a string.
  jsg::JsString readAllText(jsg::Lock& js);

  // Reads all the contents of the file as a Uint8Array.
  jsg::BufferSource readAllBytes(jsg::Lock& js);

  // Reads data from the file at the given offset into the given buffer.
  virtual size_t read(jsg::Lock& js, size_t offset, kj::ArrayPtr<kj::byte> buffer) const = 0;

  // Replaces the full contents of the file with the given data.
  // Equivalent to resize(js, data.size()) followed by write(js, 0, data).
  size_t writeAll(jsg::Lock& js, kj::ArrayPtr<const kj::byte> data) {
    resize(js, data.size());
    return write(js, 0, data);
  }

  // Replaces the full contents of the file with the given data.
  // Equivalent to resize(js, data.size()) followed by write(js, 0, data).
  size_t writeAll(jsg::Lock& js, kj::StringPtr data) {
    return writeAll(js, data.asBytes());
  }

  // Writes data to the file at the given offset. Returns the number of bytes
  // written. Returns the number of bytes written. If the file is not writable,
  // this will throw an exception. If the offset is greater than the current
  // size of the file, the file may be resized to accommodate the new data or
  // an exception may be thrown (depending on the underlying implementation).
  virtual size_t write(jsg::Lock& js, size_t offset, kj::ArrayPtr<const kj::byte> data) = 0;

  size_t write(jsg::Lock& js, size_t offset, kj::StringPtr data) {
    return write(js, offset, data.asBytes());
  }

  // Fill the file with the given value from the given offset. This is more
  // efficient that writing the same value as it does not require any allocations.
  virtual void fill(jsg::Lock& js, kj::byte val, kj::Maybe<size_t> offset = kj::none) = 0;

  // Resize the file allocation. Note that this is potentially an expensive
  // operation as it requires allocating a new internal buffer and copying
  // the data. If the size is smaller than the current size, the contents of
  // the fill will be truncated. If the size is larger than the current size,
  // the new contents of the file will be filled with zeroes.
  virtual void resize(jsg::Lock& js, size_t size) = 0;

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
  static kj::Rc<File> newWritable(jsg::Lock& js, kj::Maybe<size_t> size = kj::none);

  // Creates a new readable in-memory file wrapping the given data. The file
  // does not take ownership of the data and the data must remain valid for the
  // lifetime of the file. The file will be read-only. It will not be initially
  // included in a directory. The contents of the file will not be tracked and
  // will not count towards the isolate external memory usage.
  static kj::Rc<File> newReadable(kj::ArrayPtr<const kj::byte> data);

  virtual kj::StringPtr jsgGetMemoryName() const = 0;
  virtual size_t jsgGetMemorySelfSize() const = 0;
  virtual void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const = 0;
};

// A directory in the virtual file system. If the directory is read-only,
// then the mutation methods will throw an exception.
class Directory: public kj::Refcounted, public kj::EnableAddRefToThis<Directory> {
 public:
  // Returns the metadata for this node.
  virtual kj::Maybe<Stat> stat(jsg::Lock& js, kj::PathPtr ptr) = 0;
  Stat stat(jsg::Lock& js) {
    return KJ_ASSERT_NONNULL(stat(js, nullptr));
  }

  // Return the number of entries in this directory. If typeFilter is provided,
  // only entries matching the given type will be counted.
  virtual size_t count(jsg::Lock& js, kj::Maybe<FsType> typeFilter = kj::none) = 0;

  using Item = kj::OneOf<kj::Rc<File>, kj::Rc<Directory>, kj::Rc<SymbolicLink>>;
  using Entry = kj::HashMap<kj::String, Item>::Entry;
  virtual Entry* begin() = 0;
  virtual Entry* end() = 0;
  virtual const Entry* begin() const = 0;
  virtual const Entry* end() const = 0;

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
  virtual kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>> tryOpen(
      jsg::Lock& js, kj::PathPtr path, kj::Maybe<FsType> createAs = kj::none) = 0;

  // Attempts to move the given file or directory into the path specified. The
  // path must be relative to the current directory. If the path already exists
  // an exception will be thrown. The name must not contain any path separators.
  // If this directory is read only, an exception will be thrown.
  virtual void add(jsg::Lock& js, kj::StringPtr name, Item entry) = 0;

  struct RemoveOptions {
    // If true and the node is a directory, remove all entries in the directory.
    bool recursive = false;
  };

  // Tries to remove the file or directory at the given path. If the node does
  // not exist, true will be returned. If the node is a directory and the recursive
  // option is not set, an exception will be thrown if the directory is not empty.
  // If the directory is read only, an exception will be thrown.
  // If the node is a file, it will be removed regardless of the recursive option
  // if the directory is not read only.
  // The path must be relative to the current directory.
  // Note that this method will only remove the node from this directory. If the
  // node as references in other directories, those will not be removed.
  // If the target is a symbolic link, the symbolic link will be removed but
  // the target will not be removed.
  virtual bool remove(
      jsg::Lock& js, kj::PathPtr path, kj::Maybe<RemoveOptions> options = kj::none) = 0;

  virtual kj::StringPtr jsgGetMemoryName() const = 0;
  virtual size_t jsgGetMemorySelfSize() const = 0;
  virtual void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const = 0;

  // Creates a new readable/writable in-memory directory. This directory will
  // not be initially included in a directory. To add it to a directory, use the
  // add method. If the directory is not added, it will be deleted
  // when the handle is dropped.
  static kj::Rc<Directory> newWritable();

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
    kj::Rc<Directory> finish();

    using Map = kj::HashMap<kj::String,
        kj::OneOf<kj::Rc<File>, kj::Rc<Directory>, kj::Own<Directory::Builder>>>;
    using Entry = Map::Entry;

   private:
    Map entries;
  };
};

// The equivalent to a symbolic link. A symlink holds a reference to both the
// virtual file system and a target path.
class SymbolicLink final: public kj::Refcounted {
 public:
  SymbolicLink(kj::Rc<Directory> root, kj::Path targetPath)
      : root(kj::mv(root)),
        targetPath(kj::mv(targetPath)) {}
  KJ_DISALLOW_COPY_AND_MOVE(SymbolicLink);

  // Gets the stat for the symbolic link itself.
  Stat stat(jsg::Lock& js);

  // Resolves the symbolic link into a file or directory.
  kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>> resolve(jsg::Lock& js);

  // Returns the target path of the symbolic link.
  kj::PathPtr getTargetPath() const {
    return targetPath;
  }

  jsg::Url getTargetUrl() const;

 private:
  kj::Rc<Directory> root;
  kj::Path targetPath;
};

class FsMap;

// The virtual file system interface. This is the main entry point for accessing the vfs.
class VirtualFileSystem {
 public:
  // The root of the virtual file system.
  virtual kj::Rc<Directory> getRoot(jsg::Lock& js) const = 0;

  // Resolves the given file URL into a file or directory.
  kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>> resolve(
      jsg::Lock& js, const jsg::Url& url) const;

  // Resolves the given file URL into metadata for a file or directory.
  kj::Maybe<Stat> resolveStat(jsg::Lock& js, const jsg::Url& url) const;

  // Creates a new symbolic link to the given target path. The target path
  // does not need to exist.
  kj::Rc<SymbolicLink> newSymbolicLink(jsg::Lock& js, const jsg::Url& url) const;

  virtual const jsg::Url& getBundleRoot() const = 0;
  virtual const jsg::Url& getTmpRoot() const = 0;

  // Get the current virtual file system for the current isolate lock.
  static kj::Maybe<const VirtualFileSystem&> tryGetCurrent(jsg::Lock&);
};

kj::Own<VirtualFileSystem> newVirtualFileSystem(kj::Own<FsMap> fsMap, kj::Rc<Directory>&& root);

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
  V(Temp, "file:///tmp/")

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
//
// By default, all file instances are created using the UnixEpochClock,
// which sets all timestamps to the Unix epoch. When creating a new
// TmpDirStoreScope, you can pass a different clock to set the
// timestamps. This is particularly important when setting timestamp
// for files created under an IoContext, in which case the we want
// to make sure the times are set to the correct time as returned by
// IoContext::now(), correctly reflecting time in for that request.
class TmpDirStoreScope final {
 public:
  static bool hasCurrent();
  static TmpDirStoreScope& current();
  TmpDirStoreScope(kj::Maybe<kj::Badge<TmpDirStoreScope>> guard = kj::none);
  KJ_DISALLOW_COPY_AND_MOVE(TmpDirStoreScope);
  ~TmpDirStoreScope() noexcept(false);

  static kj::Own<TmpDirStoreScope> create();

  kj::Rc<Directory> getDirectory() {
    return dir.addRef();
  }

 private:
  kj::Rc<Directory> dir;
  bool onStack = false;
};

// Every Worker instance has its own virtual filesystem. At a minimum, this
// filesystem contains the worker's own bundled modules/files and a temporary
// in-memory directory for the worker to use. The filesystem is not shared
// between workers. The bundle delegate is a virtual directory delegate that
// provides the directory structure for the worker's bundle.
kj::Own<VirtualFileSystem> newWorkerFileSystem(
    kj::Own<FsMap> fsMap, kj::Rc<Directory> bundleDirectory);

// Exposed only for testing purposes.
kj::Rc<Directory> getTmpDirectoryImpl();

// Returns a directory that is lazily loaded on first access.
kj::Rc<Directory> getLazyDirectoryImpl(kj::Function<kj::Rc<Directory>()> func);
}  // namespace workerd
