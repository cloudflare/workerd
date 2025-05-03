#include "filesystem.h"

#include "blob.h"

#include <workerd/api/streams/standard.h>
#include <workerd/api/url-standard.h>
#include <workerd/api/url.h>

namespace workerd::api {

namespace {
constexpr kj::StringPtr getNameForFsType(FsType type) {
  switch (type) {
    case FsType::FILE:
      return "file"_kj;
    case FsType::DIRECTORY:
      return "directory"_kj;
    case FsType::SYMLINK:
      return "symlink"_kj;
    default:
      KJ_UNREACHABLE;
  }
}

constexpr FsType getTypeForName(kj::StringPtr name) {
  if (name == "file"_kj) return FsType::FILE;
  if (name == "directory"_kj) return FsType::DIRECTORY;
  if (name == "symlink"_kj) return FsType::SYMLINK;
  KJ_UNREACHABLE;
}

jsg::Url filePathToUrl(FilePath& path) {
  KJ_SWITCH_ONEOF(path) {
    KJ_CASE_ONEOF(str, kj::String) {
      // For a string, we need to try parsing it as a URL first.
      auto url = JSG_REQUIRE_NONNULL(jsg::Url::tryParse(str, "file:///"_kj), Error, "Invalid path");
      JSG_REQUIRE(url.getProtocol() == "file:"_kjb, Error, "Only file: URLs are supported");
      return kj::mv(url);
    }
    KJ_CASE_ONEOF(legacyUrl, jsg::Ref<URL>) {
      // For a legacy URL, we need to convert it to a standard URL by
      // serializing it then parsing it again. This is unfortunate, but
      // necessary if we don't want to tie node:fs support to whether the
      // standard URL is enabled or not.
      JSG_REQUIRE(legacyUrl->getProtocol() == "file:", Error, "Only file: URLs are supported");
      auto str = legacyUrl->getHref();
      return JSG_REQUIRE_NONNULL(jsg::Url::tryParse(str.asPtr(), kj::none), Error, "Invalid path");
    }
    KJ_CASE_ONEOF(standardUrl, jsg::Ref<url::URL>) {
      return standardUrl->getInner().clone();
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace

Stat::Stat(const workerd::Stat& stat)
    : type(getNameForFsType(stat.type)),
      size(static_cast<double>(stat.size)),
      lastModifiedNs((stat.lastModified - kj::UNIX_EPOCH) / kj::NANOSECONDS),
      createdNs((stat.created - kj::UNIX_EPOCH) / kj::NANOSECONDS),
      writable(stat.writable),
      device(stat.device) {}

FileHandle::FileHandle(kj::Rc<workerd::File> inner): inner(kj::mv(inner)) {}

jsg::Ref<FileHandle> FileHandle::constructor(jsg::Lock& js, jsg::Optional<size_t> size) {
  return js.alloc<FileHandle>(workerd::File::newWritable(js, size));
}

Stat FileHandle::getStat(jsg::Lock& js) {
  return Stat(inner->stat(js));
}

void FileHandle::setLastModified(jsg::Lock& js, kj::Date date) {
  inner->setLastModified(js, date);
}

jsg::JsString FileHandle::readAllText(jsg::Lock& js) {
  return inner->readAllText(js);
}

jsg::BufferSource FileHandle::readAllBytes(jsg::Lock& js) {
  return inner->readAllBytes(js);
}

jsg::Ref<Blob> FileHandle::readAllAsBlob(jsg::Lock& js) {
  return js.alloc<Blob>(js, inner->readAllBytes(js), kj::String());
}

size_t FileHandle::read(jsg::Lock& js, size_t offset, jsg::BufferSource buffer) {
  return inner->read(js, offset, buffer);
}

size_t FileHandle::writeAll(
    jsg::Lock& js, kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String> data) {
  KJ_SWITCH_ONEOF(data) {
    KJ_CASE_ONEOF(buffer, jsg::BufferSource) {
      return inner->writeAll(js, buffer);
    }
    KJ_CASE_ONEOF(str, kj::String) {
      return inner->writeAll(js, str);
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return inner->writeAll(js, blob->getData());
    }
  }
  KJ_UNREACHABLE;
}

size_t FileHandle::write(
    jsg::Lock& js, size_t offset, kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String> data) {
  KJ_SWITCH_ONEOF(data) {
    KJ_CASE_ONEOF(buffer, jsg::BufferSource) {
      return inner->write(js, offset, buffer);
    }
    KJ_CASE_ONEOF(str, kj::String) {
      return inner->write(js, offset, str);
    }
    KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
      return inner->write(js, offset, blob->getData());
    }
  }
  KJ_UNREACHABLE;
}

void FileHandle::fill(jsg::Lock& js, size_t offset, kj::byte val) {
  inner->fill(js, offset, val);
}

void FileHandle::resize(jsg::Lock& js, size_t size) {
  inner->resize(js, size);
}

SymbolicLinkHandle::SymbolicLinkHandle(kj::Rc<workerd::SymbolicLink> inner): inner(kj::mv(inner)) {}

Stat SymbolicLinkHandle::getStat(jsg::Lock& js) {
  return Stat(inner->stat(js));
}

kj::String SymbolicLinkHandle::getTargetPath(jsg::Lock& js) {
  return inner->getTargetPath().toString(true);
}

kj::Maybe<kj::OneOf<jsg::Ref<FileHandle>, jsg::Ref<DirectoryHandle>>> SymbolicLinkHandle::resolve(
    jsg::Lock& js) {
  KJ_IF_SOME(node, inner->resolve(js)) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        return kj::Maybe(js.alloc<FileHandle>(kj::mv(file)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        return kj::Maybe(js.alloc<DirectoryHandle>(kj::mv(dir)));
      }
    }
    KJ_UNREACHABLE;
  }
  return kj::none;
}

DirectoryHandle::DirectoryHandle(kj::Rc<workerd::Directory> inner): inner(kj::mv(inner)) {}

jsg::Ref<DirectoryHandle> DirectoryHandle::constructor(jsg::Lock& js) {
  return js.alloc<DirectoryHandle>(workerd::Directory::newWritable());
}

Stat DirectoryHandle::getStat(jsg::Lock& js) {
  return Stat(inner->stat(js));
}

size_t DirectoryHandle::getCount(jsg::Lock& js, jsg::Optional<kj::String> typeFilter) {
  KJ_IF_SOME(type, typeFilter) {
    return inner->count(js, getTypeForName(type));
  } else {
    return inner->count(js);
  }
}

kj::Maybe<kj::OneOf<jsg::Ref<FileHandle>, jsg::Ref<DirectoryHandle>>> DirectoryHandle::open(
    jsg::Lock& js, FilePath path, jsg::Optional<kj::String> createAs) {
  auto url = filePathToUrl(path);
  auto str = kj::str(url.getPathname().slice(1));
  kj::Path root{};
  KJ_IF_SOME(node, inner->tryOpen(js, root.eval(str), createAs.map([](kj::StringPtr name) {
    return getTypeForName(name);
  }))) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        return kj::Maybe(js.alloc<FileHandle>(kj::mv(file)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        return kj::Maybe(js.alloc<DirectoryHandle>(kj::mv(dir)));
      }
    }
    KJ_UNREACHABLE;
  }
  return kj::none;
}

bool DirectoryHandle::remove(jsg::Lock& js, FilePath path, jsg::Optional<RemoveOptions> options) {
  auto url = filePathToUrl(path);
  auto str = kj::str(url.getPathname().slice(1));
  kj::Path root{};
  auto opts = options.orDefault(RemoveOptions());
  return inner->remove(js, root.eval(str),
      workerd::Directory::RemoveOptions{
        .recursive = opts.recursive,
      });
}

void DirectoryHandle::add(jsg::Lock& js,
    kj::String name,
    kj::OneOf<jsg::Ref<FileHandle>, jsg::Ref<DirectoryHandle>, jsg::Ref<SymbolicLinkHandle>>
        entry) {
  KJ_SWITCH_ONEOF(entry) {
    KJ_CASE_ONEOF(file, jsg::Ref<FileHandle>) {
      inner->add(js, name, file->getInner());
      return;
    }
    KJ_CASE_ONEOF(dir, jsg::Ref<DirectoryHandle>) {
      inner->add(js, name, dir->getInner());
      return;
    }
    KJ_CASE_ONEOF(link, jsg::Ref<SymbolicLinkHandle>) {
      inner->add(js, name, link->getInner());
      return;
    }
  }
  KJ_UNREACHABLE;
}

jsg::Ref<DirectoryHandle::EntryIterator> DirectoryHandle::entries(jsg::Lock& js) {
  kj::Vector<Entry> entries;
  for (auto& entry: *inner.get()) {
    KJ_SWITCH_ONEOF(entry.value) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        entries.add(Entry{
          .name = kj::str(entry.key),
          .value = js.alloc<FileHandle>(file.addRef()),
        });
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        entries.add(Entry{
          .name = kj::str(entry.key),
          .value = js.alloc<DirectoryHandle>(dir.addRef()),
        });
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        entries.add(Entry{
          .name = kj::str(entry.key),
          .value = js.alloc<SymbolicLinkHandle>(link.addRef()),
        });
      }
    }
  }

  return js.alloc<EntryIterator>(IteratorState<Entry>{
    .parent = JSG_THIS,
    .entries = entries.releaseAsArray(),
  });
}

jsg::Ref<DirectoryHandle::KeyIterator> DirectoryHandle::names(jsg::Lock& js) {
  kj::Vector<kj::String> entries;
  for (auto& entry: *inner.get()) {
    entries.add(kj::str(entry.key));
  }
  return js.alloc<KeyIterator>(IteratorState<kj::String>{
    .parent = JSG_THIS,
    .entries = entries.releaseAsArray(),
  });
}

void DirectoryHandle::forEach(jsg::Lock& js,
    jsg::Function<void(EntryType, kj::StringPtr, jsg::Ref<DirectoryHandle>)> callback,
    jsg::Optional<jsg::Value> thisArg) {
  auto receiver = js.v8Undefined();
  KJ_IF_SOME(arg, thisArg) {
    auto handle = arg.getHandle(js);
    if (!handle->IsNullOrUndefined()) {
      receiver = handle;
    }
  }
  callback.setReceiver(js.v8Ref(receiver));

  for (auto& entry: *inner.get()) {
    KJ_SWITCH_ONEOF(entry.value) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        callback(js, js.alloc<FileHandle>(file.addRef()), entry.key, JSG_THIS);
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        callback(js, js.alloc<DirectoryHandle>(dir.addRef()), entry.key, JSG_THIS);
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        callback(js, js.alloc<SymbolicLinkHandle>(link.addRef()), entry.key, JSG_THIS);
      }
    }
  }
}

kj::Maybe<kj::String> DirectoryHandle::nameNext(jsg::Lock& js, IteratorState<kj::String>& state) {
  if (state.index >= state.entries.size()) {
    return kj::none;
  }
  return kj::str(state.entries[state.index++]);
}

kj::Maybe<DirectoryHandle::Entry> DirectoryHandle::entryNext(
    jsg::Lock& js, IteratorState<Entry>& state) {
  if (state.index >= state.entries.size()) {
    return kj::none;
  }

  auto& entry = state.entries[state.index++];

  KJ_SWITCH_ONEOF(entry.value) {
    KJ_CASE_ONEOF(file, jsg::Ref<FileHandle>) {
      return Entry{
        .name = kj::str(entry.name),
        .value = file.addRef(),
      };
    }
    KJ_CASE_ONEOF(dir, jsg::Ref<DirectoryHandle>) {
      return Entry{
        .name = kj::str(entry.name),
        .value = dir.addRef(),
      };
    }
    KJ_CASE_ONEOF(link, jsg::Ref<SymbolicLinkHandle>) {
      return Entry{
        .name = kj::str(entry.name),
        .value = link.addRef(),
      };
    }
  }
  KJ_UNREACHABLE;
}

kj::Maybe<jsg::Ref<SymbolicLinkHandle>> FileSystemModule::symlink(
    jsg::Lock& js, FilePath targetPath) {
  KJ_IF_SOME(vfs, workerd::VirtualFileSystem::tryGetCurrent(js)) {
    auto url = filePathToUrl(targetPath);
    return js.alloc<SymbolicLinkHandle>(vfs.newSymbolicLink(js, url));
  }
  return kj::none;
}

kj::Maybe<jsg::Ref<DirectoryHandle>> FileSystemModule::getRoot(jsg::Lock& js) {
  KJ_IF_SOME(vfs, workerd::VirtualFileSystem::tryGetCurrent(js)) {
    return js.alloc<DirectoryHandle>(vfs.getRoot(js));
  }
  return kj::none;
}

kj::Maybe<kj::OneOf<jsg::Ref<FileHandle>, jsg::Ref<DirectoryHandle>>> FileSystemModule::open(
    jsg::Lock& js, FilePath path) {
  KJ_IF_SOME(vfs, workerd::VirtualFileSystem::tryGetCurrent(js)) {
    auto url = filePathToUrl(path);
    KJ_IF_SOME(node, vfs.resolve(js, url)) {
      KJ_SWITCH_ONEOF(node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          return kj::Maybe(js.alloc<FileHandle>(kj::mv(file)));
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          return kj::Maybe(js.alloc<DirectoryHandle>(kj::mv(dir)));
        }
      }
      KJ_UNREACHABLE;
    }
  }
  return kj::none;
}

kj::Maybe<Stat> FileSystemModule::stat(jsg::Lock& js, FilePath path) {
  KJ_IF_SOME(vfs, workerd::VirtualFileSystem::tryGetCurrent(js)) {
    auto url = filePathToUrl(path);
    return vfs.resolveStat(js, url).map([](const workerd::Stat& stat) { return Stat(stat); });
  }
  return kj::none;
}

kj::Maybe<jsg::Ref<DirectoryHandle>> FileSystemModule::getTmp(jsg::Lock& js) {
  KJ_IF_SOME(vfs, workerd::VirtualFileSystem::tryGetCurrent(js)) {
    KJ_IF_SOME(node, vfs.resolve(js, vfs.getTmpRoot())) {
      KJ_SWITCH_ONEOF(node) {
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          return js.alloc<DirectoryHandle>(kj::mv(dir));
        }
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          return kj::none;
        }
      }
    }
  }
  return kj::none;
}

// Attempt to copy a file from `from` to `to`. If `from` is a directory, an
// error is thrown. If `from` does not exist, an error is thrown. If `to`
// identifies a directory (or ends with a `/` indicating a directory) an error
// is thrown. If the exclusive option is set and the to file already exists,
// an error is thrown. If the exclusive option is not set and the file already
// exists, it is overwritten. If the copy fails, the destination file will not
// be created. If the `to` indicates a path with multiple components, all
// components except the last one must exist and must be directories or an
// error is thrown. If the call to copyFile completes without exception then
// the copy is presumed to have succeeded.
void FileSystemModule::copyFile(
    jsg::Lock& js, FilePath from, FilePath to, jsg::Optional<CopyFileOptions> options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system");
  auto fromUrl = filePathToUrl(from);
  auto toUrl = filePathToUrl(to);
  auto exclusive = options.orDefault(CopyFileOptions()).exclusive.orDefault(false);

  auto source = JSG_REQUIRE_NONNULL(vfs.resolve(js, fromUrl), Error, "Source does not exist");
  auto& sourceFile =
      JSG_REQUIRE_NONNULL(source.tryGet<kj::Rc<workerd::File>>(), Error, "Source is not a file");

  static auto performCopy = [](jsg::Lock& js, kj::Rc<workerd::File>& sourceFile,
                                kj::Rc<workerd::File>& destFile) {
    kj::byte buffer[4096];
    kj::ArrayPtr<kj::byte> bufferPtr(buffer);
    auto stat = sourceFile->stat(js);
    destFile->resize(js, stat.size);
    size_t remaining = stat.size;
    size_t offset = 0;
    while (remaining > 0) {
      size_t read = sourceFile->read(js, offset, bufferPtr);
      KJ_ASSERT(read <= remaining);
      KJ_ASSERT(read == destFile->write(js, offset, bufferPtr.slice(0, read)));
      remaining -= read;
      offset += read;
    }
  };

  JSG_REQUIRE(!toUrl.getPathname().endsWith("/"_kj), Error, "Destination cannot be a directory");
  KJ_IF_SOME(dest, vfs.resolve(js, toUrl)) {
    JSG_REQUIRE(!exclusive, Error, "Destination file exists");
    // The destination exists. Let's make sure it's a file.
    auto& destFile = JSG_REQUIRE_NONNULL(
        dest.tryGet<kj::Rc<workerd::File>>(), Error, "Destination is not a file");
    // The overwrite the contents.
    performCopy(js, sourceFile, destFile);
  } else {
    // The destination does not exist. If the path has multiple components,
    // then all components except the last one must exist and must be
    // directories.
    auto str = kj::str(toUrl.getPathname().slice(1));
    kj::Path root{};
    auto toPath = root.eval(str);
    auto parentPath = toPath.parent();
    auto name = toPath.basename();
    auto parent = JSG_REQUIRE_NONNULL(vfs.getRoot(js)->tryOpen(js, parentPath), Error,
        "Destination parent directory does not exist");
    // Make sure the parent is a directory.
    auto& parentDir = JSG_REQUIRE_NONNULL(parent.tryGet<kj::Rc<workerd::Directory>>(), Error,
        "Destination parent is not a directory");
    // Now, try to create the destination file.
    auto dest = JSG_REQUIRE_NONNULL(parentDir->tryOpen(js, name, workerd::FsType::FILE), Error,
        "Failed to create destination file");
    // Make sure the destination was created as a file
    auto& destFile = JSG_REQUIRE_NONNULL(
        dest.tryGet<kj::Rc<workerd::File>>(), Error, "Destination is not a file");
    performCopy(js, sourceFile, destFile);
  }
}

// =======================================================================================

namespace {
bool isValidFileName(kj::StringPtr name) {
  return name.size() > 0 && name != "."_kj && name != ".."_kj && name.find("/"_kj) == kj::none;
}
}  // namespace

jsg::Promise<jsg::Ref<FileSystemDirectoryHandle>> StorageManager::getDirectory(
    jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception) {
  KJ_IF_SOME(vfs, workerd::VirtualFileSystem::tryGetCurrent(js)) {
    return js.resolvedPromise(
        js.alloc<FileSystemDirectoryHandle>(jsg::USVString(), vfs.getRoot(js)));
  }

  auto ex = js.domException(kj::str("SecurityError"), kj::str("No current virtual file system"));
  return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(exception.wrap(js, kj::mv(ex)));
}

FileSystemDirectoryHandle::FileSystemDirectoryHandle(
    jsg::USVString name, kj::Rc<workerd::Directory> inner)
    : FileSystemHandle(kj::mv(name)),
      inner(kj::mv(inner)) {}

jsg::Promise<bool> FileSystemDirectoryHandle::isSameEntry(
    jsg::Lock& js, jsg::Ref<FileSystemHandle> other) {
  KJ_IF_SOME(other, kj::dynamicDowncastIfAvailable<FileSystemDirectoryHandle>(*other)) {
    return js.resolvedPromise(other.inner.get() == inner.get());
  }
  return js.resolvedPromise(false);
}

jsg::Promise<jsg::Ref<FileSystemFileHandle>> FileSystemDirectoryHandle::getFileHandle(jsg::Lock& js,
    jsg::USVString name,
    jsg::Optional<FileSystemGetFileOptions> options,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception) {
  JSG_REQUIRE(isValidFileName(name), TypeError, "Invalid file name");
  kj::Maybe<FsType> createAs;
  KJ_IF_SOME(opts, options) {
    if (opts.create) createAs = FsType::FILE;
  }
  KJ_IF_SOME(node, inner->tryOpen(js, kj::Path({name}), createAs)) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        return js.resolvedPromise(js.alloc<FileSystemFileHandle>(kj::mv(name), kj::mv(file)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        auto ex =
            js.domException(kj::str("TypeMismatchError"), kj::str("File name is a directory"));
        return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(exception.wrap(js, kj::mv(ex)));
      }
    }
    KJ_UNREACHABLE;
  }

  auto ex = js.domException(kj::str("NotFoundError"), kj::str("File not found"));
  return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(exception.wrap(js, kj::mv(ex)));
}

jsg::Promise<jsg::Ref<FileSystemDirectoryHandle>> FileSystemDirectoryHandle::getDirectoryHandle(
    jsg::Lock& js,
    jsg::USVString name,
    jsg::Optional<FileSystemGetDirectoryOptions> options,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception) {
  JSG_REQUIRE(isValidFileName(name), TypeError, "Invalid file name");
  kj::Maybe<FsType> createAs;
  KJ_IF_SOME(opts, options) {
    if (opts.create) createAs = FsType::FILE;
  }
  KJ_IF_SOME(node, inner->tryOpen(js, kj::Path({name}), createAs)) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        return js.resolvedPromise(js.alloc<FileSystemDirectoryHandle>(kj::mv(name), kj::mv(dir)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::File>) {
        auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("File name is a file"));
        return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(
            exception.wrap(js, kj::mv(ex)));
      }
    }
    KJ_UNREACHABLE;
  }

  auto ex = js.domException(kj::str("NotFoundError"), kj::str("File not found"));
  return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(exception.wrap(js, kj::mv(ex)));
}

jsg::Promise<void> FileSystemDirectoryHandle::removeEntry(jsg::Lock& js,
    jsg::USVString name,
    jsg::Optional<FileSystemRemoveOptions> options,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception) {
  JSG_REQUIRE(isValidFileName(name), TypeError, "Invalid file name");
  auto opts = options.orDefault(FileSystemRemoveOptions{});

  if (inner->remove(js, kj::Path({name}),
          workerd::Directory::RemoveOptions{
            .recursive = opts.recursive,
          })) {
    return js.resolvedPromise();
  }

  auto ex = js.domException(kj::str("NotFoundError"), kj::str("File not found"));
  return js.rejectedPromise<void>(exception.wrap(js, kj::mv(ex)));
}

jsg::Promise<kj::Array<jsg::USVString>> FileSystemDirectoryHandle::resolve(
    jsg::Lock& js, jsg::Ref<FileSystemHandle> possibleDescendant) {
  JSG_FAIL_REQUIRE(Error, "Not implemented");
}

namespace {
kj::Array<jsg::Ref<FileSystemHandle>> collectEntries(
    jsg::Lock& js, kj::Rc<workerd::Directory>& inner) {
  kj::Vector<jsg::Ref<FileSystemHandle>> entries;
  for (auto& entry: *inner.get()) {
    KJ_SWITCH_ONEOF(entry.value) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        entries.add(
            js.alloc<FileSystemFileHandle>(js.accountedUSVString(entry.key), file.addRef()));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        entries.add(
            js.alloc<FileSystemDirectoryHandle>(js.accountedUSVString(entry.key), dir.addRef()));
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        SymbolicLinkRecursionGuardScope guardScope;
        guardScope.checkSeen(link.get());
        KJ_IF_SOME(res, link->resolve(js)) {
          KJ_SWITCH_ONEOF(res) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              entries.add(
                  js.alloc<FileSystemFileHandle>(js.accountedUSVString(entry.key), file.addRef()));
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              entries.add(js.alloc<FileSystemDirectoryHandle>(
                  js.accountedUSVString(entry.key), dir.addRef()));
            }
          }
        }
      }
    }
  }
  return entries.releaseAsArray();
}
}  // namespace

jsg::Ref<FileSystemDirectoryHandle::EntryIterator> FileSystemDirectoryHandle::entries(
    jsg::Lock& js) {
  return js.alloc<EntryIterator>(IteratorState(JSG_THIS, collectEntries(js, inner)));
}

jsg::Ref<FileSystemDirectoryHandle::KeyIterator> FileSystemDirectoryHandle::keys(jsg::Lock& js) {
  return js.alloc<KeyIterator>(IteratorState(JSG_THIS, collectEntries(js, inner)));
}

jsg::Ref<FileSystemDirectoryHandle::ValueIterator> FileSystemDirectoryHandle::values(
    jsg::Lock& js) {
  return js.alloc<ValueIterator>(IteratorState(JSG_THIS, collectEntries(js, inner)));
}

void FileSystemDirectoryHandle::forEach(jsg::Lock& js,
    jsg::Function<void(
        jsg::USVString, jsg::Ref<FileSystemHandle>, jsg::Ref<FileSystemDirectoryHandle>)> callback,
    jsg::Optional<jsg::Value> thisArg) {
  auto receiver = js.v8Undefined();
  KJ_IF_SOME(arg, thisArg) {
    auto handle = arg.getHandle(js);
    if (!handle->IsNullOrUndefined()) {
      receiver = handle;
    }
  }
  callback.setReceiver(js.v8Ref(receiver));

  for (auto& entry: collectEntries(js, inner)) {
    callback(js, js.accountedUSVString(entry->getName(js)), entry.addRef(), JSG_THIS);
  }
}

FileSystemFileHandle::FileSystemFileHandle(jsg::USVString name, kj::Rc<workerd::File> inner)
    : FileSystemHandle(kj::mv(name)),
      inner(kj::mv(inner)) {}

jsg::Promise<bool> FileSystemFileHandle::isSameEntry(
    jsg::Lock& js, jsg::Ref<FileSystemHandle> other) {
  KJ_IF_SOME(other, kj::dynamicDowncastIfAvailable<FileSystemFileHandle>(*other)) {
    return js.resolvedPromise(other.inner.get() == inner.get());
  }
  return js.resolvedPromise(false);
}

jsg::Promise<jsg::Ref<File>> FileSystemFileHandle::getFile(jsg::Lock& js) {
  // TODO(node-fs): Currently this copies the file data into the new File object.
  // Alternatively, File/Blob can be modified to allow it to be backed by a
  // workerd::File such that it does not need to create a separate in-memory
  // copy of the data. We can make that optimization as a follow-up, however.
  auto stat = inner->stat(js);
  return js.resolvedPromise(
      js.alloc<File>(js, inner->readAllBytes(js), js.accountedUSVString(getName(js)), kj::String(),
          (stat.lastModified - kj::UNIX_EPOCH) / kj::MILLISECONDS));
}

jsg::Promise<jsg::Ref<FileSystemWritableFileStream>> FileSystemFileHandle::createWritable(
    jsg::Lock& js, jsg::Optional<FileSystemCreateWritableOptions> options) {
  // TODO(node-fs): Per the spec, the stream is expected to write to a temporary
  // file space that is only flushed to the underlying file when the stream is closed.
  // This is also supposed to be paying attention to the `keepExistingData` option,
  // which determines if the modified data retains the original file data or not.
  // We are not yet implementing this and it needs to be done before we can ship this.
  auto sharedState = kj::rc<FileSystemWritableFileStream::State>(inner.addRef());
  auto stream =
      js.alloc<FileSystemWritableFileStream>(newWritableStreamJsController(), sharedState.addRef());
  stream->getController().setup(js,
      UnderlyingSink{.type = kj::str("bytes"),
        .write =
            [state = sharedState.addRef()](
                jsg::Lock& js, v8::Local<v8::Value> chunk, auto c) mutable {
    // TODO(node-fs): The spec allows the write parameter to be a WriteParams struct
    // as an alternative to a BufferSource. We should implement this before shipping
    // this feature.
    return js.tryCatch([&] {
      auto& inner = JSG_REQUIRE_NONNULL(state->file, DOMInvalidStateError, "File handle closed");
      // Make sure what we got can be interpreted as bytes...
      std::shared_ptr<v8::BackingStore> backing;
      if (chunk->IsArrayBuffer() || chunk->IsArrayBufferView()) {
        jsg::BufferSource source(js, chunk);
        if (source.size() == 0) return js.resolvedPromise();
        state->position += inner->write(js, state->position, source);
        return js.resolvedPromise();
      }
      return js.rejectedPromise<void>(
          js.typeError("WritableStream received a value that is not an ArrayBuffer,"
                       "ArrayBufferView, or string type."));
    }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
  },
        .abort =
            [state = sharedState.addRef()](jsg::Lock& js, auto reason) mutable {
    state->file = kj::none;
    state->position = 0;
    return js.resolvedPromise();
  },
        .close =
            [state = sharedState.addRef()](jsg::Lock& js) mutable {
    state->file = kj::none;
    state->position = 0;
    return js.resolvedPromise();
  }},
      kj::none);

  return js.resolvedPromise(kj::mv(stream));
}

jsg::Promise<jsg::Ref<FileSystemSyncAccessHandle>> FileSystemFileHandle::createSyncAccessHandle(
    jsg::Lock& js) {
  // TODO(node-fs): Per the spec, creating a sync access handle or creating a stream
  // should be mutually exclusive and should lock the file such that no other sync
  // handles or streams can be created until the handle/stream is closed. We are not
  // yet implementing locks on the file and should consider doing so before we ship this.
  return js.resolvedPromise(js.alloc<FileSystemSyncAccessHandle>(inner.addRef()));
}

FileSystemSyncAccessHandle::FileSystemSyncAccessHandle(kj::Rc<workerd::File> inner)
    : inner(kj::mv(inner)) {}

double FileSystemSyncAccessHandle::read(
    jsg::Lock& js, jsg::BufferSource buffer, jsg::Optional<FileSystemReadWriteOptions> options) {
  auto& inner = JSG_REQUIRE_NONNULL(this->inner, DOMInvalidStateError, "File handle closed");
  auto offset = static_cast<size_t>(options.orDefault({}).at.orDefault(position));
  auto stat = inner->stat(js);
  if (offset > stat.size && !stat.device) {
    position = stat.size;
    return 0;
  }
  auto ret = inner->read(js, offset, buffer);

  position += ret;
  return static_cast<double>(ret);
}

double FileSystemSyncAccessHandle::write(
    jsg::Lock& js, jsg::BufferSource buffer, jsg::Optional<FileSystemReadWriteOptions> options) {
  auto& inner = JSG_REQUIRE_NONNULL(this->inner, DOMInvalidStateError, "File handle closed");
  auto offset = static_cast<size_t>(options.orDefault({}).at.orDefault(position));
  auto stat = inner->stat(js);
  if (offset > stat.size) {
    inner->resize(js, offset + buffer.size());
  }
  auto ret = inner->write(js, offset, buffer);
  position = offset + ret;
  return static_cast<double>(ret);
}

void FileSystemSyncAccessHandle::truncate(jsg::Lock& js, double newSize) {
  JSG_REQUIRE(newSize >= 0, TypeError, "Invalid size");
  auto& inner = JSG_REQUIRE_NONNULL(this->inner, DOMInvalidStateError, "File handle closed");
  inner->resize(js, static_cast<size_t>(newSize));
  auto stat = inner->stat(js);
  if (position > stat.size) {
    position = stat.size;
  }
}

double FileSystemSyncAccessHandle::getSize(jsg::Lock& js) {
  auto& inner = JSG_REQUIRE_NONNULL(this->inner, DOMInvalidStateError, "File handle closed");
  return static_cast<double>(inner->stat(js).size);
}

void FileSystemSyncAccessHandle::flush(jsg::Lock& js) {
  JSG_REQUIRE_NONNULL(this->inner, DOMInvalidStateError, "File handle closed");
  // Non-op
}

void FileSystemSyncAccessHandle::close(jsg::Lock& js) {
  inner = kj::none;
}

FileSystemWritableFileStream::FileSystemWritableFileStream(
    kj::Own<WritableStreamController> controller, kj::Rc<State> sharedState)
    : WritableStream(kj::mv(controller)),
      sharedState(kj::mv(sharedState)) {}

jsg::Promise<void> FileSystemWritableFileStream::write(
    jsg::Lock& js, kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String, WriteParams> data) {
  JSG_REQUIRE(!getController().isLockedToWriter(), TypeError,
      "Cannot write to a stream that is locked to a reader");
  auto& inner = JSG_REQUIRE_NONNULL(sharedState->file, DOMInvalidStateError, "File handle closed");
  auto writer = getWriter(js);
  KJ_DEFER(writer->releaseLock(js));

  return js.tryCatch([&] {
    KJ_SWITCH_ONEOF(data) {
      KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
        sharedState->position += inner->write(js, sharedState->position, blob->getData());
      }
      KJ_CASE_ONEOF(buffer, jsg::BufferSource) {
        sharedState->position += inner->write(js, sharedState->position, buffer);
      }
      KJ_CASE_ONEOF(str, kj::String) {
        sharedState->position += inner->write(js, sharedState->position, str);
      }
      KJ_CASE_ONEOF(params, WriteParams) {
        size_t offset = sharedState->position;
        KJ_IF_SOME(pos, params.position) {
          JSG_REQUIRE(pos >= 0, TypeError, "Invalid position");
          offset = static_cast<size_t>(pos);
          auto stat = inner->stat(js);
          if (offset > stat.size) {
            inner->resize(js, offset);
          }
        }

        if (params.type == "write"_kj) {
          KJ_IF_SOME(data, params.data) {
            KJ_SWITCH_ONEOF(data) {
              KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
                auto ret = inner->write(js, offset, blob->getData());
                sharedState->position = offset + ret;
              }
              KJ_CASE_ONEOF(buffer, jsg::BufferSource) {
                auto ret = inner->write(js, offset, buffer);
                sharedState->position = offset + ret;
              }
              KJ_CASE_ONEOF(str, kj::String) {
                auto ret = inner->write(js, offset, str);
                sharedState->position = offset + ret;
              }
            }
          }
        } else if (params.type == "seek"_kj) {
          double paramPos = params.position.orDefault(0);
          JSG_REQUIRE(paramPos >= 0, TypeError, "Invalid position");
          auto pos = static_cast<size_t>(paramPos);
          sharedState->position += pos;
          auto stat = inner->stat(js);
          if (sharedState->position > stat.size) {
            inner->resize(js, sharedState->position);
          }
        } else if (params.type == "truncate"_kj) {
          double paramSize = params.size.orDefault(0);
          JSG_REQUIRE(paramSize >= 0, TypeError, "Invalid size");
          auto size = static_cast<size_t>(paramSize);
          inner->resize(js, size);
          auto stat = inner->stat(js);
          if (sharedState->position > stat.size) {
            sharedState->position = stat.size;
          }
        } else {
          return js.rejectedPromise<void>(
              js.typeError(kj::str("Invalid write type: ", params.type)));
        }
      }
    }

    return js.resolvedPromise();
  }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
}

jsg::Promise<void> FileSystemWritableFileStream::seek(jsg::Lock& js, double position) {
  JSG_REQUIRE(position >= 0, TypeError, "Invalid position");
  auto& inner = JSG_REQUIRE_NONNULL(sharedState->file, DOMInvalidStateError, "File handle closed");
  auto stat = inner->stat(js);
  if (position > stat.size) {
    inner->resize(js, position);
  }
  sharedState->position = static_cast<size_t>(position);
  return js.resolvedPromise();
}

jsg::Promise<void> FileSystemWritableFileStream::truncate(jsg::Lock& js, double size) {
  JSG_REQUIRE(size >= 0, TypeError, "Invalid size");
  auto& inner = JSG_REQUIRE_NONNULL(sharedState->file, DOMInvalidStateError, "File handle closed");
  inner->resize(js, static_cast<size_t>(size));
  auto stat = inner->stat(js);
  if (sharedState->position > stat.size) {
    sharedState->position = stat.size;
  }
  return js.resolvedPromise();
}

}  // namespace workerd::api
