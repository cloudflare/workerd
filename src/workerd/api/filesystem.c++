#include "filesystem.h"

#include "blob.h"
#include "url-standard.h"
#include "url.h"

#include <workerd/api/streams/standard.h>

namespace workerd::api {

// =======================================================================================
// Implementation of cloudflare-internal:filesystem in support of node:fs

namespace {
static constexpr uint32_t kMax = kj::maxValue;
constexpr kj::StringPtr nameForFsType(FsType type) {
  switch (type) {
    case FsType::FILE:
      return "file"_kj;
    case FsType::DIRECTORY:
      return "directory"_kj;
    case FsType::SYMLINK:
      return "symlink"_kj;
  }
  KJ_UNREACHABLE;
}

// A file path is passed to the C++ layer as a URL object. However, we have two different
// implementations of URL in the system. This class wraps and abstracts over both of them.
struct NormalizedFilePath {
  kj::OneOf<jsg::Ref<url::URL>, jsg::Url> url;

  static kj::OneOf<jsg::Ref<url::URL>, jsg::Url> normalize(FileSystemModule::FilePath path) {
    KJ_SWITCH_ONEOF(path) {
      KJ_CASE_ONEOF(legacy, jsg::Ref<URL>) {
        return JSG_REQUIRE_NONNULL(
            jsg::Url::tryParse(legacy->getHref(), "file:///"_kj), Error, "Invalid URL"_kj);
      }
      KJ_CASE_ONEOF(standard, jsg::Ref<url::URL>) {
        return kj::mv(standard);
      }
    }
    KJ_UNREACHABLE;
  }

  NormalizedFilePath(FileSystemModule::FilePath path): url(normalize(kj::mv(path))) {
    validate();
  }

  void validate() {
    KJ_SWITCH_ONEOF(url) {
      KJ_CASE_ONEOF(standard, jsg::Ref<url::URL>) {
        JSG_REQUIRE(standard->getProtocol() == "file:"_kj, Error, "File path must be a file: URL");
        JSG_REQUIRE(standard->getHost().size() == 0, Error, "File path must not have a host");
      }
      KJ_CASE_ONEOF(legacy, jsg::Url) {
        JSG_REQUIRE(legacy.getProtocol() == "file:"_kj, TypeError, "File path must be a file: URL");
        JSG_REQUIRE(legacy.getHost().size() == 0, Error, "File path must not have a host");
      }
    }
  }

  operator const jsg::Url&() const {
    KJ_SWITCH_ONEOF(url) {
      KJ_CASE_ONEOF(legacy, jsg::Url) {
        return legacy;
      }
      KJ_CASE_ONEOF(standard, jsg::Ref<url::URL>) {
        return *standard;
      }
    }
    KJ_UNREACHABLE;
  }

  operator const kj::Path() const {
    const jsg::Url& url = *this;
    auto path = kj::str(url.getPathname().slice(1));
    kj::Path root{};
    return root.eval(path);
  }
};
}  // namespace

Stat::Stat(const workerd::Stat& stat)
    : type(nameForFsType(stat.type)),
      size(stat.size),
      lastModified((stat.lastModified - kj::UNIX_EPOCH) / kj::NANOSECONDS),
      created((stat.created - kj::UNIX_EPOCH) / kj::NANOSECONDS),
      writable(stat.writable),
      device(stat.device) {}

kj::Maybe<Stat> FileSystemModule::stat(
    jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd, StatOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalizedPath(kj::mv(path));
      KJ_IF_SOME(node,
          vfs.resolve(
              js, normalizedPath, {.followLinks = options.followSymlinks.orDefault(true)})) {
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            return Stat(file->stat(js));
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            return Stat(dir->stat(js));
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            // If a symbolic link is returned here then the options.followSymLinks
            // must have been set to false.
            return Stat(link->stat(js));
          }
        }
        KJ_UNREACHABLE;
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
      KJ_SWITCH_ONEOF(opened.node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          return Stat(file->stat(js));
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          return Stat(dir->stat(js));
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          return Stat(link->stat(js));
        }
      }
      KJ_UNREACHABLE;
    }
  }
  return kj::none;
}

void FileSystemModule::setLastModified(
    jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd, kj::Date lastModified, StatOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalizedPath(kj::mv(path));
      KJ_IF_SOME(node,
          vfs.resolve(
              js, normalizedPath, {.followLinks = options.followSymlinks.orDefault(true)})) {
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            file->setLastModified(js, lastModified);
            return;
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            // Do nothing
            return;
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            // If we got here, then followSymLinks was set to false. We cannot
            // change the last modified time of a symbolic link in our vfs so
            // we do nothing.
            return;
          }
        }
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
      KJ_SWITCH_ONEOF(opened.node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          file->setLastModified(js, lastModified);
          return;
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          // Do nothing
          return;
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          // Do nothing
          return;
        }
      }
    }
  }
  KJ_UNREACHABLE;
}

void FileSystemModule::truncate(jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd, uint32_t size) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalizedPath(kj::mv(path));
      KJ_IF_SOME(node, vfs.resolve(js, normalizedPath)) {
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            file->resize(js, size);
            return;
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            JSG_FAIL_REQUIRE(Error, "Invalid operation on a directory");
            return;
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            // If we got here, then followSymLinks was set to false. We cannot
            // truncate a symbolic link.
            JSG_FAIL_REQUIRE(Error, "Invalid operation on a symlink");
          }
        }
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
      KJ_SWITCH_ONEOF(opened.node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          file->resize(js, size);
          return;
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          JSG_FAIL_REQUIRE(Error, "Invalid operation on a directory");
          return;
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          JSG_FAIL_REQUIRE(Error, "Invalid operation on a symlink");
        }
      }
    }
  }
  KJ_UNREACHABLE;
}

kj::String FileSystemModule::readLink(jsg::Lock& js, FilePath path, ReadLinkOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  NormalizedFilePath normalizedPath(kj::mv(path));
  auto node = JSG_REQUIRE_NONNULL(
      vfs.resolve(js, normalizedPath, {.followLinks = false}), Error, "file not found");
  KJ_SWITCH_ONEOF(node) {
    KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
      JSG_REQUIRE(!options.failIfNotSymlink, Error, "invalid argument");
      kj::Path path = normalizedPath;
      return path.toString(true);
    }
    KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
      JSG_REQUIRE(!options.failIfNotSymlink, Error, "invalid argument");
      kj::Path path = normalizedPath;
      return path.toString(true);
    }
    KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
      return link->getTargetPath().toString(true);
    }
  }
  KJ_UNREACHABLE;
}

void FileSystemModule::link(jsg::Lock& js, FilePath from, FilePath to, LinkOptions options) {
  // The from argument is where we are creating the link, while the to is the target.
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  NormalizedFilePath normalizedFrom(kj::mv(from));
  NormalizedFilePath normalizedTo(kj::mv(to));

  // First, let's make sure the destination (from) does not already exist.
  const jsg::Url& fromUrl = normalizedFrom;
  const jsg::Url& toUrl = normalizedTo;

  JSG_REQUIRE(vfs.resolve(js, fromUrl) == kj::none, Error, "File already exists"_kj);
  // Now, let's split the fromUrl into a base directory URL and a file name so
  // that we can make sure the destination directory exists.
  jsg::Url::Relative fromRelative = fromUrl.getRelative();
  JSG_REQUIRE(fromRelative.name.size() > 0, Error, "Invalid filename"_kj);

  auto parent =
      JSG_REQUIRE_NONNULL(vfs.resolve(js, fromRelative.base), Error, "Directory does not exist"_kj);
  auto& dir = JSG_REQUIRE_NONNULL(
      parent.tryGet<kj::Rc<workerd::Directory>>(), Error, "Invalid argument"_kj);

  // Dir is where the new link will go. fromRelative.name is the name of the new link
  // in this directory.

  // If we are creating a symbolic link, we do not need to check if the target exists.
  if (options.symbolic) {
    dir->add(js, fromRelative.name, vfs.newSymbolicLink(js, toUrl));
    return;
  }

  // If we are creating a hard link, however, the target must exist.
  auto target = JSG_REQUIRE_NONNULL(
      vfs.resolve(js, toUrl, {.followLinks = false}), Error, "file not found"_kj);
  KJ_SWITCH_ONEOF(target) {
    KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
      dir->add(js, fromRelative.name, file.addRef());
    }
    KJ_CASE_ONEOF(tdir, kj::Rc<workerd::Directory>) {
      dir->add(js, fromRelative.name, tdir.addRef());
    }
    KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
      dir->add(js, fromRelative.name, link.addRef());
    }
  }
}

void FileSystemModule::unlink(jsg::Lock& js, FilePath path) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  NormalizedFilePath normalizedPath(kj::mv(path));
  const jsg::Url& url = normalizedPath;
  auto relative = url.getRelative();
  auto parent =
      JSG_REQUIRE_NONNULL(vfs.resolve(js, relative.base), Error, "Directory does not exist"_kj);
  auto& dir = JSG_REQUIRE_NONNULL(
      parent.tryGet<kj::Rc<workerd::Directory>>(), Error, "Invalid argument"_kj);
  // The unlink method cannot be used to remove directories.

  kj::Path fpath(relative.name);
  auto stat = JSG_REQUIRE_NONNULL(dir->stat(js, fpath), Error, "file not found");
  JSG_REQUIRE(stat.type != FsType::DIRECTORY, Error, "Cannot unlink a directory");

  dir->remove(js, fpath);
}

int FileSystemModule::open(jsg::Lock& js, FilePath path, OpenOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  NormalizedFilePath normalizedPath(kj::mv(path));
  auto& opened = vfs.openFd(js, normalizedPath,
      workerd::VirtualFileSystem::OpenOptions{
        .read = options.read,
        .write = options.write,
        .append = options.append,
        .exclusive = options.exclusive,
        .followLinks = options.followSymlinks,
      });
  return opened.fd;
}

void FileSystemModule::close(jsg::Lock& js, int fd) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  vfs.closeFd(js, fd);
}

uint32_t FileSystemModule::write(
    jsg::Lock& js, int fd, kj::Array<jsg::BufferSource> data, WriteOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);

  static const auto getPosition = [](jsg::Lock& js, auto& opened, auto& file,
                                      const WriteOptions& options) -> uint32_t {
    if (opened.append) {
      // If the file descriptor is opened in append mode, we ignore the position
      // option and always append to the end of the file.
      auto stat = file->stat(js);
      return stat.size;
    }
    auto pos = options.position.orDefault(opened.position);
    JSG_REQUIRE(pos <= kMax, Error, "Position out of range");
    return static_cast<uint32_t>(pos);
  };

  KJ_SWITCH_ONEOF(opened.node) {
    KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
      auto pos = getPosition(js, opened, file, options);
      uint32_t total = 0;
      for (auto& buffer: data) {
        auto written = file->write(js, pos, buffer);
        pos += written;
        total += written;
      }
      // We only update the position if the options.position is not set and
      // the file descriptor is not opened in append mode.
      if (options.position == kj::none && !opened.append) {
        opened.position += total;
      }
      return total;
    }
    KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
      JSG_FAIL_REQUIRE(Error, "Invalid operation on a directory");
    }
    KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
      // If we get here, then followSymLinks was set to false when open was called.
      // We can't write to a symbolic link.
      JSG_FAIL_REQUIRE(Error, "Invalid operation on a symlink");
    }
  }
  KJ_UNREACHABLE;
}

uint32_t FileSystemModule::read(
    jsg::Lock& js, int fd, kj::Array<jsg::BufferSource> data, WriteOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
  JSG_REQUIRE(opened.read, Error, "File descriptor not opened for reading"_kj);

  KJ_SWITCH_ONEOF(opened.node) {
    KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
      auto pos = options.position.orDefault(opened.position);
      JSG_REQUIRE(pos <= kMax, Error, "Position out of range");
      uint32_t total = 0;
      for (auto& buffer: data) {
        auto read = file->read(js, pos, buffer);
        // if read is less than the size of the buffer, we are at EOF.
        pos += read;
        total += read;
        if (read < buffer.size()) break;
      }
      // We only update the position if the options.position is not set.
      if (options.position == kj::none) {
        opened.position += total;
      }
      return total;
    }
    KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
      JSG_FAIL_REQUIRE(Error, "Invalid operation on a directory");
    }
    KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
      // If we get here, then followSymLinks was set to false when open was called.
      // We can't read from a symbolic link.
      JSG_FAIL_REQUIRE(Error, "Invalid operation on a symlink");
    }
  }
  KJ_UNREACHABLE;
}

jsg::BufferSource FileSystemModule::readAll(jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalized(kj::mv(path));
      KJ_IF_SOME(node, vfs.resolve(js, normalized)) {
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            return file->readAllBytes(js);
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            JSG_FAIL_REQUIRE(Error, "Invalid operation on a directory");
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            // We shouldn't be able to get here since we are following symlinks.
            KJ_UNREACHABLE;
          }
        }
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
      JSG_REQUIRE(opened.read, Error, "File descriptor not opened for reading"_kj);

      // Make sure we're reading from a file.
      auto& file = JSG_REQUIRE_NONNULL(
          opened.node.tryGet<kj::Rc<workerd::File>>(), Error, "Invalid operation"_kj);

      // Move the opened.position to the end of the file.
      KJ_DEFER({
        auto stat = file->stat(js);
        opened.position = stat.size;
      });

      return file->readAllBytes(js);
    }
  }
  KJ_UNREACHABLE;
}

uint32_t FileSystemModule::writeAll(jsg::Lock& js,
    kj::OneOf<int, FilePath> pathOrFd,
    jsg::BufferSource data,
    WriteAllOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);

  JSG_REQUIRE(data.size() <= kMax, Error, "Data size out of range"_kj);

  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalized(kj::mv(path));
      KJ_IF_SOME(node, vfs.resolve(js, normalized)) {
        // If the exclusive option is set, the file must not already exist.
        JSG_REQUIRE(!options.exclusive, Error, "file already exists");
        // The file already exists, we can write to it.
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            // First let's check that the file is writable.
            auto stat = file->stat(js);
            JSG_REQUIRE(stat.writable, Error, "access is denied");

            // If the append option is set, we will write to the end of the file
            // instead of overwriting it.
            if (options.append) {
              return file->write(js, stat.size, data);
            }

            // Otherwise, we overwrite the entire file.
            return file->writeAll(js, data);
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            JSG_FAIL_REQUIRE(Error, "Invalid operation on a directory");
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            // If we get here, then followSymLinks was set to false when open was called.
            // We can't write to a symbolic link.
            JSG_FAIL_REQUIRE(Error, "Invalid operation on a symlink");
          }
        }
        KJ_UNREACHABLE;
      }
      // The file does not exist. We first need to create it, then write to it.
      // Let's make sure the parent directory exists.
      const jsg::Url& url = normalized;
      jsg::Url::Relative relative = url.getRelative();
      auto parent =
          JSG_REQUIRE_NONNULL(vfs.resolve(js, relative.base), Error, "Directory does not exist"_kj);
      // Let's make sure the parent is a directory.
      auto& dir = JSG_REQUIRE_NONNULL(
          parent.tryGet<kj::Rc<workerd::Directory>>(), Error, "Invalid argument"_kj);
      auto stat = dir->stat(js);
      JSG_REQUIRE(stat.writable, Error, "access is denied");
      auto file = workerd::File::newWritable(js, static_cast<uint32_t>(data.size()));
      auto written = file->writeAll(js, data);
      dir->add(js, relative.name, kj::mv(file));
      return written;
    }
    KJ_CASE_ONEOF(fd, int) {
      auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
      // Otherwise, we'll overwrite the file...
      JSG_REQUIRE(opened.write, Error, "File descriptor not opened for writing"_kj);

      // Make sure we're writing to a file.
      auto& file = JSG_REQUIRE_NONNULL(
          opened.node.tryGet<kj::Rc<workerd::File>>(), Error, "Invalid operation"_kj);

      auto stat = file->stat(js);
      // Make sure the file is writable.
      JSG_REQUIRE(stat.writable, Error, "access is denied");

      KJ_DEFER({
        // In either case, we need to update the position of the file descriptor.
        stat = file->stat(js);
        opened.position = stat.size;
      });

      // If the file descriptor was opened in append mode, or if the append option
      // is set, then we'll use write instead to append to the end of the file.
      if (opened.append || options.append) {
        return write(js, fd, kj::arr(kj::mv(data)),
            {
              .position = stat.size,
            });
      }

      // Otherwise, we overwrite the entire file.
      return file->writeAll(js, data);
    }
  }

  KJ_UNREACHABLE;
}

void FileSystemModule::renameOrCopy(
    jsg::Lock& js, FilePath src, FilePath dest, RenameOrCopyOptions options) {
  // The source must exist, the destination must not.
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  NormalizedFilePath normalizedSrc(kj::mv(src));
  NormalizedFilePath normalizedDest(kj::mv(dest));

  auto srcNode = JSG_REQUIRE_NONNULL(vfs.resolve(js, normalizedSrc), Error, "File not found"_kj);

  const jsg::Url& destUrl = normalizedDest;
  const jsg::Url& srcUrl = normalizedSrc;
  JSG_REQUIRE(vfs.resolve(js, destUrl) == kj::none, Error, "File already exists"_kj);
  auto relative = destUrl.getRelative();
  // The destination parent must exist.
  auto parent =
      JSG_REQUIRE_NONNULL(vfs.resolve(js, relative.base), Error, "Directory does not exist"_kj);
  // Let's make sure the parent is a directory.
  auto& dir = JSG_REQUIRE_NONNULL(
      parent.tryGet<kj::Rc<workerd::Directory>>(), Error, "Invalid argument"_kj);

  kj::Maybe<kj::Rc<Directory>> srcParent;
  if (!options.copy) {
    // If we are not copying, let's make sure that the source directory is writable
    // before we actually try moving it.
    auto relative = srcUrl.getRelative();
    auto parent =
        JSG_REQUIRE_NONNULL(vfs.resolve(js, relative.base), Error, "Directory does not exist"_kj);
    auto& srcDir = JSG_REQUIRE_NONNULL(
        parent.tryGet<kj::Rc<workerd::Directory>>(), Error, "Invalid argument"_kj);
    auto stat = srcDir->stat(js);
    JSG_REQUIRE(stat.writable, Error, "access is denied");
    srcParent = srcDir.addRef();
  }

  // The next part is easy. We either clone or add ref the original node and add it to the
  // destination directory.
  KJ_SWITCH_ONEOF(srcNode) {
    KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
      auto newFile = options.copy ? file->clone(js) : file.addRef();
      dir->add(js, relative.name, kj::mv(newFile));
    }
    KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
      JSG_REQUIRE(!options.copy, Error, "Cannot copy a directory");
      dir->add(js, relative.name, dir.addRef());
    }
    KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
      dir->add(js, relative.name, link.addRef());
    }
  }

  KJ_IF_SOME(dir, srcParent) {
    auto relative = srcUrl.getRelative();
    dir->remove(js, kj::Path({relative.name}), {.recursive = true});
  }
}

jsg::Optional<kj::String> FileSystemModule::mkdir(
    jsg::Lock& js, FilePath path, MkdirOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  NormalizedFilePath normalizedPath(kj::mv(path));
  const jsg::Url& url = normalizedPath;

  // The path must not already exist. However, if the path is a directory, we
  // will just return rather than throwing an error.
  KJ_IF_SOME(node, vfs.resolve(js, url, {.followLinks = false})) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        JSG_FAIL_REQUIRE(Error, "File already exists"_kj);
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        // The directory already exists. We will just return.
        return kj::none;
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        JSG_FAIL_REQUIRE(Error, "File already exists"_kj);
      }
    }
  };

  if (options.recursive) {
    JSG_REQUIRE(!options.tmp, Error, "Cannot recursively create a temporary directory");
    // If the recursive option is set, we will create all the directories in the
    // path that do not exist, returning the path to the first one that was created.
    const kj::Path kjPath = normalizedPath;
    const auto parentPath = kjPath.parent();
    const auto name = kjPath.basename();
    kj::Maybe<kj::String> createdPath;

    // We'll start from the root and work our way down.
    auto current = vfs.getRoot(js);
    kj::Path currentPath{};
    for (const auto& part: parentPath) {
      currentPath = currentPath.append(part);
      // Try opening the next part of the path. Note that we are not using the
      // createAs option here because we don't necessarily want to implicitly
      // create the directory if it doesn't exist. We want to create it explicitly
      // so that we can return the path to the first directory that was created
      // and tryOpen does not us if the directory already existed or was created.
      KJ_IF_SOME(node, current->tryOpen(js, kj::Path({part}))) {
        // Let's make sure the node is a directory.
        auto& dir = JSG_REQUIRE_NONNULL(
            node.tryGet<kj::Rc<workerd::Directory>>(), Error, "Invalid argument"_kj);
        // Now we can check the next part of the path.
        current = kj::mv(dir);
        continue;
      }

      // The node does not exist, let's create it so long as the current
      // directory is writable.
      auto stat = current->stat(js);
      JSG_REQUIRE(stat.writable, Error, "access is denied");
      auto dir = workerd::Directory::newWritable();
      current->add(js, part, dir.addRef());
      current = kj::mv(dir);
      if (createdPath == kj::none) {
        createdPath = currentPath.toString(true);
      }
    }

    // Now that we have th parent directory, let's try creating the new directory.
    auto newDir = workerd::Directory::newWritable();
    current->add(js, name.toString(false), kj::mv(newDir));
    return kj::mv(createdPath);
  }

  // If the recursive option is not set, we will create the directory only if
  // the parent directory exists. If the parent directory does not exist, we
  // will return an error.
  auto relative = url.getRelative();
  auto parent =
      JSG_REQUIRE_NONNULL(vfs.resolve(js, relative.base), Error, "Directory does not exist"_kj);
  // Let's make sure the parent is a directory.
  auto& dir = JSG_REQUIRE_NONNULL(
      parent.tryGet<kj::Rc<workerd::Directory>>(), Error, "Invalid argument"_kj);
  auto stat = dir->stat(js);
  JSG_REQUIRE(stat.writable, Error, "access is denied");
  auto newDir = workerd::Directory::newWritable();

  if (options.tmp) {
    JSG_REQUIRE(tmpFileCounter < kMax, Error, "Too many temporary directories");
    auto name = kj::str(relative.name, tmpFileCounter++);
    dir->add(js, name, kj::mv(newDir));
    auto newUrl = KJ_REQUIRE_NONNULL(relative.base.resolve(name), "invalid URL");
    return kj::str(newUrl.getPathname());
  }

  dir->add(js, relative.name, kj::mv(newDir));
  return kj::none;
}

void FileSystemModule::rm(jsg::Lock& js, FilePath path, RmOptions options) {
  // TODO(node:fs): Implement the force option.
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  NormalizedFilePath normalizedPath(kj::mv(path));
  const jsg::Url& url = normalizedPath;
  auto relative = url.getRelative();
  auto parent =
      JSG_REQUIRE_NONNULL(vfs.resolve(js, relative.base), Error, "Directory does not exist"_kj);
  // Let's make sure the parent is a directory.
  auto& dir = JSG_REQUIRE_NONNULL(
      parent.tryGet<kj::Rc<workerd::Directory>>(), Error, "Invalid argument"_kj);
  auto stat = dir->stat(js);
  JSG_REQUIRE(stat.writable, Error, "access is denied");

  kj::Path name(relative.name);

  if (options.dironly) {
    // If the dironly option is set, we will only remove the entry if it is a directory.
    auto stat = JSG_REQUIRE_NONNULL(dir->stat(js, name), Error, "file not found");
    JSG_REQUIRE(stat.type == workerd::FsType::DIRECTORY, Error, "Not a directory");
  }

  dir->remove(js, name, {.recursive = options.recursive});
}

namespace {
static constexpr int UV_DIRENT_FILE = 1;
static constexpr int UV_DIRENT_DIR = 2;
static constexpr int UV_DIRENT_LINK = 3;
static constexpr int UV_DIRENT_CHAR = 6;
void readdirImpl(jsg::Lock& js,
    const workerd::VirtualFileSystem& vfs,
    kj::Rc<workerd::Directory>& dir,
    const kj::Path& path,
    const FileSystemModule::ReadDirOptions& options,
    kj::Vector<FileSystemModule::DirEntHandle>& entries) {
  for (auto& entry: *dir.get()) {
    auto name = options.recursive ? path.append(entry.key).toString(false) : kj::str(entry.key);
    KJ_SWITCH_ONEOF(entry.value) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        auto stat = file->stat(js);
        entries.add(FileSystemModule::DirEntHandle{
          .name = kj::mv(name),
          .parentPath = path.toString(true),
          .type = stat.device ? UV_DIRENT_CHAR : UV_DIRENT_FILE,
        });
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        entries.add(FileSystemModule::DirEntHandle{
          .name = kj::mv(name),
          .parentPath = path.toString(true),
          .type = UV_DIRENT_DIR,
        });

        if (options.recursive) {
          readdirImpl(js, vfs, dir, path.append(entry.key), options, entries);
        }
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        entries.add(FileSystemModule::DirEntHandle{
          .name = kj::mv(name),
          .parentPath = path.toString(true),
          .type = UV_DIRENT_LINK,
        });

        if (options.recursive) {
          workerd::SymbolicLinkRecursionGuardScope guard;
          guard.checkSeen(link.get());
          KJ_IF_SOME(target, link->resolve(js)) {
            KJ_SWITCH_ONEOF(target) {
              KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
                // Do nothing
              }
              KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
                readdirImpl(js, vfs, dir, path.append(entry.key), options, entries);
              }
            }
          }
        }
      }
    }
  }
}
}  // namespace

kj::Array<FileSystemModule::DirEntHandle> FileSystemModule::readdir(
    jsg::Lock& js, FilePath path, ReadDirOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  NormalizedFilePath normalizedPath(kj::mv(path));

  auto node = JSG_REQUIRE_NONNULL(
      vfs.resolve(js, normalizedPath, {.followLinks = false}), Error, "file not found"_kj);
  auto& dir =
      JSG_REQUIRE_NONNULL(node.tryGet<kj::Rc<workerd::Directory>>(), Error, "not a directory"_kj);

  kj::Vector<DirEntHandle> entries;
  readdirImpl(js, vfs, dir, normalizedPath, options, entries);
  return entries.releaseAsArray();
}

// =======================================================================================
// Implementation of the Web File System API

namespace {
constexpr bool isValidFileName(kj::StringPtr name) {
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
  KJ_IF_SOME(node,
      inner->tryOpen(js, kj::Path({name}),
          Directory::OpenOptions{
            .createAs = createAs,
          })) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        return js.resolvedPromise(js.alloc<FileSystemFileHandle>(kj::mv(name), kj::mv(file)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        auto ex =
            js.domException(kj::str("TypeMismatchError"), kj::str("File name is a directory"));
        return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(exception.wrap(js, kj::mv(ex)));
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        KJ_UNREACHABLE;
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
  KJ_IF_SOME(node,
      inner->tryOpen(js, kj::Path({name}),
          Directory::OpenOptions{
            .createAs = createAs,
          })) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        return js.resolvedPromise(js.alloc<FileSystemDirectoryHandle>(kj::mv(name), kj::mv(dir)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::File>) {
        auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("File name is a file"));
        return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(
            exception.wrap(js, kj::mv(ex)));
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        KJ_UNREACHABLE;
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

  // Per the spec, the writable stream we create here is expected to write into
  // a temporary space until the stream is closed. When closed, the original file
  // contents are replaced with the new contents. If the stream is aborted or
  // errored, the temporary file data is discarded.

  auto opts = options.orDefault(FileSystemCreateWritableOptions{});

  // If keepExistingData is true, the temporary file is created with a copy of
  // the original file data. Otherwise, the temporary file is created empty,
  // which means that if we create a writable stream and close it without writing
  // anything, the original file data is lost.
  bool keepExistingData = opts.keepExistingData.orDefault(false);

  auto sharedState = kj::rc<FileSystemWritableFileStream::State>(
      inner.addRef(), keepExistingData ? inner->clone(js) : workerd::File::newWritable(js));
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
      auto& inner = JSG_REQUIRE_NONNULL(state->temp, DOMInvalidStateError, "File handle closed");
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
    // When aborted, we just drop any of the written data on the floor.
    state->clear();
    return js.resolvedPromise();
  },
        .close =
            [state = sharedState.addRef()](jsg::Lock& js) mutable {
    KJ_DEFER(state->clear());
    return js.tryCatch([&] {
      KJ_IF_SOME(temp, state->temp) {
        KJ_IF_SOME(file, state->file) {
          file->replace(js, kj::mv(temp));
        }
      }
      return js.resolvedPromise();
    }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
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

uint32_t FileSystemSyncAccessHandle::read(
    jsg::Lock& js, jsg::BufferSource buffer, jsg::Optional<FileSystemReadWriteOptions> options) {
  auto& inner = JSG_REQUIRE_NONNULL(this->inner, DOMInvalidStateError, "File handle closed");
  auto offset = options.orDefault({}).at.orDefault(position);
  auto stat = inner->stat(js);
  if (offset > stat.size && !stat.device) {
    position = stat.size;
    return 0;
  }
  auto ret = inner->read(js, offset, buffer);

  position += ret;
  return ret;
}

uint32_t FileSystemSyncAccessHandle::write(
    jsg::Lock& js, jsg::BufferSource buffer, jsg::Optional<FileSystemReadWriteOptions> options) {
  auto& inner = JSG_REQUIRE_NONNULL(this->inner, DOMInvalidStateError, "File handle closed");
  auto offset = options.orDefault({}).at.orDefault(position);
  auto stat = inner->stat(js);
  if (offset > stat.size) {
    inner->resize(js, offset + buffer.size());
  }
  auto ret = inner->write(js, offset, buffer);
  position = offset + ret;
  return ret;
}

void FileSystemSyncAccessHandle::truncate(jsg::Lock& js, uint32_t newSize) {
  auto& inner = JSG_REQUIRE_NONNULL(this->inner, DOMInvalidStateError, "File handle closed");
  inner->resize(js, newSize);
  auto stat = inner->stat(js);
  if (position > stat.size) {
    position = stat.size;
  }
}

uint32_t FileSystemSyncAccessHandle::getSize(jsg::Lock& js) {
  auto& inner = JSG_REQUIRE_NONNULL(this->inner, DOMInvalidStateError, "File handle closed");
  return inner->stat(js).size;
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
  auto& inner = JSG_REQUIRE_NONNULL(sharedState->temp, DOMInvalidStateError, "File handle closed");
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
        uint32_t offset = sharedState->position;
        KJ_IF_SOME(offset, params.position) {
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
          uint32_t pos = params.position.orDefault(0);
          sharedState->position += pos;
          auto stat = inner->stat(js);
          if (sharedState->position > stat.size) {
            inner->resize(js, sharedState->position);
          }
        } else if (params.type == "truncate"_kj) {
          uint32_t size = params.size.orDefault(0);
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

jsg::Promise<void> FileSystemWritableFileStream::seek(jsg::Lock& js, uint32_t position) {
  auto& inner = JSG_REQUIRE_NONNULL(sharedState->temp, DOMInvalidStateError, "File handle closed");
  auto stat = inner->stat(js);
  if (position > stat.size) {
    inner->resize(js, position);
  }
  sharedState->position = position;
  return js.resolvedPromise();
}

jsg::Promise<void> FileSystemWritableFileStream::truncate(jsg::Lock& js, uint32_t size) {
  auto& inner = JSG_REQUIRE_NONNULL(sharedState->temp, DOMInvalidStateError, "File handle closed");
  inner->resize(js, size);
  auto stat = inner->stat(js);
  if (sharedState->position > stat.size) {
    sharedState->position = stat.size;
  }
  return js.resolvedPromise();
}

}  // namespace workerd::api
