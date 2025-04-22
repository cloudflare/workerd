#include "filesystem.h"

#include "blob.h"

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
}  // namespace

Stat::Stat(const workerd::Stat& stat)
    : type(getNameForFsType(stat.type)),
      size(stat.size),
      lastModified(stat.lastModified),
      created(stat.created),
      writable(stat.writable) {}

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
    jsg::Lock& js, kj::String path, jsg::Optional<kj::String> createAs) {
  auto url = JSG_REQUIRE_NONNULL(jsg::Url::tryParse(path, "file:///"_kj), Error, "Invalid path");
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

bool DirectoryHandle::remove(jsg::Lock& js, kj::String path, jsg::Optional<RemoveOptions> options) {
  auto url = JSG_REQUIRE_NONNULL(jsg::Url::tryParse(path, "file:///"_kj), Error, "Invalid path");
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
    jsg::Lock& js, kj::String targetPath) {
  KJ_IF_SOME(vfs, workerd::VirtualFileSystem::tryGetCurrent(js)) {
    auto url =
        JSG_REQUIRE_NONNULL(jsg::Url::tryParse(targetPath, "file:///"_kj), Error, "Invalid path");
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
    jsg::Lock& js, kj::String path) {
  KJ_IF_SOME(vfs, workerd::VirtualFileSystem::tryGetCurrent(js)) {
    auto url = JSG_REQUIRE_NONNULL(jsg::Url::tryParse(path, "file:///"_kj), Error, "Invalid path");
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

kj::Maybe<Stat> FileSystemModule::stat(jsg::Lock& js, kj::String path) {
  KJ_IF_SOME(vfs, workerd::VirtualFileSystem::tryGetCurrent(js)) {
    auto url = JSG_REQUIRE_NONNULL(jsg::Url::tryParse(path, "file:///"_kj), Error, "Invalid path");
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

}  // namespace workerd::api
