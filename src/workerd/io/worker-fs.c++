#include "worker-fs.h"

#include <workerd/io/io-context.h>

#include <algorithm>

namespace workerd {

#define DEFINE_DEFAULTS_FOR_ROOTS(name, path)                                                      \
  const jsg::Url FsMap::kDefault##name##Path = path##_url;
KNOWN_VFS_ROOTS(DEFINE_DEFAULTS_FOR_ROOTS)
#undef DEFINE_DEFAULTS_FOR_ROOTS

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
  kj::Maybe<Stat> stat(jsg::Lock& js, kj::PathPtr ptr) override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return dir->stat(js, ptr);
    }
    if (ptr.size() == 0) {
      return Stat{
        .type = FsType::DIRECTORY,
        .size = 0,
        .lastModified = kj::UNIX_EPOCH,
        .writable = true,
      };
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

  kj::Maybe<FsNode> tryOpen(jsg::Lock& js, kj::PathPtr path, OpenOptions options = {}) override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      return dir->tryOpen(js, path, kj::mv(options));
    }
    return kj::none;
  }

  void add(jsg::Lock& js, kj::StringPtr name, Item entry) override {
    KJ_IF_SOME(dir, tryGetDirectory()) {
      dir->add(js, name, kj::mv(entry));
      return;
    }
    JSG_FAIL_REQUIRE(Error, "Cannot add a file into a read-only directory");
  }

  bool remove(jsg::Lock& js, kj::PathPtr path, RemoveOptions options = {}) override {
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

 private:
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

  kj::Maybe<Stat> stat(jsg::Lock& js, kj::PathPtr ptr) override {
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

  kj::Maybe<FsNode> tryOpen(jsg::Lock& js, kj::PathPtr path, OpenOptions options = {}) override {
    return getDirectory()->tryOpen(js, path, kj::mv(options));
  }

  void add(jsg::Lock& js, kj::StringPtr name, Item item) override {
    getDirectory()->add(js, name, kj::mv(item));
  }

  bool remove(jsg::Lock& js, kj::PathPtr path, RemoveOptions options = {}) override {
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
void validatePathWithNoSeparators(kj::StringPtr path) {
  try {
    auto parsed = kj::Path::parse(path);
    JSG_REQUIRE(parsed.size() == 1, Error, "Invalid path: \"", path, "\"");
  } catch (kj::Exception& e) {
    JSG_FAIL_REQUIRE(Error, "Invalid path: \"", path, "\"");
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

  kj::Maybe<Stat> stat(jsg::Lock& js, kj::PathPtr ptr) override {
    // When the path ptr size is 0, then we're looking for the stat of this directory.
    if (ptr.size() == 0) {
      return Stat{
        .type = FsType::DIRECTORY,
        .size = 0,
        .lastModified = kj::UNIX_EPOCH,
        .writable = Writable,
      };
    }

    // Otherwise, we need to look up the entry...
    KJ_IF_SOME(found, entries.find(ptr[0])) {
      KJ_SWITCH_ONEOF(found) {
        KJ_CASE_ONEOF(file, kj::Rc<File>) {
          // We found a file. If the remaining path is empty, yay! Return the stat.
          if (ptr.size() == 1) {
            return file->stat(js);
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
          guardScope.checkSeen(link.get());
          KJ_IF_SOME(resolved, link->resolve(js)) {
            KJ_SWITCH_ONEOF(resolved) {
              KJ_CASE_ONEOF(file, kj::Rc<File>) {
                return kj::Maybe(file->stat(js));
              }
              KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
                return dir->stat(js, ptr.slice(1, ptr.size()));
              }
            }
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

  kj::Maybe<FsNode> tryOpen(jsg::Lock& js, kj::PathPtr path, OpenOptions opts = {}) override {
    if (path.size() == 0) {
      // An empty path ends up just returning this directory.
      return kj::Maybe<FsNode>(addRefToThis());
    }
    KJ_IF_SOME(found, entries.find(path[0])) {
      if (path.size() == 1) {
        // We found the entry, return it.
        KJ_SWITCH_ONEOF(found) {
          KJ_CASE_ONEOF(file, kj::Rc<File>) {
            return kj::Maybe<FsNode>(file.addRef());
          }
          KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
            return kj::Maybe<FsNode>(dir.addRef());
          }
          KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
            if (!opts.followLinks) {
              // If we're not following links, when we just return the link itself
              // here.
              return kj::Maybe<FsNode>(link.addRef());
            }
            // Resolve the symbolic link and return the target, guarding against
            // recursion while doing so.
            SymbolicLinkRecursionGuardScope guardScope;
            guardScope.checkSeen(link.get());
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
          guardScope.checkSeen(link.get());
          KJ_IF_SOME(resolved, link->resolve(js)) {
            KJ_SWITCH_ONEOF(resolved) {
              KJ_CASE_ONEOF(file, kj::Rc<File>) {
                return kj::none;
              }
              KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
                return dir->tryOpen(js, path, kj::mv(opts));
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
      // If the directory is not writable, we cannot create a new entry
      JSG_REQUIRE(opts.createAs == kj::none, Error,
          "Cannot create a new file or directory in a read-only directory");
    }

    return kj::none;
  }

  void add(jsg::Lock& js, kj::StringPtr name, Item fileOrDirectory) override {
    if constexpr (Writable) {
      validatePathWithNoSeparators(name);
      JSG_REQUIRE(entries.find(name) == kj::none, Error, "File or directory already exists: \"",
          name, "\"");
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
    } else {
      JSG_FAIL_REQUIRE(Error, "Cannot add a file into a read-only directory");
    }
  }

  // Tries to remove the file or directory at the given path. If the node does
  // not exist, true will be returned. If the node is a directory and the recursive
  // option is not set, an exception will be thrown if the directory is not empty.
  // If the directory is read only, an exception will be thrown.
  // If the node is a file, it will be removed regardless of the recursive option
  // if the directory is not read only.
  // The path must be relative to the current directory.
  bool remove(jsg::Lock& js, kj::PathPtr path, RemoveOptions opts = {}) override {
    if constexpr (Writable) {
      if (path.size() == 0) return false;
      KJ_IF_SOME(found, entries.find(path[0])) {
        KJ_SWITCH_ONEOF(found) {
          KJ_CASE_ONEOF(file, kj::Rc<File>) {
            JSG_REQUIRE(
                path.size() == 1, Error, "Path resolved to a file but has more than one component");
            return entries.erase(path[0]);
          }
          KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
            path = path.slice(1, path.size());
            if (path.size() == 0) {
              if (opts.recursive) {
                // We can remove it since we are in recursive mode.
                return entries.erase(path[0]);
              } else if (dir->count(js) == 0) {
                // The directory is empty. We can remove it.
                return entries.erase(path[0]);
              }
              // The directory is not empty. We cannot remove it.
              JSG_FAIL_REQUIRE(Error, "Directory is not empty");
            } else {
              return dir->remove(js, path, kj::mv(opts));
            }
          }
          KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
            // If we found a symbolic link, we can remove it if our path
            // is exactly the symbolic link. If the path is longer, then
            // we are trying to remove the target of the symbolic link
            // which we do not allow.
            JSG_REQUIRE(path.size() == 1, Error,
                "Path resolved to a symbolic link but has more than one component");
            return entries.erase(path[0]);
          }
        }
      }

      return true;
    } else {
      JSG_FAIL_REQUIRE(Error, "Cannot remove a file or directory from a read-only directory");
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

 private:
  kj::HashMap<kj::String, Item> entries;

  // Called by tryOpen to create a new file or directory at the given path.
  kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>> tryCreate(
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
          return kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(ret));
        }
        case FsType::DIRECTORY: {
          auto dir = Directory::newWritable();
          auto ret = dir.addRef();
          entries.insert(kj::str(path[0]), kj::mv(dir));
          return kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(ret));
        }
        case FsType::SYMLINK: {
          JSG_FAIL_REQUIRE(Error, "Cannot create a symlink with tryOpen");
        }
      }
    }

    // Otherwise we need to recursively create a directory and ask it to create the file.
    auto dir = Directory::newWritable();
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
          return kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(file));
        }
        KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
          return kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(dir));
        }
        KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
          KJ_UNREACHABLE;
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

  // Constructor used to create a writable file.
  FileImpl(kj::Array<kj::byte> owned): ownedOrView(kj::mv(owned)), lastModified(kj::UNIX_EPOCH) {}

  void setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    if (isWritable()) {
      lastModified = date;
    }
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
  uint32_t write(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> buffer) override {
    static constexpr uint32_t kMax = kj::maxValue;
    JSG_REQUIRE(buffer.size() <= kMax, Error, "File size exceeds maximum limit");
    auto& owned = writableView();
    size_t end = offset + buffer.size();
    if (end > owned.size()) resize(js, end);
    owned.slice(offset, end).copyFrom(buffer);
    return buffer.size();
  }

  void resize(jsg::Lock& js, uint32_t size) override {
    auto& owned = writableView();
    if (size == owned.size()) return;  // Nothing to do.

    auto newData = kj::heapArray<kj::byte>(size);

    if (size > owned.size()) {
      // To grow the file, we need to allocate a new array, copy the old data over,
      // and replace the original.
      newData.first(owned.size()).copyFrom(owned);
      newData.slice(owned.size()).fill(0);
    } else {
      newData.asPtr().copyFrom(owned.first(size));
    }
    ownedOrView = newData.attach(js.getExternalMemoryAdjustment(newData.size()));
  }

  void fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    auto& owned = JSG_REQUIRE_NONNULL(
        ownedOrView.tryGet<kj::Array<kj::byte>>(), Error, "Cannot modify a read-only file");
    owned.slice(offset.orDefault(0)).fill(value);
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
      KJ_CASE_ONEOF(owned, kj::Array<kj::byte>) {
        tracker.trackField("owned", owned);
        return;
      }
      KJ_CASE_ONEOF(view, kj::ArrayPtr<const kj::byte>) {
        return;
      }
    }
  }

  kj::Rc<File> clone(jsg::Lock&) override {
    KJ_SWITCH_ONEOF(ownedOrView) {
      KJ_CASE_ONEOF(owned, kj::Array<kj::byte>) {
        return kj::rc<FileImpl>(kj::heapArray<kj::byte>(owned));
      }
      KJ_CASE_ONEOF(view, kj::ArrayPtr<const kj::byte>) {
        return kj::rc<FileImpl>(kj::heapArray<kj::byte>(view));
      }
    }
    KJ_UNREACHABLE;
  }

  void replace(jsg::Lock& js, kj::Rc<File> file) override {
    JSG_REQUIRE_NONNULL(
        ownedOrView.tryGet<kj::Array<kj::byte>>(), Error, "Cannot replace a read-only file");

    auto stat = file->stat(js);
    auto buffer =
        kj::heapArray<kj::byte>(stat.size).attach(js.getExternalMemoryAdjustment(stat.size));
    file->read(js, 0, buffer.asPtr());
    ownedOrView = kj::mv(buffer);
    lastModified = stat.lastModified;
  }

 private:
  kj::OneOf<kj::Array<kj::byte>, kj::ArrayPtr<const kj::byte>> ownedOrView;
  kj::Date lastModified;

  bool isWritable() const {
    // Our file is only writable if it owns the actual data buffer.
    return ownedOrView.is<kj::Array<kj::byte>>();
  }

  kj::Array<kj::byte>& writableView() {
    return JSG_REQUIRE_NONNULL(
        ownedOrView.tryGet<kj::Array<kj::byte>>(), Error, "Cannot write to a read-only file");
  }

  kj::ArrayPtr<const kj::byte> readableView() const {
    KJ_SWITCH_ONEOF(ownedOrView) {
      KJ_CASE_ONEOF(view, kj::ArrayPtr<const kj::byte>) {
        return view;
      }
      KJ_CASE_ONEOF(owned, kj::Array<kj::byte>) {
        return owned.asPtr().asConst();
      }
    }
    KJ_UNREACHABLE;
  }
};

using ReadableDirectory = DirectoryBase<false>;
using WritableDirectory = DirectoryBase<true>;

class VirtualFileSystemImpl final: public VirtualFileSystem {
 public:
  VirtualFileSystemImpl(
      kj::Own<FsMap> fsMap, kj::Rc<Directory>&& root, kj::Own<VirtualFileSystem::Observer> observer)
      : fsMap(kj::mv(fsMap)),
        root(kj::mv(root)),
        observer(kj::mv(observer)) {}

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

  OpenedFile& openFd(jsg::Lock& js, const jsg::Url& url, OpenOptions opts = {}) const override {

    kj::Maybe<FsType> createAs = opts.exclusive ? kj::none : kj::Maybe(FsType::FILE);
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
      JSG_FAIL_REQUIRE(Error, "Too many open files");
    }

    // TODO(node-fs): Currently tryOpen will always attempt to follow the
    // symlinks, which means we cannot correctly handle followLinks = false
    // just yet.
    JSG_REQUIRE(opts.followLinks, Error, "Cannot open a file with followLinks yet");

    KJ_IF_SOME(node,
        getRoot(js)->tryOpen(js, root.eval(str),
            Directory::OpenOptions{
              .createAs = createAs,
              .followLinks = opts.followLinks,
            })) {
      // If the exclusive option is set and we got here, then we need to
      // throw an error because we cannot open a file that already exists.
      JSG_REQUIRE(!opts.exclusive, Error, "File already exists");

      // If we are opening a node for writing, we need to make sure that the
      // node is writable.
      if (opts.write) {
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<File>) {
            auto stat = file->stat(js);
            JSG_REQUIRE(stat.writable, Error, "File is not writable");
          }
          KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
            // Similar to Node.js, we do not allow opening fd's for
            // directories for writing.
            JSG_FAIL_REQUIRE(Error, "Directory is not writable");
          }
          KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
            // Symlinks are never directly writable so the write flag
            // makes no sense.
            JSG_FAIL_REQUIRE(Error, "Symbolic link is not writable");
          }
        }
      }

      KJ_DASSERT(openedFiles.find(nextFd) == kj::none);
      KJ_DEFER(observer->onOpen(openedFiles.size(), nextFd));
      return openedFiles.insert(OpenedFile{
        .fd = nextFd++,
        .read = opts.read,
        .write = opts.write,
        .append = opts.append,
        .node = kj::mv(node),
      });
    }

    // The file does not exist, and apparently was not created. Likely the
    // directory is not writable or does not exist.
    JSG_FAIL_REQUIRE(Error, "Cannot open file: ", url);
  }

  void closeFd(jsg::Lock& js, int fd) const override {
    openedFiles.eraseMatch(fd);
    observer->onClose(openedFiles.size(), nextFd);
  }

  kj::Maybe<OpenedFile&> tryGetFd(jsg::Lock& js, int fd) const override {
    return openedFiles.find(fd);
  }

 private:
  kj::Own<FsMap> fsMap;
  mutable kj::Rc<Directory> root;
  kj::Own<VirtualFileSystem::Observer> observer;

  // The next file descriptor to be used for the next file opened.
  mutable int nextFd = 0;

  struct Callbacks final {
    int keyForRow(OpenedFile& row) const {
      return row.fd;
    }
    bool matches(OpenedFile& row, int fd) const {
      return row.fd == fd;
    }
    int hashCode(int fd) const {
      return kj::hashCode(fd);
    }
  };

  mutable kj::Table<OpenedFile, kj::HashIndex<Callbacks>> openedFiles;
};
}  // namespace

jsg::JsString File::readAllText(jsg::Lock& js) {
  auto info = stat(js);
  KJ_DASSERT(info.type == FsType::FILE);
  if (info.size == 0) return js.str();

  KJ_STACK_ARRAY(char, data, info.size, 4096, 4096);
  auto size = read(js, 0, data.asBytes());
  JSG_REQUIRE(size == info.size, Error, "failed to read all data");
  return js.str(data);
}

jsg::BufferSource File::readAllBytes(jsg::Lock& js) {
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
  validatePathWithNoSeparators(name);
  KJ_REQUIRE(entries.find(name) == kj::none, "file or directory already exists: \"", name, "\"");
  entries.insert(kj::str(name), kj::mv(fileOrDirectory));
}

void Directory::Builder::add(kj::StringPtr name, kj::Own<Directory::Builder> builder) {
  validatePathWithNoSeparators(name);
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

kj::Rc<File> File::newWritable(jsg::Lock& js, kj::Maybe<uint32_t> size) {
  auto actualSize = size.orDefault(0);
  auto data = kj::heapArray<kj::byte>(actualSize);
  if (actualSize > 0) data.asPtr().fill(0);
  auto owned = data.attach(js.getExternalMemoryAdjustment(actualSize));
  return kj::rc<FileImpl>(kj::mv(owned));
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
  return *static_cast<TmpDirStoreScope*>(tmpDirStorageScope);
}

TmpDirStoreScope::TmpDirStoreScope(kj::Maybe<kj::Badge<TmpDirStoreScope>> guard)
    : dir(Directory::newWritable()) {
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

kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>> SymbolicLink::resolve(jsg::Lock& js) {
  KJ_IF_SOME(ret, root->tryOpen(js, getTargetPath())) {
    KJ_SWITCH_ONEOF(ret) {
      KJ_CASE_ONEOF(file, kj::Rc<File>) {
        return kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(file));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<Directory>) {
        return kj::Maybe<kj::OneOf<kj::Rc<File>, kj::Rc<Directory>>>(kj::mv(dir));
      }
      KJ_CASE_ONEOF(link, kj::Rc<SymbolicLink>) {
        // The resolve(...) method here follows all symbolic links in the path,
        // so when it encounters a symlink as the path is being processed, it
        // will attempt to resolve it into it's target or return kj::none. If
        // you want the symlink itself, then use tryOpen(...) on the directory
        KJ_UNREACHABLE;
      }
    }
    KJ_UNREACHABLE;
  }
  return kj::none;
}

kj::Rc<Directory> getLazyDirectoryImpl(kj::Function<kj::Rc<Directory>()> func) {
  return kj::rc<LazyDirectory>(kj::mv(func));
}

kj::Maybe<const VirtualFileSystem&> VirtualFileSystem::tryGetCurrent(jsg::Lock&) {
  // Note that the jsg::Lock& argument here is not actually used. We require
  // that a jsg::Lock reference is passed in as proof that current() is called
  // from within a valid isolate lock so that the Worker::Api::current()
  // call below will work as expected.
  return Worker::Api::current().getVirtualFileSystem();
}

kj::Maybe<FsNode> VirtualFileSystem::resolve(jsg::Lock& js, const jsg::Url& url) const {
  if (url.getProtocol() != "file:"_kj) {
    // We only accept file URLs.
    return kj::none;
  }
  // We want to strip the leading slash from the path.
  auto path = kj::str(url.getPathname().slice(1));
  kj::Path root{};
  return getRoot(js)->tryOpen(js, root.eval(path));
}

kj::Maybe<Stat> VirtualFileSystem::resolveStat(jsg::Lock& js, const jsg::Url& url) const {
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
  if (url.getProtocol() != "file:"_kj) {
    // We only accept file URLs.
    JSG_FAIL_REQUIRE(Error, "Invalid URL: ", url);
  }
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

void SymbolicLinkRecursionGuardScope::checkSeen(SymbolicLink* link) {
  if (symbolicLinkGuard == nullptr) {
    return;
  }
  auto& guard = *symbolicLinkGuard;
  JSG_REQUIRE(guard.linksSeen.find(link) == kj::none, Error, "Recursive symbolic link detected");
  guard.linksSeen.insert(link);
}

namespace {
// Implementations of special "device" files equivalent to special devices typically
// found on posix systems.

// /dev/null is a special file that discards all data written to it and returns
// EOF on reads.
class DevNullFile final: public File, public kj::EnableAddRefToThis<DevNullFile> {
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

  kj::Rc<File> clone(jsg::Lock&) override {
    return addRefToThis();
  }

  void replace(jsg::Lock& js, kj::Rc<File> file) override {
    // No-op.
  }

  void setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    // No-op.
  }

  void fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    // No-op.
  }

  void resize(jsg::Lock& js, uint32_t size) override {
    // No-op.
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

  uint32_t write(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> buffer) override {
    return buffer.size();
  }
};

// /dev/zero is a special file that returns zeroes when read from and
// ignores writes.
class DevZeroFile final: public File, public kj::EnableAddRefToThis<DevZeroFile> {
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

  kj::Rc<File> clone(jsg::Lock&) override {
    return addRefToThis();
  }

  void replace(jsg::Lock& js, kj::Rc<File> file) override {
    // No-op.
  }

  void setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    // No-op.
  }

  void fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    // No-op.
  }

  void resize(jsg::Lock& js, uint32_t size) override {
    // No-op.
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

  uint32_t write(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> buffer) override {
    return buffer.size();
  }
};

// /dev/full is a special file that returns zeroes when read from and
// returns an error when written to.
class DevFullFile final: public File, public kj::EnableAddRefToThis<DevFullFile> {
 public:
  DevFullFile() = default;

  Stat stat(jsg::Lock& js) override {
    return Stat{
      .type = FsType::FILE,
      .size = 0,
      .lastModified = kj::UNIX_EPOCH,
      .writable = false,
      .device = true,
    };
  }

  kj::Rc<File> clone(jsg::Lock&) override {
    return addRefToThis();
  }

  void replace(jsg::Lock& js, kj::Rc<File> file) override {
    JSG_FAIL_REQUIRE(Error, "Cannot replace /dev/full");
  }

  void setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    // No-op.
  }

  void fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    JSG_FAIL_REQUIRE(Error, "Cannot write to /dev/full");
  }

  void resize(jsg::Lock& js, uint32_t size) override {
    JSG_FAIL_REQUIRE(Error, "Cannot write to /dev/full");
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

  uint32_t write(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> buffer) override {
    JSG_FAIL_REQUIRE(Error, "Cannot write to /dev/full");
  }
};

class DevRandomFile final: public File, public kj::EnableAddRefToThis<DevRandomFile> {
 public:
  DevRandomFile() = default;

  Stat stat(jsg::Lock& js) override {
    return Stat{
      .type = FsType::FILE,
      .size = 0,
      .lastModified = kj::UNIX_EPOCH,
      .writable = false,
      .device = true,
    };
  }

  kj::Rc<File> clone(jsg::Lock&) override {
    return addRefToThis();
  }

  void replace(jsg::Lock& js, kj::Rc<File> file) override {
    JSG_FAIL_REQUIRE(Error, "Cannot replace /dev/random");
  }

  void setLastModified(jsg::Lock& js, kj::Date date = kj::UNIX_EPOCH) override {
    JSG_FAIL_REQUIRE(Error, "Cannot write to /dev/random");
  }

  void fill(jsg::Lock& js, kj::byte value, kj::Maybe<uint32_t> offset) override {
    JSG_FAIL_REQUIRE(Error, "Cannot write to /dev/random");
  }

  void resize(jsg::Lock& js, uint32_t size) override {
    JSG_FAIL_REQUIRE(Error, "Cannot write to /dev/random");
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

  uint32_t write(jsg::Lock& js, uint32_t offset, kj::ArrayPtr<const kj::byte> buffer) override {
    JSG_FAIL_REQUIRE(Error, "Cannot write to /dev/random");
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

}  // namespace workerd
