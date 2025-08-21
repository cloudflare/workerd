#include "worker-fs.h"

#include <workerd/io/io-context.h>
#include <workerd/io/tracer.h>
#include <workerd/util/uuid.h>
#include <workerd/util/weak-refs.h>

#include <algorithm>

namespace workerd {

#define DEFINE_DEFAULTS_FOR_ROOTS(name, path)                                                      \
  const jsg::Url FsMap::kDefault##name##Path = path##_url;
KNOWN_VFS_ROOTS(DEFINE_DEFAULTS_FOR_ROOTS)
#undef DEFINE_DEFAULTS_FOR_ROOTS

// Helper function to get the current working directory path.
// Returns the Cwd if available, otherwise returns an empty path (root).
kj::Maybe<kj::PathPtr> getCurrentWorkingDirectory() {
  if (IoContext::hasCurrent()) {
    return IoContext::current().getTmpDirStoreScope().getCwd();
  }
  if (TmpDirStoreScope::hasCurrent()) {
    return TmpDirStoreScope::current().getCwd();
  }
  return kj::none;
}

// Helper function to set the current working directory path.
// Returns false if no context is available.
bool setCurrentWorkingDirectory(kj::Path newCwd) {
  if (IoContext::hasCurrent()) {
    auto& ioContext = IoContext::current();
    ioContext.getTmpDirStoreScope().setCwd(kj::mv(newCwd));
    return true;
  } else if (TmpDirStoreScope::hasCurrent()) {
    TmpDirStoreScope::current().setCwd(kj::mv(newCwd));
    return true;
  }
  return false;
}

namespace {
// The SymbolicLinkRecursionGuardScope is used on-stack to guard against
// circular symbolic links. As soon as a cycle is detected, it throws.
// Since resolution is always synchronous, we can use thread-local to
// track the current scope, allowing multiple scopes to be in the stack
// without needed to pass the guard around or do any other bookkeeping.
thread_local SymbolicLinkRecursionGuardScope* symbolicLinkGuard = nullptr;

// Thread-local storage to track the current temp directory storage scope
// on the stack.
static thread_local TmpDirStoreScope* tmpDirStorageScope = nullptr;

// The TmpDirectory is a special directory implementation that uses the
// current TmpDirStoreScope to actually store the directory contents. The
// current TmpDirStoreScope can either be set on the stack or via the current
// IoContext. What this means is that every IoContext has it's own temporary
// directory that is deleted when the IoContext is destructed. This allows
// allows for top-level evaluations running outside of the IoContext to have
// their own temporary directory space should we decide that temp files at
// the global scope are useful.
class TmpDirectory final: public Directory {
 public:
  kj::Maybe<kj::OneOf<FsError, Stat>> stat(jsg::Lock& js, kj::PathPtr ptr) override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return kj::Maybe<kj::OneOf<FsError, Stat>>(dir->stat(js, ptr));
    }
    if (ptr.size() == 0) {
      return kj::Maybe<kj::OneOf<FsError, Stat>>(Stat{
        .type = FsType::DIRECTORY,
        .size = 0,
        .lastModified = kj::UNIX_EPOCH,
        .writable = true,
      });
    }
    return kj::none;
  }

  size_t count(jsg::Lock& js, kj::Maybe<FsType> typeFilter = kj::none) override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return dir->count(js, typeFilter);
    }
    return 0;
  }

  Entry* begin() override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return dir->begin();
    }
    return nullptr;
  }

  Entry* end() override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return dir->end();
    }
    return nullptr;
  }

  const Entry* begin() const override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return dir->begin();
    }
    return nullptr;
  }

  const Entry* end() const override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return dir->end();
    }
    return nullptr;
  }

  kj::Maybe<FsNodeWithError> tryOpen(
      jsg::Lock& js, kj::PathPtr path, OpenOptions options = {}) override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return dir->tryOpen(js, path, kj::mv(options));
    }
    return kj::none;
  }

  kj::Maybe<FsError> add(jsg::Lock& js, kj::StringPtr name, Item entry) override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return dir->add(js, name, kj::mv(entry));
    }
    return FsError::NOT_PERMITTED;
  }

  kj::OneOf<FsError, bool> remove(
      jsg::Lock& js, kj::PathPtr path, RemoveOptions options = {}) override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return dir->remove(js, path, kj::mv(options));
    }
    return false;
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "TmpDirectory"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(TmpDirectory);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    // The memory contents of this directory are not tracked. This is
    // because they are entirely dependent on the current storage scope
    // or IoContext. However, it is not likely that jsgGetMemoryInfo
    // will be called when either of these are current.
  }

  kj::StringPtr getUniqueId(jsg::Lock&) const override {
    KJ_IF_SOME(id, maybeUniqueId) {
      return id;
    }
    // Generating a UUID requires randomness, which requires an IoContext.
    JSG_REQUIRE(IoContext::hasCurrent(), Error, "Cannot generate a unique ID outside of a request");
    auto& ioContext = IoContext::current();
    maybeUniqueId = workerd::randomUUID(ioContext.getEntropySource());
    return KJ_ASSERT_NONNULL(maybeUniqueId);
  }

 private:
  mutable kj::Maybe<kj::String> maybeUniqueId;

  kj::Maybe<kj::Rc<Directory>> tryGetDirectory() const {
    if (IoContext::hasCurrent()) {
      return IoContext::current().getTmpDirStoreScope().getDirectory();
    }
    if (TmpDirStoreScope::hasCurrent()) {
      return TmpDirStoreScope::current().getDirectory();
    }
    return kj::none;
  }
};

// LazyDirectory is a directory that is lazily loaded on first access.
// It is used, for example, by bundle-fs to load the bundle directory
// from the worker configuration lazily when the directory is first
// accessed in order to avoid the cost of loading the entire bundle if
// the worker never actually accesses it.
class LazyDirectory final: public Directory {
 public:
  LazyDirectory(kj::Function<kj::Rc<Directory>()> func): lazyDir(kj::mv(func)) {}

  kj::Maybe<kj::OneOf<FsError, Stat>> stat(jsg::Lock& js, kj::PathPtr ptr) override {
    return getDirectory()->stat(js, ptr);
  }

  size_t count(jsg::Lock& js, kj::Maybe<FsType> typeFilter = kj::none) override {
    return getDirectory()->count(js, typeFilter);
  }

  Entry* begin() override {
    return getDirectory()->begin();
  }

  Entry* end() override {
    return getDirectory()->end();
  }

  const Entry* begin() const override {
    return getDirectory()->begin();
  }

  const Entry* end() const override {
    return getDirectory()->end();
  }

  kj::Maybe<FsNodeWithError> tryOpen(
      jsg::Lock& js, kj::PathPtr path, OpenOptions options = {}) override {
    return getDirectory()->tryOpen(js, path, kj::mv(options));
  }

  kj::Maybe<FsError> add(jsg::Lock& js, kj::StringPtr name, Item item) override {
    return getDirectory()->add(js, name, kj::mv(item));
  }

  kj::OneOf<FsError, bool> remove(
      jsg::Lock& js, kj::PathPtr path, RemoveOptions options = {}) override {
    return getDirectory()->remove(js, path, kj::mv(options));
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "LazyDirectory"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(LazyDirectory);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    // We will only track the contents of this directory if it has
    // be lazily loaded.
    KJ_SWITCH_ONEOF(lazyDir) {
      KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
        dir->jsgGetMemoryInfo(tracker);
        return;
      }
      KJ_CASE_ONEOF(func, kj::Function<kj::Rc<Directory>()>) {
        // We don't know the contents of this directory  If it has not, then the contents are yet.
        // Do not track.
        return;
      }
    }
  }

  kj::StringPtr getUniqueId(jsg::Lock& js) const override {
    return getDirectory()->getUniqueId(js);
  }

 private:
  mutable kj::OneOf<kj::Rc<Directory>, kj::Function<kj::Rc<Directory>()>> lazyDir;

  kj::Rc<Directory> getDirectory() const {
    KJ_SWITCH_ONEOF(lazyDir) {
      KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
        return dir.addRef();
      }
      KJ_CASE_ONEOF(func, kj::Function<kj::Rc<Directory>()>) {
        auto dir = func();
        auto ret = dir.addRef();
        lazyDir = kj::mv(dir);
        return kj::mv(ret);
      }
    }
    KJ_UNREACHABLE;
  }
};

// Validates that the given path does not contain any path separators and
// can be parsed as a single path element. Throws if the checks fail.
bool validatePathWithNoSeparators(kj::StringPtr path) {
  try {
    auto parsed = kj::Path::parse(path);
    return parsed.size() == 1;
  } catch (kj::Exception& e) {
    return false;
  }
}

// The primary implementation of the Directory interface.
template <bool Writable = false>
class DirectoryBase final: public Directory {
 public:
  DirectoryBase() = default;

  DirectoryBase(kj::HashMap<kj::String, Item> entries): entries(kj::mv(entries)) {
    // When this constructor is used, we assume that the directory is read-only.
    KJ_DASSERT(!Writable);
  }

  kj::Maybe<kj::OneOf<FsError, Stat>> stat(jsg::Lock& js, kj::PathPtr ptr) override {
    // When the path ptr size is 0, then we're looking for the stat of this directory.
    if (ptr.size() == 0) {
      return kj::Maybe<kj::OneOf<FsError, Stat>>(Stat{
        .type = FsType::DIRECTORY,
        .size = 0,
        .lastModified = kj::UNIX_EPOCH,
        .writable = Writable,
      });
    }

    // Otherwise, we need to look up the entry...
    KJ_IF_SOME(found, entries.find(ptr[0])) {
      KJ_SWITCH_ONEOF(found) {
        KJ_CASE_ONEOF(file, kj::Rc<File>) {
          // We found a file. If the remaining path is empty, yay! Return the stat.
          if (ptr.size() == 1) {
            return kj::Maybe<kj::OneOf<FsError, Stat>>(file->stat(js));
          }
          // Otherwise we'll fall through to return kj::none
        }
        KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
          // We found a directory. We can just ask it for the stat. If the path
          // ends up being empty, then that directory will return it's own stat.
          return dir->stat(js, ptr.slice(1, ptr.size()));
        }
        KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
          // We found a symbolic link. We will resolve it and, if it resolves
          // to something, we will ask it for the stat. Otherwise we return
          // kj::none.
          SymbolicLinkRecursionGuardScope guardScope;
          KJ_IF_SOME(err, guardScope.checkSeen(link.get())) {
            return kj::Maybe<kj::OneOf<FsError, Stat>>(err);
          }
          KJ_IF_SOME(resolved, link->resolve(js)) {
            KJ_SWITCH_ONEOF(resolved) {
              KJ_CASE_ONEOF(file, kj::Rc<File>) {
                return kj::Maybe<kj::OneOf<FsError, Stat>>(file->stat(js));
              }
              KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
                return dir->stat(js, ptr.slice(1, ptr.size()));
              }
              KJ_CASE_ONEOF(err, FsError) {
                return kj::Maybe<kj::OneOf<FsError, Stat>>(err);
              }
            }
            KJ_UNREACHABLE;
          }
        }
      }
    }
    return kj::none;
  }

  size_t count(jsg::Lock& js, kj::Maybe<FsType> typeFilter = kj::none) override {
    KJ_IF_SOME(type, typeFilter) {
      return std::count_if(begin(), end(), [type](const auto& entry) {
        KJ_SWITCH_ONEOF(entry.value) {
          KJ_CASE_ONEOF(file, kj::Rc<File>) {
            return type == FsType::FILE;
          }
          KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
            return type == FsType::DIRECTORY;
          }
          KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
            return type == FsType::SYMLINK;
          }
        }
        KJ_UNREACHABLE;
      });
    }
    return entries.size();
  }

  Entry* begin() override {
    return entries.begin();
  }

  Entry* end() override {
    return entries.end();
  }

  const Entry* begin() const override {
    return entries.begin();
  }

  const Entry* end() const override {
    return entries.end();
  }

  kj::Maybe<FsNodeWithError> tryOpen(
      jsg::Lock& js, kj::PathPtr path, OpenOptions opts = {}) override {
    if (path.size() == 0) {
      // An empty path ends up just returning this directory.
      return kj::Maybe<FsNodeWithError>(kj::Rc<Directory>(addRefToThis()));
    }

    KJ_IF_SOME(found, entries.find(path[0])) {
      if (path.size() == 1) {
        // We found the entry, return it.
        KJ_SWITCH_ONEOF(found) {
          KJ_CASE_ONEOF(file, kj::Rc<File>) {
            return kj::Maybe<FsNodeWithError>(file.addRef());
          }
          KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
            return kj::Maybe<FsNodeWithError>(dir.addRef());
          }
          KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
            if (!opts.followLinks) {
              // If we're not following links, when we just return the link itself
              // here.
              return kj::Maybe<FsNodeWithError>(link.addRef());
            }
            // Resolve the symbolic link and return the target, guarding against
            // recursion while doing so.
            SymbolicLinkRecursionGuardScope guardScope;
            KJ_IF_SOME(err, guardScope.checkSeen(link.get())) {
              return kj::Maybe<FsNodeWithError>(err);
            }
            return link->resolve(js);
          }
        }
        KJ_UNREACHABLE;
      }

      // There's more than one component in the path, we need to keep looking.
      path = path.slice(1, path.size());
      KJ_SWITCH_ONEOF(found) {
        KJ_CASE_ONEOF(file, kj::Rc<File>) {
          // We found a file, but we were looking for a directory.
          return kj::none;
        }
        KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
          // We found a directory, continue searching.
          return dir->tryOpen(js, path, kj::mv(opts));
        }
        KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
          // If the symbolic link resolves to a directory, then we can continue
          // searching, otherwise we return nothing.
          // Unless we're being asked to not follow links, then just return kj::none.
          if (!opts.followLinks) {
            return kj::none;
          }
          SymbolicLinkRecursionGuardScope guardScope;
          KJ_IF_SOME(err, guardScope.checkSeen(link.get())) {
            return kj::Maybe<FsNodeWithError>(err);
          }
          KJ_IF_SOME(resolved, link->resolve(js)) {
            KJ_SWITCH_ONEOF(resolved) {
              KJ_CASE_ONEOF(file, kj::Rc<File>) {
                return kj::none;
              }
              KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
                return dir->tryOpen(js, path, kj::mv(opts));
              }
              KJ_CASE_ONEOF(err, FsError) {
                return kj::Maybe<FsNodeWithError>(err);
              }
            }
          } else {
            // The symbolic link does not resolve to anything.
            return kj::none;
          }
        }
      }
    }

    // If we haven't found anything, we can try to create a new file or directory
    // if the directory is writable and the createAs parameter is set.
    if constexpr (Writable) {
      KJ_IF_SOME(type, opts.createAs) {
        return tryCreate(js, path, type);
      }
    } else {
      if (opts.createAs != kj::none) {
        return kj::Maybe<FsNodeWithError>(FsError::READ_ONLY);
      }
    }

    return kj::none;
  }

  kj::Maybe<FsError> add(jsg::Lock& js, kj::StringPtr name, Item fileOrDirectory) override {
    if constexpr (Writable) {
      if (!validatePathWithNoSeparators(name)) {
        return FsError::INVALID_PATH;
      }
      if (entries.find(name) != kj::none) {
        return FsError::ALREADY_EXISTS;
      }
      auto ret = ([&]() -> Item {
        KJ_SWITCH_ONEOF(fileOrDirectory) {
          KJ_CASE_ONEOF(file, kj::Rc<File>) {
            return file.addRef();
          }
          KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
            return dir.addRef();
          }
          KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
            return link.addRef();
          }
        }
        KJ_UNREACHABLE;
      })();
      entries.insert(kj::str(name), kj::mv(ret));
      return kj::none;
    } else {
      return FsError::NOT_PERMITTED;
    }
  }

  // Tries to remove the file or directory at the given path. If the node does
  // not exist, false will be returned. If the node is a directory and the recursive
  // option is not set, an exception will be thrown if the directory is not empty.
  // If the directory is read only, an exception will be thrown.
  // If the node is a file, it will be removed regardless of the recursive option
  // if the directory is not read only.
  // The path must be relative to the current directory.
  kj::OneOf<FsError, bool> remove(
      jsg::Lock& js, kj::PathPtr path, RemoveOptions opts = {}) override {
    if constexpr (Writable) {
      if (path.size() == 0) return false;
      KJ_IF_SOME(found, entries.find(path[0])) {
        KJ_SWITCH_ONEOF(found) {
          KJ_CASE_ONEOF(file, kj::Rc<File>) {
            if (path.size() != 1) return FsError::NOT_DIRECTORY;
            return entries.erase(path[0]);
          }
          KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
            if (path.size() == 1) {
              if (dir->count(js) > 0 && !opts.recursive) {
                return FsError::NOT_EMPTY;
              }
              return entries.erase(path[0]);
            }
            return dir->remove(js, path.slice(1, path.size()), kj::mv(opts));
          }
          KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
            // If we found a symbolic link, we can remove it if our path
            // is exactly the symbolic link. If the path is longer, then
            // we are trying to remove the target of the symbolic link
            // which we do not allow.
            if (path.size() != 1) return FsError::NOT_DIRECTORY;
            return entries.erase(path[0]);
          }
        }
      }

      return false;
    } else {
      return FsError::READ_ONLY;
    }
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "Directory"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(DirectoryBase);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    for (auto& entry: entries) {
      KJ_SWITCH_ONEOF(entry.value) {
        KJ_CASE_ONEOF(file, kj::Rc<File>) {
          tracker.trackField("file", *file.get());
          break;
        }
        KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
          tracker.trackField("directory", *dir.get());
          break;
        }
        KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
          // There's no need to track the symbolic link itself.
        }
      }
    }
  }

  kj::StringPtr getUniqueId(jsg::Lock&) const override {
    KJ_IF_SOME(id, maybeUniqueId) {
      return id;
    }
    // Generating a UUID requires randomness, which requires an IoContext.
    JSG_REQUIRE(IoContext::hasCurrent(), Error, "Cannot generate a unique ID outside of a request");
    auto& ioContext = IoContext::current();
    maybeUniqueId = workerd::randomUUID(ioContext.getEntropySource());
    return KJ_ASSERT_NONNULL(maybeUniqueId);
  }

  void countTowardsIsolateLimit(jsg::Lock& js) const override {
    // We only count writable directories towards the isolate limit.
    // Why? Because read-only directories are controlled by the runtime
    // and not users and we don't want to count them against the isolate.
    if constexpr (Writable) {
      if (maybeMemoryAdjustment == kj::none) {
        maybeMemoryAdjustment = js.getExternalMemoryAdjustment(sizeof(DirectoryBase));
      }
    }
  }

 private:
  kj::HashMap<kj::String, Item> entries;
  mutable kj::Maybe<kj::String> maybeUniqueId;
  mutable kj::Maybe<jsg::ExternalMemoryAdjustment> maybeMemoryAdjustment;

  // Called by tryOpen to create a new file or directory at the given path.
  kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>> tryCreate(
      jsg::Lock& js, kj::PathPtr path, FsType createAs) {
    KJ_DASSERT(Writable);
    KJ_DASSERT(path.size() > 0);

    // If the path size is one, then we are creating the file or directory
    // in *this* directory.
    if (path.size() == 1) {
      switch (createAs) {
        case FsType::FILE: {
          auto file = File::newWritable(js);
          auto ret = file.addRef();
          entries.insert(kj::str(path[0]), kj::mv(file));
          return kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(ret));
        }
        case FsType::DIRECTORY: {
          auto dir = Directory::newWritable(js);
          auto ret = dir.addRef();
          entries.insert(kj::str(path[0]), kj::mv(dir));
          return kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(ret));
        }
        case FsType::SYMLINK: {
          return kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>>(
              FsError::NOT_PERMITTED);
        }
      }
    }

    // Otherwise we need to recursively create a directory and ask it to create the file.
    auto dir = Directory::newWritable(js);
    KJ_IF_SOME(ret,
        dir->tryOpen(js, path.slice(1, path.size()),
            OpenOptions{
              .createAs = createAs,
            })) {
      // We will only create the new subdirectory in this directory if the
      // child target was successfully created/opened.
      entries.insert(kj::str(path[0]), kj::mv(dir));
      KJ_SWITCH_ONEOF(ret) {
        KJ_CASE_ONEOF(file, kj::Rc<File>) {
          return kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(file));
        }
        KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
          return kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(dir));
        }
        KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
          KJ_UNREACHABLE;
        }
        KJ_CASE_ONEOF(err, FsError) {
          return kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>>(err);
        }
      }
    }
    return kj::none;
  }
};

// The implementation of the File interface.
class FileImpl final: public File {
 public:
  // Constructor used to create a read-only file.
  FileImpl(kj::ArrayPtr<const kj::byte> data): ownedOrView(data), lastModified(kj::UNIX_EPOCH) {}
  FileImpl(kj::Array<kj::byte>&& owned) = delete;

  // Constructor used to create a writable file.
  FileImpl(jsg::Lock& js, kj::Array<kj::byte> owned)
      : ownedOrView(Owned(js, kj::mv(owned))),
        lastModified(kj::UNIX_EPOCH) {}

  kj::Maybe<FsError> setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    if (isWritable()) {
      lastModified = date;
    }
    return kj::none;
  }

  Stat stat(jsg::Lock& js) override {
    return Stat{
      .type = FsType::FILE,
      .size = static_cast<uint32_t>(readableView().size()),
      .lastModified = lastModified,
      .writable = isWritable(),
    };
  }

  uint32_t read(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<kj::byte> buffer) const override {
    auto data = readableView();
    if (offset >= data.size() || buffer.size() == 0) return 0;
    auto src = data.slice(offset);
    KJ_DASSERT(src.size() > 0);
    if (buffer.size() > src.size()) {
      buffer.first(src.size()).copyFrom(src);
      return src.size();
    }
    buffer.copyFrom(src.first(buffer.size()));
    return buffer.size();
  }

  // Writes data to the file at the given offset.
  kj::OneOf<FsError, uint32_t> write(
      jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> buffer) override {
    auto maxSize = Worker::Isolate::from(js).getLimitEnforcer().getBlobSizeLimit();
    if (buffer.size() > maxSize) {
      return FsError::FILE_SIZE_LIMIT_EXCEEDED;
    }

    size_t end = offset + buffer.size();
    if (end > maxSize || end < offset /* overflow check */) {
      return FsError::FILE_SIZE_LIMIT_EXCEEDED;
    }
    if (!isWritable()) {  
      return FsError::READ_ONLY;
    }

    if (end > writableView().size()) {
      KJ_IF_SOME(err, resize(js, end)) {
        return err;
      }
    }
    writableView().slice(offset, end).copyFrom(buffer);
    return static_cast<uint32_t>(buffer.size());
  }

  kj::Maybe<FsError> resize(jsg::Lock& js, uint32_t size) override {
    if (!isWritable()) {
      return FsError::READ_ONLY;
    }
    auto& owned = ownedOrView.get<Owned>();
    if (size == owned.data.size()) return kj::none;  // Nothing to do.

    auto maxSize = Worker::Isolate::from(js).getLimitEnforcer().getBlobSizeLimit();
    if (size > maxSize) {
      return FsError::FILE_SIZE_LIMIT_EXCEEDED;
    }

    auto newData = kj::heapArray<kj::byte>(size);

    if (size > owned.data.size()) {
      // To grow the file, we need to allocate a new array, copy the old data over,
      // and replace the original.
      newData.first(owned.data.size()).copyFrom(owned.data);
      newData.slice(owned.data.size()).fill(0);
    } else {
      newData.asPtr().copyFrom(owned.data.first(size));
    }
    owned.adjustment.setNow(js, newData.size());
    owned.data = kj::mv(newData);
    return kj::none;
  }

  kj::Maybe<FsError> fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    if (!isWritable()) {
      return FsError::READ_ONLY;
    }
    auto view = writableView();
    int actualOffset = offset.orDefault(0);
    if (actualOffset >= view.size() || view.size() == 0) return kj::none;
    view.slice(actualOffset).fill(value);
    return kj::none;
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "File"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(FileImpl);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    // We only track the memory if we own the data.
    KJ_SWITCH_ONEOF(ownedOrView) {
      KJ_CASE_ONEOF(owned, Owned) {
        tracker.trackField("owned", owned.data);
        return;
      }
      KJ_CASE_ONEOF(view, kj::ArrayPtr<const kj::byte>) {
        return;
      }
    }
  }

  kj::OneOf<FsError, kj::Rc<File>> clone(jsg::Lock& js) override {
    auto maxSize = Worker::Isolate::from(js).getLimitEnforcer().getBlobSizeLimit();
    KJ_SWITCH_ONEOF(ownedOrView) {
      KJ_CASE_ONEOF(owned, Owned) {
        if (owned.data.size() > maxSize) [[unlikely]] {
          return FsError::FILE_SIZE_LIMIT_EXCEEDED;
        }
        kj::Rc<File> file = kj::rc<FileImpl>(js, kj::heapArray<kj::byte>(owned.data));
        return kj::mv(file);
      }
      KJ_CASE_ONEOF(view, kj::ArrayPtr<const kj::byte>) {
        if (view.size() > maxSize) [[unlikely]] {
          return FsError::FILE_SIZE_LIMIT_EXCEEDED;
        }
        kj::Rc<File> file = kj::rc<FileImpl>(js, kj::heapArray<kj::byte>(view));
        return kj::mv(file);
      }
    }
    KJ_UNREACHABLE;
  }

  kj::Maybe<FsError> replace(jsg::Lock& js, kj::Rc<File> file) override {
    if (!isWritable()) {
      return FsError::READ_ONLY;
    }

    auto stat = file->stat(js);
    auto buffer = kj::heapArray<kj::byte>(stat.size);
    file->read(js, 0, buffer.asPtr());
    auto& owned = ownedOrView.get<Owned>();
    owned.adjustment.setNow(js, buffer.size());
    owned.data = kj::mv(buffer);
    lastModified = stat.lastModified;
    return kj::none;
  }

  kj::StringPtr getUniqueId(jsg::Lock&) const override {
    KJ_IF_SOME(id, maybeUniqueId) {
      return id;
    }
    // Generating a UUID requires randomness, which requires an IoContext.
    JSG_REQUIRE(IoContext::hasCurrent(), Error, "Cannot generate a unique ID outside of a request");
    auto& ioContext = IoContext::current();
    maybeUniqueId = workerd::randomUUID(ioContext.getEntropySource());
    return KJ_ASSERT_NONNULL(maybeUniqueId);
  }

  void countTowardsIsolateLimit(jsg::Lock& js) const override {
    // We only count writable files towards the isolate limit. Read-only files are
    // controlled by the runtime. We don't want to count them against the isolate.
    if (isWritable()) {
      if (maybeMemoryAdjustment == kj::none) {
        maybeMemoryAdjustment = js.getExternalMemoryAdjustment(sizeof(FileImpl));
      }
    }
  }

 private:
  struct Owned {
    kj::Array<kj::byte> data;
    jsg::ExternalMemoryAdjustment adjustment;
    Owned(jsg::Lock& js, kj::Array<kj::byte>&& data)
        : data(kj::mv(data)),
          adjustment(js.getExternalMemoryAdjustment(this->data.size())) {}
  };
  kj::OneOf<Owned, kj::ArrayPtr<const kj::byte>> ownedOrView;
  kj::Date lastModified;
  mutable kj::Maybe<kj::String> maybeUniqueId;
  mutable kj::Maybe<jsg::ExternalMemoryAdjustment> maybeMemoryAdjustment;

  bool isWritable() const {
    // Our file is only writable if it owns the actual data buffer.
    return ownedOrView.is<Owned>();
  }

  kj::ArrayPtr<kj::byte> writableView() {
    return KJ_REQUIRE_NONNULL(ownedOrView.tryGet<Owned>()).data.asPtr();
  }

  jsg::ExternalMemoryAdjustment& getAdjustment() {
    return KJ_REQUIRE_NONNULL(ownedOrView.tryGet<Owned>()).adjustment;
  }

  kj::ArrayPtr<const kj::byte> readableView() const {
    KJ_SWITCH_ONEOF(ownedOrView) {
      KJ_CASE_ONEOF(view, kj::ArrayPtr<const kj::byte>) {
        return view;
      }
      KJ_CASE_ONEOF(owned, Owned) {
        return owned.data.asPtr().asConst();
      }
    }
    KJ_UNREACHABLE;
  }
};

using ReadableDirectory = DirectoryBase<false>;
using WritableDirectory = DirectoryBase<true>;

class VirtualFileSystemImpl;

class FdHandle final {
 public:
  FdHandle(kj::Rc<WeakRef<VirtualFileSystemImpl>> weakFs, int fd): weakFs(kj::mv(weakFs)), fd(fd) {}

  ~FdHandle() noexcept(false);

 private:
  kj::Rc<WeakRef<VirtualFileSystemImpl>> weakFs;
  int fd;
};

class VirtualFileSystemImpl final: public VirtualFileSystem {
 public:
  VirtualFileSystemImpl(
      kj::Own<FsMap> fsMap, kj::Rc<Directory>&& root, kj::Own<VirtualFileSystem::Observer> observer)
      : fsMap(kj::mv(fsMap)),
        root(kj::mv(root)),
        observer(kj::mv(observer)),
        weakThis(
            kj::rc<WeakRef<VirtualFileSystemImpl>>(kj::Badge<VirtualFileSystemImpl>(), *this)) {}

  kj::Rc<OpenedFile> getStdio(jsg::Lock& js, Stdio stdio) const override;

  ~VirtualFileSystemImpl() noexcept(false) override {
    weakThis->invalidate();
  }

  kj::Rc<Directory> getRoot(jsg::Lock& js) const override {
    return root.addRef();
  }

  const jsg::Url& getBundleRoot() const override {
    return fsMap->getBundleRoot();
  }

  const jsg::Url& getTmpRoot() const override {
    return fsMap->getTempRoot();
  }

  const jsg::Url& getDevRoot() const override {
    return fsMap->getDevRoot();
  }

  kj::OneOf<FsError, kj::Rc<OpenedFile>> openFd(
      jsg::Lock& js, const jsg::Url& url, OpenOptions opts = {}) const override {

    kj::Path root{};
    auto str = kj::str(url.getPathname().slice(1));

    // We will impose an absolute max number of total file descriptors to
    // max int... in practice, the production system should condemn the
    // worker far before this limit is reached. Note that this is not *opened*
    // file descriptors, this is total file descriptors opened. There is no
    // way to reset this counter.
    static constexpr int kMax = kj::maxValue;
    if (nextFd == kMax) {
      observer->onMaxFds(openedFiles.size());
      return FsError::TOO_MANY_OPEN_FILES;
    }

    auto rootDir = getRoot(js);
    auto path = root.eval(str);

    if (opts.exclusive && opts.write) {
      // If the exclusive flag is set with the witable flag, then we fail
      // if the file already exists.
      KJ_IF_SOME(maybeStat, rootDir->stat(js, path)) {
        KJ_SWITCH_ONEOF(maybeStat) {
          KJ_CASE_ONEOF(stat, Stat) {
            return FsError::ALREADY_EXISTS;
          }
          KJ_CASE_ONEOF(err, FsError) {
            return err;
          }
        }
        KJ_UNREACHABLE;
      }
    }

    KJ_IF_SOME(node,
        rootDir->tryOpen(js, path,
            Directory::OpenOptions{
              .createAs = FsType::FILE,
              .followLinks = opts.followLinks,
            })) {
      KJ_SWITCH_ONEOF(node) {
        KJ_CASE_ONEOF(file, kj::Rc<File>) {
          if (opts.write) {
            auto stat = file->stat(js);
            if (!stat.writable) return FsError::NOT_PERMITTED;
          }
          KJ_DASSERT(openedFiles.find(nextFd) == kj::none);
          KJ_DEFER(observer->onOpen(openedFiles.size(), nextFd));
          auto fd = nextFd++;
          auto opened = kj::rc<OpenedFile>(fd, opts.read, opts.write, opts.append, kj::mv(file));
          openedFiles.insert(fd, opened.addRef());
          return kj::mv(opened);
        }
        KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
          if (opts.write) {
            // Similar to Node.js, we do not allow opening fd's for
            // directories for writing.
            return FsError::NOT_PERMITTED_ON_DIRECTORY;
          }
          KJ_DASSERT(openedFiles.find(nextFd) == kj::none);
          KJ_DEFER(observer->onOpen(openedFiles.size(), nextFd));
          auto fd = nextFd++;
          auto opened = kj::rc<OpenedFile>(fd, opts.read, opts.write, opts.append, kj::mv(dir));
          openedFiles.insert(fd, opened.addRef());
          return kj::mv(opened);
        }
        KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
          // Symlinks are never directly writable so the write flag
          // makes no sense.
          if (opts.write) {
            return FsError::NOT_PERMITTED;
          }
          KJ_DASSERT(openedFiles.find(nextFd) == kj::none);
          KJ_DEFER(observer->onOpen(openedFiles.size(), nextFd));
          auto fd = nextFd++;
          auto opened = kj::rc<OpenedFile>(fd, opts.read, opts.write, opts.append, kj::mv(link));
          openedFiles.insert(fd, opened.addRef());
          return kj::mv(opened);
        }
        KJ_CASE_ONEOF(err, FsError) {
          // If we got an error, we just return it.
          return err;
        }
      }
      KJ_UNREACHABLE;
    }

    // The file does not exist, and apparently was not created. Likely the
    // directory is not writable or does not exist.
    return FsError::FAILED;
  }

  void closeFd(jsg::Lock& js, int fd) const override {
    // We do not allow closing the stdio file descriptors.
    static constexpr int kMaxFd = static_cast<int>(Stdio::ERR);
    if (fd <= kMaxFd) return;
    closeFdWithoutExplicitLock(fd);
  }

  kj::Maybe<kj::Rc<OpenedFile>> tryGetFd(jsg::Lock& js, int fd) const override {
    static constexpr int kMaxFd = static_cast<int>(Stdio::ERR);
    if (fd <= kMaxFd) {
      return getStdio(js, static_cast<Stdio>(fd));
    }
    KJ_IF_SOME(opened, openedFiles.find(fd)) {
      return opened.addRef();
    }
    return kj::none;
  }

  kj::Own<void> wrapFd(jsg::Lock& js, int fd) const override {
    return kj::heap<FdHandle>(weakThis.addRef(), fd);
  }

  // While held, holds a lock on the given locator url. While locked,
  // certain operations on the identified node are not allowed.
  struct Lock {
    // Because we can't guarantee that the the lock will certainly be destroyed
    // before the VFS, we hold a weak reference to the VFS. If the VFS is destroyed
    // first, the lock essentially becomes a no-op.
    kj::Rc<WeakRef<VirtualFileSystemImpl>> vfs;
    const jsg::Url& url;

    void lock(const jsg::Url& locator) const {
      vfs->runIfAlive([&locator](auto& vfs) {
        KJ_IF_SOME(locked, vfs.locks.find(locator)) {
          locked++;
        } else {
          vfs.locks.insert(locator.clone(), 1);
        }
      });
    }

    void unlock(const jsg::Url& locator) const {
      vfs->runIfAlive([&locator](auto& vfs) {
        KJ_IF_SOME(locked, vfs.locks.find(locator)) {
          if (locked == 1) {
            vfs.locks.erase(locator);
          } else {
            KJ_ASSERT(locked > 0);
            --locked;
          }
        }
      });
    }

    Lock(kj::Rc<WeakRef<VirtualFileSystemImpl>> vfs, const jsg::Url& url)
        : vfs(kj::mv(vfs)),
          url(url) {
      lock(url);

      // This is fun... per the webfs spec, we need to prevent directories from
      // being removed if any locks are held on any files descendent from it,
      // so when we grab a lock, we also need to lock the parent path.
      auto maybeParent = url.getParent();
      while (maybeParent != kj::none) {
        auto& parent = KJ_ASSERT_NONNULL(maybeParent);
        lock(parent);
        maybeParent = parent.getParent();
      }
    }
    ~Lock() noexcept(false) {
      unlock(url);
      auto maybeParent = url.getParent();
      while (maybeParent != kj::none) {
        auto& parent = KJ_ASSERT_NONNULL(maybeParent);
        unlock(parent);
        maybeParent = parent.getParent();
      }
    }
  };

  kj::Own<void> lock(jsg::Lock& js, const jsg::Url& locator) const override {
    return kj::heap<Lock>(weakThis.addRef(), locator);
  }
  bool isLocked(jsg::Lock& js, const jsg::Url& locator) const override {
    KJ_IF_SOME(locked, locks.find(locator)) {
      return locked > 0;
    }
    return false;
  }

 private:
  // All operations on the VFS are expected to be performed while holding the
  // isolate lock even tho the VFS itself does not depend directly on the
  // lock. This is to ensure that the VFS operations are thread-safe without
  // incurring additional locking overhead.
  kj::Own<FsMap> fsMap;
  mutable kj::Rc<Directory> root;
  kj::Own<VirtualFileSystem::Observer> observer;
  mutable kj::Rc<WeakRef<VirtualFileSystemImpl>> weakThis;
  friend class FdHandle;

  // The next file descriptor to be used for the next file opened.
  mutable int nextFd = static_cast<int>(Stdio::ERR) + 1;

  mutable kj::HashMap<int, kj::Rc<OpenedFile>> openedFiles;
  mutable kj::HashMap<jsg::Url, size_t> locks;

  void closeFdWithoutExplicitLock(int fd) const {
    openedFiles.erase(fd);
    observer->onClose(openedFiles.size(), nextFd);
  }
};

FdHandle::~FdHandle() noexcept(false) {
  weakFs->runIfAlive([&](VirtualFileSystemImpl& vfs) { vfs.closeFdWithoutExplicitLock(fd); });
}

}  // namespace

kj::OneOf<FsError, jsg::JsString> File::readAllText(jsg::Lock& js) {
  auto info = stat(js);
  KJ_DASSERT(info.type == FsType::FILE);
  if (info.size == 0) return js.str();

  KJ_STACK_ARRAY(char, data, info.size, 4096, 4096);
  auto size = read(js, 0, data.asBytes());
  if (size != info.size) {
    return FsError::FAILED;
  }
  return js.str(data);
}

kj::OneOf<FsError, jsg::BufferSource> File::readAllBytes(jsg::Lock& js) {
  auto info = stat(js);
  KJ_DASSERT(info.type == FsType::FILE);
  auto backing = jsg::BackingStore::alloc<v8::Uint8Array>(js, info.size);
  if (info.size > 0) {
    KJ_ASSERT(read(js, 0, backing) == info.size);
  }
  return jsg::BufferSource(js, kj::mv(backing));
}

void Directory::Builder::add(
    kj::StringPtr name, kj::OneOf<kj::Rc<File>, kj::Rc<Directory>> fileOrDirectory) {
  KJ_REQUIRE(validatePathWithNoSeparators(name));
  KJ_REQUIRE(entries.find(name) == kj::none, "file or directory already exists: \"", name, "\"");
  entries.insert(kj::str(name), kj::mv(fileOrDirectory));
}

void Directory::Builder::add(kj::StringPtr name, kj::Own<Directory::Builder> builder) {
  KJ_REQUIRE(validatePathWithNoSeparators(name));
  KJ_REQUIRE(entries.find(name) == kj::none, "file or directory already exists: \"", name, "\"");
  entries.insert(kj::str(name), kj::mv(builder));
}

void Directory::Builder::addPath(
    kj::PathPtr path, kj::OneOf<kj::Rc<File>, kj::Rc<Directory>> fileOrDirectory) {
  KJ_ASSERT(path.size() > 0);

  if (path.size() == 1) {
    return add(path[0], kj::mv(fileOrDirectory));
  }

  // We have multiple path segments. We need to either find or create the
  // directory at the first segment and then add the rest of the path to
  // it.
  auto& entry = entries.findOrCreate(
      path[0], [&] { return Entry{kj::str(path[0]), kj::heap<Directory::Builder>()}; });

  KJ_SWITCH_ONEOF(entry) {
    KJ_CASE_ONEOF(file, kj::Rc<File>) {
      // The current entry is a file but we are trying to add a directory.
      // This is an error.
      KJ_FAIL_ASSERT("Path already exists and is a file: ", path[0]);
    }
    KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
      // The current entry is a directory that is already built.
      // This is an error.
      KJ_FAIL_ASSERT("Path already exists and is a directory: ", path[0]);
    }
    KJ_CASE_ONEOF(builder, kj::Own<Directory::Builder>) {
      // The current entry is a directory builder. We need to add the
      // rest of the path to it.
      builder->addPath(path.slice(1, path.size()), kj::mv(fileOrDirectory));
    }
  }
}

kj::Rc<Directory> Directory::Builder::finish() {
  auto map = kj::mv(entries);
  kj::HashMap<kj::String, Directory::Item> ret;

  auto addEntry = [&](kj::String name, kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>&& entry) {
    ret.upsert(
        kj::str(name), kj::mv(entry), [&](auto& current, auto&& node) { return kj::mv(node); });
  };

  for (auto& entry: map) {
    KJ_SWITCH_ONEOF(entry.value) {
      KJ_CASE_ONEOF(file, kj::Rc<File>) {
        addEntry(kj::mv(entry.key), kj::mv(file));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
        addEntry(kj::mv(entry.key), kj::mv(dir));
      }
      KJ_CASE_ONEOF(builder, kj::Own<Directory::Builder>) {
        addEntry(kj::mv(entry.key), builder->finish());
      }
    }
  }
  return kj::rc<ReadableDirectory>(kj::mv(ret));
}

kj::Rc<Directory> Directory::newWritable() {
  return kj::rc<WritableDirectory>();
}

kj::Rc<Directory> Directory::newEmptyReadonly() {
  Directory::Builder builder;
  return builder.finish();
}

kj::Rc<Directory> Directory::newWritable(jsg::Lock& js) {
  auto dir = kj::rc<WritableDirectory>();
  dir->countTowardsIsolateLimit(js);
  return kj::mv(dir);
}

kj::Rc<File> File::newWritable(jsg::Lock& js, kj::Maybe<uint32_t> size) {
  // We will cap the maximum size of the file.
  auto maxSize = Worker::Isolate::from(js).getLimitEnforcer().getBlobSizeLimit();
  auto actualSize = kj::min(size.orDefault(0), maxSize);
  auto data = kj::heapArray<kj::byte>(actualSize);
  if (actualSize > 0) data.asPtr().fill(0);
  auto file = kj::rc<FileImpl>(js, kj::mv(data));
  file->countTowardsIsolateLimit(js);
  return kj::mv(file);
}

kj::Rc<File> File::newReadable(kj::ArrayPtr<const kj::byte> data) {
  return kj::rc<FileImpl>(data);
}

kj::Own<VirtualFileSystem> newVirtualFileSystem(
    kj::Own<FsMap> fsMap, kj::Rc<Directory>&& root, kj::Own<VirtualFileSystem::Observer> observer) {
  return kj::heap<VirtualFileSystemImpl>(kj::mv(fsMap), kj::mv(root), kj::mv(observer));
}

kj::Own<VirtualFileSystem> newWorkerFileSystem(kj::Own<FsMap> fsMap,
    kj::Rc<Directory> bundleDirectory,
    kj::Own<VirtualFileSystem::Observer> observer) {
  // Our root directory is a read-only directory
  Directory::Builder builder;
  builder.addPath(fsMap->getBundlePath(), kj::mv(bundleDirectory));
  builder.addPath(fsMap->getTempPath(), getTmpDirectoryImpl());
  builder.addPath(fsMap->getDevPath(), getDevDirectory());
  return newVirtualFileSystem(kj::mv(fsMap), builder.finish(), kj::mv(observer));
}

kj::Rc<Directory> getTmpDirectoryImpl() {
  return kj::rc<TmpDirectory>();
}

bool TmpDirStoreScope::hasCurrent() {
  return tmpDirStorageScope != nullptr;
}

TmpDirStoreScope& TmpDirStoreScope::current() {
  KJ_ASSERT(hasCurrent(), "no current TmpDirStoreScope");
  return *tmpDirStorageScope;
}

TmpDirStoreScope::TmpDirStoreScope(kj::Maybe<kj::Badge<TmpDirStoreScope>> guard)
    : dir(Directory::newWritable()),
      // we use the /bundle cwd for the isolate vfs
      // and the /tmp cwd for the iocontext vfs
      cwd({"bundle"}) {
  if (guard == kj::none) {
    kj::requireOnStack(this, "must be created on the stack");
    onStack = true;
    KJ_ASSERT(!hasCurrent(), "TmpDirStoreScope already exists on this thread");
    tmpDirStorageScope = this;
  }
}

TmpDirStoreScope::~TmpDirStoreScope() noexcept(false) {
  if (onStack) {
    KJ_ASSERT(tmpDirStorageScope == this, "this TmpDirStoreScope not on the stack");
    tmpDirStorageScope = nullptr;
  }
}

kj::Own<TmpDirStoreScope> TmpDirStoreScope::create() {
  // Creating the instance with the badge will ensure that
  // it is not set as current in the stack.
  return kj::heap<TmpDirStoreScope>(kj::Badge<TmpDirStoreScope>());
}

Stat SymbolicLink::stat(jsg::Lock& js) {
  return Stat{
    .type = FsType::SYMLINK,
    .size = 0,
    .lastModified = kj::UNIX_EPOCH,
    .writable = false,
  };
}

jsg::Url SymbolicLink::getTargetUrl() const {
  auto path = getTargetPath().toString(false);
  return KJ_ASSERT_NONNULL(jsg::Url::tryParse(path, "file:///"_kj));
}

kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>> SymbolicLink::resolve(
    jsg::Lock& js) {
  KJ_IF_SOME(ret, root->tryOpen(js, getTargetPath())) {
    KJ_SWITCH_ONEOF(ret) {
      KJ_CASE_ONEOF(file, kj::Rc<File>) {
        return kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(file));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
        return kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(dir));
      }
      KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
        // The resolve(...) method here follows all symbolic links in the path,
        // so when it encounters a symlink as the path is being processed, it
        // will attempt to resolve it into it's target or return kj::none. If
        // you want the symlink itself, then use tryOpen(...) on the directory
        KJ_UNREACHABLE;
      }
      KJ_CASE_ONEOF(err, FsError) {
        return kj::Maybe<kj::OneOf<FsError, kj::Rc<File>, kj::Rc<Directory>>>(err);
      }
    }
    KJ_UNREACHABLE;
  }
  return kj::none;
}

kj::StringPtr SymbolicLink::getUniqueId(jsg::Lock&) const {
  KJ_IF_SOME(id, maybeUniqueId) {
    return id;
  }
  // Generating a UUID requires randomness, which requires an IoContext.
  JSG_REQUIRE(IoContext::hasCurrent(), Error, "Cannot generate a unique ID outside of a request");
  auto& ioContext = IoContext::current();
  maybeUniqueId = workerd::randomUUID(ioContext.getEntropySource());
  return KJ_ASSERT_NONNULL(maybeUniqueId);
}

void SymbolicLink::countTowardsIsolateLimit(jsg::Lock& js) const {
  if (maybeMemoryAdjustment == kj::none) {
    maybeMemoryAdjustment = js.getExternalMemoryAdjustment(sizeof(SymbolicLink));
  }
}

kj::Rc<Directory> getLazyDirectoryImpl(kj::Function<kj::Rc<Directory>()> func) {
  return kj::rc<LazyDirectory>(kj::mv(func));
}

const VirtualFileSystem& VirtualFileSystem::current(jsg::Lock& js) {
  // The VFS is stored in an embedder data slot in the v8::Context associated with
  // the current jsg::Lock. The actual instance is kept alive using a kj::Own held
  // by the Worker::Script.
  return KJ_ASSERT_NONNULL(jsg::getAlignedPointerFromEmbedderData<VirtualFileSystem>(
      js.v8Context(), jsg::ContextPointerSlot::VIRTUAL_FILE_SYSTEM));
}

kj::Maybe<FsNodeWithError> VirtualFileSystem::resolve(
    jsg::Lock& js, const jsg::Url& url, ResolveOptions options) const {
  if (url.getProtocol() != "file:"_kj) {
    // We only accept file URLs.
    return kj::none;
  }
  // We want to strip the leading slash from the path.
  auto path = kj::str(url.getPathname().slice(1));
  kj::Path root{};
  return getRoot(js)->tryOpen(js, root.eval(path),
      Directory::OpenOptions{
        .followLinks = options.followLinks,
      });
}

kj::Maybe<kj::OneOf<FsError, Stat>> VirtualFileSystem::resolveStat(
    jsg::Lock& js, const jsg::Url& url) const {
  if (url.getProtocol() != "file:"_kj) {
    // We only accept file URLs.
    return kj::none;
  }
  // We want to strip the leading slash from the path.
  auto path = kj::str(url.getPathname().slice(1));
  kj::Path root{};
  return getRoot(js)->stat(js, root.eval(path));
}

kj::Rc<SymbolicLink> VirtualFileSystem::newSymbolicLink(jsg::Lock& js, const jsg::Url& url) const {
  KJ_REQUIRE(url.getProtocol() == "file:"_kj);
  auto path = kj::str(url.getPathname().slice(1));
  kj::Path root{};
  return kj::rc<SymbolicLink>(getRoot(js), root.eval(path));
}

SymbolicLinkRecursionGuardScope::SymbolicLinkRecursionGuardScope() {
  if (symbolicLinkGuard == nullptr) {
    symbolicLinkGuard = this;
  }
}
SymbolicLinkRecursionGuardScope::~SymbolicLinkRecursionGuardScope() noexcept(false) {
  if (symbolicLinkGuard == this) {
    symbolicLinkGuard = nullptr;
  }
}

kj::Maybe<FsError> SymbolicLinkRecursionGuardScope::checkSeen(SymbolicLink* link) {
  if (symbolicLinkGuard == nullptr) {
    return kj::none;
  }
  auto& guard = *symbolicLinkGuard;
  if (guard.linksSeen.find(link) != kj::none) {
    return FsError::SYMLINK_DEPTH_EXCEEDED;
  }
  guard.linksSeen.insert(link);
  return kj::none;
}

namespace {
// Implementations of special "device" files equivalent to special devices typically
// found on posix systems.

// /dev/null is a special file that discards all data written to it and returns
// EOF on reads.
class DevNullFile final: public File {
 public:
  DevNullFile() = default;

  Stat stat(jsg::Lock& js) override {
    return Stat{
      .type = FsType::FILE,
      .size = 0,
      .lastModified = kj::UNIX_EPOCH,
      .writable = true,
      .device = true,
    };
  }

  kj::OneOf<FsError, kj::Rc<File>> clone(jsg::Lock&) override {
    kj::Rc<File> ref = addRefToThis();
    return kj::mv(ref);
  }

  kj::Maybe<FsError> replace(jsg::Lock& js, kj::Rc<File> file) override {
    return kj::none;
  }

  kj::Maybe<FsError> setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    return kj::none;
  }

  kj::Maybe<FsError> fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    return kj::none;
  }

  kj::Maybe<FsError> resize(jsg::Lock& js, uint32_t size) override {
    return kj::none;
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "/dev/null"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(DevNullFile);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    // No-op.
  }

  uint32_t read(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<kj::byte> buffer) const override {
    return 0;
  }

  kj::OneOf<FsError, uint32_t> write(
      jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> buffer) override {
    return static_cast<uint32_t>(buffer.size());
  }

  kj::StringPtr getUniqueId(jsg::Lock&) const override {
    KJ_IF_SOME(id, maybeUniqueId) {
      return id;
    }
    // Generating a UUID requires randomness, which requires an IoContext.
    JSG_REQUIRE(IoContext::hasCurrent(), Error, "Cannot generate a unique ID outside of a request");
    auto& ioContext = IoContext::current();
    maybeUniqueId = workerd::randomUUID(ioContext.getEntropySource());
    return KJ_ASSERT_NONNULL(maybeUniqueId);
  }

 private:
  mutable kj::Maybe<kj::String> maybeUniqueId;
};

// /dev/zero is a special file that returns zeroes when read from and
// ignores writes.
class DevZeroFile final: public File {
 public:
  DevZeroFile() = default;

  Stat stat(jsg::Lock& js) override {
    return Stat{
      .type = FsType::FILE,
      .size = 0,
      .lastModified = kj::UNIX_EPOCH,
      .writable = true,
      .device = true,
    };
  }

  kj::OneOf<FsError, kj::Rc<File>> clone(jsg::Lock&) override {
    kj::Rc<File> ref = addRefToThis();
    return kj::mv(ref);
  }

  kj::Maybe<FsError> replace(jsg::Lock& js, kj::Rc<File> file) override {
    return kj::none;
  }

  kj::Maybe<FsError> setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    return kj::none;
  }

  kj::Maybe<FsError> fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    return kj::none;
  }

  kj::Maybe<FsError> resize(jsg::Lock& js, uint32_t size) override {
    return kj::none;
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "/dev/zero"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(DevZeroFile);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    // No-op.
  }

  uint32_t read(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<kj::byte> buffer) const override {
    buffer.fill(0);
    return buffer.size();
  }

  kj::OneOf<FsError, uint32_t> write(
      jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> buffer) override {
    return static_cast<uint32_t>(buffer.size());
  }

  kj::StringPtr getUniqueId(jsg::Lock&) const override {
    KJ_IF_SOME(id, maybeUniqueId) {
      return id;
    }
    // Generating a UUID requires randomness, which requires an IoContext.
    JSG_REQUIRE(IoContext::hasCurrent(), Error, "Cannot generate a unique ID outside of a request");
    auto& ioContext = IoContext::current();
    maybeUniqueId = workerd::randomUUID(ioContext.getEntropySource());
    return KJ_ASSERT_NONNULL(maybeUniqueId);
  }

 private:
  mutable kj::Maybe<kj::String> maybeUniqueId;
};

// /dev/full is a special file that returns zeroes when read from and
// returns an error when written to.
class DevFullFile final: public File {
 public:
  DevFullFile() = default;

  Stat stat(jsg::Lock& js) override {
    return Stat{
      .type = FsType::FILE,
      .size = 0,
      .lastModified = kj::UNIX_EPOCH,
      .writable = true,
      .device = true,
    };
  }

  kj::OneOf<FsError, kj::Rc<File>> clone(jsg::Lock&) override {
    kj::Rc<File> ref = addRefToThis();
    return kj::mv(ref);
  }

  kj::Maybe<FsError> replace(jsg::Lock& js, kj::Rc<File> file) override {
    return FsError::NOT_PERMITTED;
  }

  kj::Maybe<FsError> setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    return kj::none;
  }

  kj::Maybe<FsError> fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    return FsError::NOT_PERMITTED;
  }

  kj::Maybe<FsError> resize(jsg::Lock& js, uint32_t size) override {
    return FsError::NOT_PERMITTED;
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "/dev/full"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(DevFullFile);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    // No-op.
  }

  uint32_t read(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<kj::byte> buffer) const override {
    buffer.fill(0);
    return buffer.size();
  }

  kj::OneOf<FsError, uint32_t> write(
      jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> buffer) override {
    return FsError::NOT_PERMITTED;
  }

  kj::StringPtr getUniqueId(jsg::Lock&) const override {
    KJ_IF_SOME(id, maybeUniqueId) {
      return id;
    }
    // Generating a UUID requires randomness, which requires an IoContext.
    JSG_REQUIRE(IoContext::hasCurrent(), Error, "Cannot generate a unique ID outside of a request");
    auto& ioContext = IoContext::current();
    maybeUniqueId = workerd::randomUUID(ioContext.getEntropySource());
    return KJ_ASSERT_NONNULL(maybeUniqueId);
  }

 private:
  mutable kj::Maybe<kj::String> maybeUniqueId;
};

class DevRandomFile final: public File {
 public:
  DevRandomFile() = default;

  Stat stat(jsg::Lock& js) override {
    return Stat{
      .type = FsType::FILE,
      .size = 0,
      .lastModified = kj::UNIX_EPOCH,
      .writable = true,
      .device = true,
    };
  }

  kj::OneOf<FsError, kj::Rc<File>> clone(jsg::Lock&) override {
    kj::Rc<File> ref = addRefToThis();
    return kj::mv(ref);
  }

  kj::Maybe<FsError> replace(jsg::Lock& js, kj::Rc<File> file) override {
    return FsError::NOT_PERMITTED;
  }

  kj::Maybe<FsError> setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    return FsError::NOT_PERMITTED;
  }

  kj::Maybe<FsError> fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    return FsError::NOT_PERMITTED;
  }

  kj::Maybe<FsError> resize(jsg::Lock& js, uint32_t size) override {
    return FsError::NOT_PERMITTED;
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "/dev/random"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(DevRandomFile);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {
    // No-op.
  }

  kj::OneOf<FsError, uint32_t> write(
      jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> buffer) override {
    return FsError::NOT_PERMITTED;
  }

  uint32_t read(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<kj::byte> buffer) const override {
    // We can only generate random bytes when we have an active IoContext.
    // If there is no IoContext, this will return 0 bytes.
    if (!IoContext::hasCurrent()) return 0;
    auto& ioContext = IoContext::current();
    if (isPredictableModeForTest()) {
      buffer.fill(9);
    } else {
      ioContext.getEntropySource().generate(buffer);
    }
    return buffer.size();
  }

  kj::StringPtr getUniqueId(jsg::Lock&) const override {
    KJ_IF_SOME(id, maybeUniqueId) {
      return id;
    }
    // Generating a UUID requires randomness, which requires an IoContext.
    JSG_REQUIRE(IoContext::hasCurrent(), Error, "Cannot generate a unique ID outside of a request");
    auto& ioContext = IoContext::current();
    maybeUniqueId = workerd::randomUUID(ioContext.getEntropySource());
    return KJ_ASSERT_NONNULL(maybeUniqueId);
  }

 private:
  mutable kj::Maybe<kj::String> maybeUniqueId;
};

// Write stdio via console.log. Somewhat convoluted, but this then supports:
// - inspector reporting
// - structured logging
// - stdio output otherwise
void writeStdio(jsg::Lock& js, VirtualFileSystem::Stdio type, kj::ArrayPtr<const kj::byte> bytes) {
  auto chars = bytes.asChars();
  size_t endPos = chars.size();
  if (endPos > 0 && chars[endPos - 1] == '\n') endPos--;

  KJ_IF_SOME(console, js.global().get(js, "console"_kj).tryCast<jsg::JsObject>()) {
    auto method = console.get(js, "log"_kj);
    if (method.isFunction()) {
      v8::Local<v8::Value> methodVal(method);
      auto methodFunc = jsg::JsFunction(methodVal.As<v8::Function>());

      kj::String outputStr;
      auto isolate = &Worker::Isolate::from(js);
      auto prefix = type == VirtualFileSystem::Stdio::OUT ? isolate->getStdoutPrefix()
                                                          : isolate->getStderrPrefix();
      if (endPos == 0) {
        methodFunc.call(js, console, js.str(prefix));
      } else if (prefix.size() > 0) {
        methodFunc.call(js, console, js.str(kj::str(prefix, " "_kj, chars.first(endPos))));
      } else {
        methodFunc.call(js, console, js.str(chars.first(endPos)));
      }
      return;
    }
  }
  KJ_LOG(WARNING, "No console.log implementation available for stdio logging");
}

// An StdioFile is a special file implementation used to represent stdin,
// stdout, and stderr outputs. Writes are always forwarded to the underlying
// logging mechanisms. Reads always return EOF (0-byte reads).
class StdioFile final: public File {
 public:
  StdioFile(VirtualFileSystem::Stdio type)
      : type(type),
        // TODO(sometime): Investigate if we can refactor out the weakref here?
        weakThis(kj::rc<WeakRef<StdioFile>>(kj::Badge<StdioFile>(), *this)) {}

  ~StdioFile() noexcept(false) override {
    weakThis->invalidate();
  }

  Stat stat(jsg::Lock& js) override {
    return Stat{
      .type = FsType::FILE,
      .size = 0,
      .lastModified = kj::UNIX_EPOCH,
      .writable = true,
      .device = false,
    };
  }

  kj::OneOf<FsError, kj::Rc<File>> clone(jsg::Lock&) override {
    kj::Rc<File> ref = addRefToThis();
    return kj::mv(ref);
  }

  kj::Maybe<FsError> replace(jsg::Lock& js, kj::Rc<File> file) override {
    return FsError::NOT_PERMITTED;
  }

  kj::Maybe<FsError> setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    return FsError::NOT_PERMITTED;
  }

  kj::Maybe<FsError> fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    return FsError::NOT_PERMITTED;
  }

  kj::Maybe<FsError> resize(jsg::Lock& js, uint32_t size) override {
    return FsError::NOT_PERMITTED;
  }

  kj::StringPtr jsgGetMemoryName() const override {
    return "stdio"_kj;
  }

  size_t jsgGetMemorySelfSize() const override {
    return sizeof(StdioFile);
  }

  void jsgGetMemoryInfo(jsg::MemoryTracker& tracker) const override {}

  kj::OneOf<FsError, uint32_t> write(
      jsg::Lock& js, uint32_t, kj::ArrayPtr<const kj::byte> buffer) override {
    if (buffer.size() > MAX_WRITE_SIZE) {
      buffer = buffer.first(MAX_WRITE_SIZE);
    }

    if (buffer.size() == 0) return uint32_t(0);

    // We ignore the offset here. All writes are assumed to be appends.
    if (type != VirtualFileSystem::Stdio::IN) {
      size_t pos = 0;

      // Newline-based buffering
      while (pos < buffer.size()) {
        size_t newlinePos = pos;
        while (newlinePos < buffer.size() && buffer[newlinePos] != '\n') {
          newlinePos++;
        }

        if (newlinePos < buffer.size()) {
          auto lineData = buffer.slice(pos, newlinePos + 1);

          if (lineBuffer.size() > 0) {
            // We have buffered data - append the line data to it
            lineBuffer.addAll(lineData);
            writeStdio(js, type, lineBuffer.asPtr());
            lineBuffer.clear();
          } else {
            // No buffered data - log this line directly
            writeStdio(js, type, lineData);
          }

          pos = newlinePos + 1;
        } else {
          // No newlines -> append to line buffer
          auto remaining = buffer.slice(pos);
          auto totalSize = lineBuffer.size() + remaining.size();

          if (totalSize <= MAX_LINE_BUFFER_SIZE) {
            lineBuffer.addAll(remaining);
          } else {
            // New data alone exceeds limit, replace entire line buffer
            if (remaining.size() >= MAX_LINE_BUFFER_SIZE) {
              lineBuffer.clear();
              lineBuffer.addAll(remaining.slice(remaining.size() - MAX_LINE_BUFFER_SIZE));
            } else {
              // Combined size exceeds limit, remove oldest data
              auto toRemove = totalSize - MAX_LINE_BUFFER_SIZE;
              kj::Vector<kj::byte> newBuffer(MAX_LINE_BUFFER_SIZE);
              newBuffer.addAll(lineBuffer.slice(toRemove, lineBuffer.size()));
              newBuffer.addAll(remaining);
              lineBuffer = kj::mv(newBuffer);
            }
          }

          // Schedule a microtask to flush the lineBuffer
          // this way synchronous writes join the line, but async writes will be on a new line
          scheduleFlushMicrotask(js);

          break;
        }
      }
    }
    return static_cast<uint32_t>(buffer.size());
  }

  uint32_t read(jsg::Lock&, uint32_t, kj::ArrayPtr<kj::byte>) const override {
    return 0;  // EOF
  }

  kj::StringPtr getUniqueId(jsg::Lock&) const override {
    KJ_IF_SOME(id, maybeUniqueId) {
      return id;
    }
    // Generating a UUID requires randomness, which requires an IoContext.
    JSG_REQUIRE(IoContext::hasCurrent(), Error, "Cannot generate a unique ID outside of a request");
    auto& ioContext = IoContext::current();
    maybeUniqueId = workerd::randomUUID(ioContext.getEntropySource());
    return KJ_ASSERT_NONNULL(maybeUniqueId);
  }

 private:
  VirtualFileSystem::Stdio type;
  mutable kj::Maybe<kj::String> maybeUniqueId;

  static constexpr size_t MAX_LINE_BUFFER_SIZE = 4096;
  static constexpr size_t MAX_WRITE_SIZE = 16 * 1024;
  mutable kj::Vector<kj::byte> lineBuffer;
  mutable bool microtaskScheduled = false;

  kj::Rc<WeakRef<StdioFile>> weakThis;

  void scheduleFlushMicrotask(jsg::Lock& js) {
    if (microtaskScheduled) return;
    microtaskScheduled = true;

    // Create ephemeral callback with weak reference for safety
    auto callback = js.wrapSimpleFunction(js.v8Context(),
        [weakThis = weakThis->addRef()](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) {
      weakThis->runIfAlive([&](StdioFile& self) {
        self.microtaskScheduled = false;

        if (self.lineBuffer.size() > 0) {
          if (IoContext::hasCurrent()) {
            writeStdio(js, self.type, self.lineBuffer.asPtr());
          }
          self.lineBuffer.clear();
        }
      });
    });
    js.v8Isolate->EnqueueMicrotask(callback);
  }
};

}  // namespace

kj::Rc<File> getDevNull() {
  return kj::rc<DevNullFile>();
}

kj::Rc<File> getDevZero() {
  return kj::rc<DevZeroFile>();
}

kj::Rc<File> getDevFull() {
  return kj::rc<DevFullFile>();
}

kj::Rc<File> getDevRandom() {
  return kj::rc<DevRandomFile>();
}

kj::Rc<Directory> getDevDirectory() {
  Directory::Builder builder;
  builder.add("null", getDevNull());
  builder.add("zero", getDevZero());
  builder.add("full", getDevFull());
  builder.add("random", getDevRandom());
  return builder.finish();
}

kj::Rc<VirtualFileSystem::OpenedFile> VirtualFileSystemImpl::getStdio(
    jsg::Lock& js, Stdio stdio) const {
  int n = static_cast<int>(stdio);
  KJ_IF_SOME(existing, openedFiles.find(n)) {
    return existing.addRef();
  }
  auto stdioFile = kj::rc<StdioFile>(stdio);
  kj::Rc<workerd::File> file = kj::mv(stdioFile);
  auto opened = kj::rc<VirtualFileSystem::OpenedFile>(n, true, true, true, kj::mv(file));
  openedFiles.insert(n, opened.addRef());
  return kj::mv(opened);
}

}  // namespace workerd
