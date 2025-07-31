#include "filesystem.h"

#include "blob.h"
#include "url-standard.h"
#include "url.h"

#include <workerd/api/node/exceptions.h>
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
  const jsg::Url url;

  static jsg::Url normalize(FileSystemModule::FilePath path) {
    KJ_SWITCH_ONEOF(path) {
      KJ_CASE_ONEOF(legacy, jsg::Ref<URL>) {
        auto parsed = JSG_REQUIRE_NONNULL(
            jsg::Url::tryParse(legacy->getHref(), "file:///"_kj), Error, "Invalid URL"_kj);
        // The cloning here is necessary to de-percent-encode characters in the
        // path that don't need to be percent-encoded, allowing us to treat equivalent
        // encodings of the same path as equal. For instance, '/foo' and '/%66oo' should
        // be considered the same path since 'f' and '%66' are equivalent. Importantly,
        // this retains percent-encoding on characters that do need to be percent-encoded
        // to be valid in URLs, such as non-ASCII characters.
        return parsed.clone(jsg::Url::EquivalenceOption::NORMALIZE_PATH);
      }
      KJ_CASE_ONEOF(standard, jsg::Ref<url::URL>) {
        jsg::Url url = *standard;
        return url.clone(jsg::Url::EquivalenceOption::NORMALIZE_PATH);
      }
    }
    KJ_UNREACHABLE;
  }

  NormalizedFilePath(FileSystemModule::FilePath path): url(normalize(kj::mv(path))) {
    validate();
  }

  void validate() {
    JSG_REQUIRE(url.getProtocol() == "file:"_kj, TypeError, "File path must be a file: URL");
    JSG_REQUIRE(url.getHost().size() == 0, Error, "File path must not have a host");
  }

  operator const jsg::Url&() const {
    return url;
  }

  operator const kj::Path() const {
    auto path = kj::str(url.getPathname().slice(1));
    kj::Path root{};
    return root.eval(path);
  }
};

[[noreturn]] void throwFsError(jsg::Lock& js, workerd::FsError error, kj::StringPtr syscall) {
  switch (error) {
    case workerd::FsError::NOT_DIRECTORY: {
      node::THROW_ERR_UV_ENOTDIR(js, syscall);
    }
    case workerd::FsError::NOT_EMPTY: {
      node::THROW_ERR_UV_ENOTEMPTY(js, syscall);
    }
    case workerd::FsError::READ_ONLY: {
      node::THROW_ERR_UV_EPERM(js, syscall);
    }
    case workerd::FsError::TOO_MANY_OPEN_FILES: {
      node::THROW_ERR_UV_EMFILE(js, syscall);
    }
    case workerd::FsError::ALREADY_EXISTS: {
      node::THROW_ERR_UV_EEXIST(js, syscall);
    }
    case workerd::FsError::NOT_SUPPORTED: {
      node::THROW_ERR_UV_ENOSYS(js, syscall);
    }
    case workerd::FsError::NOT_PERMITTED: {
      node::THROW_ERR_UV_EPERM(js, syscall);
    }
    case workerd::FsError::NOT_PERMITTED_ON_DIRECTORY: {
      node::THROW_ERR_UV_EISDIR(js, syscall);
    }
    case workerd::FsError::FAILED: {
      node::THROW_ERR_UV_EIO(js, syscall);
    }
    case workerd::FsError::INVALID_PATH: {
      node::THROW_ERR_UV_EINVAL(js, syscall, "Invalid path"_kj);
    }
    case workerd::FsError::FILE_SIZE_LIMIT_EXCEEDED: {
      node::THROW_ERR_UV_EPERM(js, syscall, "File size limit exceeded"_kj);
    }
    case workerd::FsError::SYMLINK_DEPTH_EXCEEDED: {
      node::THROW_ERR_UV_ELOOP(js, syscall, "symlink depth exceeded"_kj);
    }
    default: {
      node::THROW_ERR_UV_EPERM(js, syscall);
    }
  }
  KJ_UNREACHABLE;
}
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
  auto& vfs = workerd::VirtualFileSystem::current(js);
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
          KJ_CASE_ONEOF(err, workerd::FsError) {
            // If we got here, then the path was not found.
            throwFsError(js, err, "stat"_kj);
          }
        }
        KJ_UNREACHABLE;
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      KJ_IF_SOME(opened, vfs.tryGetFd(js, fd)) {
        KJ_SWITCH_ONEOF(opened->node) {
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
      } else {
        node::THROW_ERR_UV_EBADF(js, "fstat"_kj);
      }
    }
  }
  return kj::none;
}

void FileSystemModule::setLastModified(
    jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd, kj::Date lastModified, StatOptions options) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalizedPath(kj::mv(path));
      KJ_IF_SOME(node,
          vfs.resolve(
              js, normalizedPath, {.followLinks = options.followSymlinks.orDefault(true)})) {
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            KJ_IF_SOME(err, file->setLastModified(js, lastModified)) {
              // If we got here, then the file is read-only.
              throwFsError(js, err, "futimes"_kj);
            }
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
          KJ_CASE_ONEOF(err, workerd::FsError) {
            // If we got here, then the path was not found.
            throwFsError(js, err, "futimes"_kj);
          }
        }
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      KJ_IF_SOME(opened, vfs.tryGetFd(js, fd)) {
        KJ_SWITCH_ONEOF(opened->node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            KJ_IF_SOME(err, file->setLastModified(js, lastModified)) {
              throwFsError(js, err, "futimes"_kj);
            }
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
        KJ_UNREACHABLE;
      } else {
        node::THROW_ERR_UV_EBADF(js, "futimes"_kj);
      }
    }
  }
  KJ_UNREACHABLE;
}

void FileSystemModule::truncate(jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd, uint32_t size) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalizedPath(kj::mv(path));
      KJ_IF_SOME(node, vfs.resolve(js, normalizedPath)) {
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            KJ_IF_SOME(err, file->resize(js, size)) {
              throwFsError(js, err, "ftruncate"_kj);
            }
            return;
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            node::THROW_ERR_UV_EISDIR(js, "ftruncate"_kj);
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            // If we got here, then followSymLinks was set to false. We cannot
            // truncate a symbolic link.
            node::THROW_ERR_UV_EINVAL(js, "ftruncate"_kj);
          }
          KJ_CASE_ONEOF(err, workerd::FsError) {
            // If we got here, then the path was not found.
            throwFsError(js, err, "ftruncate"_kj);
          }
        }
      } else {
        node::THROW_ERR_UV_ENOENT(js, "ftruncate"_kj);
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      KJ_IF_SOME(opened, vfs.tryGetFd(js, fd)) {
        KJ_SWITCH_ONEOF(opened->node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            KJ_IF_SOME(err, file->resize(js, size)) {
              throwFsError(js, err, "ftruncate"_kj);
            }
            return;
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            node::THROW_ERR_UV_EISDIR(js, "ftruncate"_kj);
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            node::THROW_ERR_UV_EINVAL(js, "ftruncate"_kj);
          }
        }
        KJ_UNREACHABLE;
      } else {
        node::THROW_ERR_UV_EBADF(js, "ftruncate"_kj);
      }
    }
  }
  KJ_UNREACHABLE;
}

kj::String FileSystemModule::readLink(jsg::Lock& js, FilePath path, ReadLinkOptions options) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  NormalizedFilePath normalizedPath(kj::mv(path));
  KJ_IF_SOME(node, vfs.resolve(js, normalizedPath, {.followLinks = false})) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        if (options.failIfNotSymlink) {
          node::THROW_ERR_UV_EINVAL(js, "readlink"_kj);
        }
        kj::Path path = normalizedPath;
        return path.toString(true);
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        if (options.failIfNotSymlink) {
          node::THROW_ERR_UV_EINVAL(js, "readlink"_kj);
        }
        kj::Path path = normalizedPath;
        return path.toString(true);
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        return link->getTargetPath().toString(true);
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        // If we got here, then the path was not found.
        throwFsError(js, err, "readlink"_kj);
      }
    }
    KJ_UNREACHABLE;
  } else {
    node::THROW_ERR_UV_ENOENT(js, "readlink"_kj);
  }
}

void FileSystemModule::link(jsg::Lock& js, FilePath from, FilePath to, LinkOptions options) {
  // The from argument is where we are creating the link, while the to is the target.
  auto& vfs = workerd::VirtualFileSystem::current(js);
  NormalizedFilePath normalizedFrom(kj::mv(from));
  NormalizedFilePath normalizedTo(kj::mv(to));

  // First, let's make sure the destination (from) does not already exist.
  const jsg::Url& fromUrl = normalizedFrom;
  const jsg::Url& toUrl = normalizedTo;

  KJ_IF_SOME(maybeNode, vfs.resolve(js, fromUrl)) {
    KJ_IF_SOME(err, maybeNode.tryGet<workerd::FsError>()) {
      throwFsError(js, err, "link"_kj);
    }
    // If we got here, then the destination already exists.
    node::THROW_ERR_UV_EEXIST(js, "link"_kj, "File already exists"_kj);
  }

  // Now, let's split the fromUrl into a base directory URL and a file name so
  // that we can make sure the destination directory exists.
  jsg::Url::Relative fromRelative = fromUrl.getRelative();

  if (fromRelative.name.size() == 0) {
    node::THROW_ERR_UV_EINVAL(js, "link"_kj, "Invalid filename"_kj);
  }

  KJ_IF_SOME(parent, vfs.resolve(js, fromRelative.base)) {
    KJ_IF_SOME(dir, parent.tryGet<kj::Rc<workerd::Directory>>()) {
      // Dir is where the new link will go. fromRelative.name is the name of
      // the new link in this directory.

      // If we are creating a symbolic link, we do not need to check if the target exists.
      if (options.symbolic) {
        KJ_IF_SOME(err, dir->add(js, fromRelative.name, vfs.newSymbolicLink(js, toUrl))) {
          throwFsError(js, err, "link"_kj);
        }
        return;
      }

      // If we are creating a hard link, however, the target must exist.
      KJ_IF_SOME(target, vfs.resolve(js, toUrl, {.followLinks = false})) {
        KJ_SWITCH_ONEOF(target) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            KJ_IF_SOME(err, dir->add(js, fromRelative.name, file.addRef())) {
              throwFsError(js, err, "link"_kj);
            }
          }
          KJ_CASE_ONEOF(tdir, kj::Rc<workerd::Directory>) {
            // It is not permitted to hardlink to a directory.
            node::THROW_ERR_UV_EPERM(js, "link"_kj, "Cannot hardlink to a directory"_kj);
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            KJ_IF_SOME(err, dir->add(js, fromRelative.name, link.addRef())) {
              throwFsError(js, err, "link"_kj);
            }
          }
          KJ_CASE_ONEOF(err, workerd::FsError) {
            // If we got here, then the target path was not found.
            throwFsError(js, err, "link"_kj);
          }
        }
      } else {
        node::THROW_ERR_UV_ENOENT(js, "link"_kj, "File not found"_kj);
      }
    } else {
      node::THROW_ERR_UV_EINVAL(js, "link"_kj, "Not a directory"_kj);
    }
  } else {
    node::THROW_ERR_UV_ENOENT(js, "link"_kj, "Directory does not exist"_kj);
  }
}

void FileSystemModule::unlink(jsg::Lock& js, FilePath path) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  NormalizedFilePath normalizedPath(kj::mv(path));
  const jsg::Url& url = normalizedPath;
  auto relative = url.getRelative();

  KJ_IF_SOME(parent, vfs.resolve(js, relative.base)) {
    KJ_IF_SOME(dir, parent.tryGet<kj::Rc<workerd::Directory>>()) {
      kj::Path fpath(relative.name);
      KJ_IF_SOME(stat, dir->stat(js, fpath)) {
        KJ_SWITCH_ONEOF(stat) {
          KJ_CASE_ONEOF(stat, workerd::Stat) {
            if (stat.type == FsType::DIRECTORY) {
              node::THROW_ERR_UV_EISDIR(js, "unlink"_kj, "Cannot unlink a directory"_kj);
            }
          }
          KJ_CASE_ONEOF(err, workerd::FsError) {
            throwFsError(js, err, "unlink"_kj);
          }
        }
      } else {
        node::THROW_ERR_UV_ENOENT(js, "unlink"_kj, "File not found"_kj);
      }

      KJ_SWITCH_ONEOF(dir->remove(js, fpath)) {
        KJ_CASE_ONEOF(res, bool) {
          // Ignore the return.
        }
        KJ_CASE_ONEOF(err, workerd::FsError) {
          throwFsError(js, err, "unlink"_kj);
        }
      }
    } else {
      node::THROW_ERR_UV_ENOTDIR(js, "unlink"_kj, "Parent path is not a directory"_kj);
    }
  } else {
    node::THROW_ERR_UV_ENOENT(js, "unlink"_kj, "File not found"_kj);
  }
}

int FileSystemModule::open(jsg::Lock& js, FilePath path, OpenOptions options) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  NormalizedFilePath normalizedPath(kj::mv(path));
  KJ_SWITCH_ONEOF(vfs.openFd(js, normalizedPath,
                      workerd::VirtualFileSystem::OpenOptions{
                        .read = options.read,
                        .write = options.write,
                        .append = options.append,
                        .exclusive = options.exclusive,
                        .followLinks = options.followSymlinks,
                      })) {
    KJ_CASE_ONEOF(opened, kj::Rc<workerd::VirtualFileSystem::OpenedFile>) {
      return opened->fd;
    }
    KJ_CASE_ONEOF(err, workerd::FsError) {
      throwFsError(js, err, "open"_kj);
    }
  }
  KJ_UNREACHABLE;
}

void FileSystemModule::close(jsg::Lock& js, int fd) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  vfs.closeFd(js, fd);
}

uint32_t FileSystemModule::write(
    jsg::Lock& js, int fd, kj::Array<jsg::BufferSource> data, WriteOptions options) {
  auto& vfs = workerd::VirtualFileSystem::current(js);

  KJ_IF_SOME(opened, vfs.tryGetFd(js, fd)) {
    static const auto getPosition = [](jsg::Lock& js, auto& opened, auto& file,
                                        const WriteOptions& options) -> uint32_t {
      if (opened->append) {
        // If the file descriptor is opened in append mode, we ignore the position
        // option and always append to the end of the file.
        auto stat = file->stat(js);
        return stat.size;
      }
      auto pos = options.position.orDefault(opened->position);
      if (pos > kMax) {
        node::THROW_ERR_UV_EINVAL(js, "write"_kj, "position out of range"_kj);
      }
      return static_cast<uint32_t>(pos);
    };

    KJ_SWITCH_ONEOF(opened->node) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        auto pos = getPosition(js, opened, file, options);
        uint32_t total = 0;
        for (auto& buffer: data) {
          KJ_SWITCH_ONEOF(file->write(js, pos, buffer)) {
            KJ_CASE_ONEOF(written, uint32_t) {
              pos += written;
              total += written;
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              throwFsError(js, err, "write"_kj);
            }
          }
        }
        // We only update the position if the options.position is not set and
        // the file descriptor is not opened in append mode.
        if (options.position == kj::none && !opened->append) {
          opened->position += total;
        }
        return total;
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        node::THROW_ERR_UV_EISDIR(js, "write"_kj);
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        // If we get here, then followSymLinks was set to false when open was called.
        // We can't write to a symbolic link.
        node::THROW_ERR_UV_EINVAL(js, "write"_kj);
      }
    }
    KJ_UNREACHABLE;
  } else {
    node::THROW_ERR_UV_EBADF(js, "write"_kj);
  }
}

uint32_t FileSystemModule::read(
    jsg::Lock& js, int fd, kj::Array<jsg::BufferSource> data, WriteOptions options) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  KJ_IF_SOME(opened, vfs.tryGetFd(js, fd)) {
    if (!opened->read) {
      node::THROW_ERR_UV_EBADF(js, "read"_kj);
    }

    KJ_SWITCH_ONEOF(opened->node) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        auto pos = options.position.orDefault(opened->position);
        if (pos > kMax) {
          node::THROW_ERR_UV_EINVAL(js, "read"_kj, "position out of range"_kj);
        }
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
          opened->position += total;
        }
        return total;
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        node::THROW_ERR_UV_EISDIR(js, "read"_kj);
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        // If we get here, then followSymLinks was set to false when open was called.
        // We can't read from a symbolic link.
        node::THROW_ERR_UV_EINVAL(js, "read"_kj);
      }
    }
    KJ_UNREACHABLE;
  } else {
    node::THROW_ERR_UV_EBADF(js, "read"_kj);
  }
}

jsg::BufferSource FileSystemModule::readAll(jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalized(kj::mv(path));
      KJ_IF_SOME(node, vfs.resolve(js, normalized)) {
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            KJ_SWITCH_ONEOF(file->readAllBytes(js)) {
              KJ_CASE_ONEOF(data, jsg::BufferSource) {
                return kj::mv(data);
              }
              KJ_CASE_ONEOF(err, workerd::FsError) {
                throwFsError(js, err, "readAll"_kj);
              }
            }
            KJ_UNREACHABLE;
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            node::THROW_ERR_UV_EISDIR(js, "readAll"_kj);
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            // We shouldn't be able to get here since we are following symlinks.
            KJ_UNREACHABLE;
          }
          KJ_CASE_ONEOF(err, workerd::FsError) {
            throwFsError(js, err, "readAll"_kj);
          }
        }
      } else {
        node::THROW_ERR_UV_ENOENT(js, "readAll"_kj);
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      KJ_IF_SOME(opened, vfs.tryGetFd(js, fd)) {
        if (!opened->read) {
          node::THROW_ERR_UV_EBADF(js, "fread"_kj);
        }

        KJ_IF_SOME(file, opened->node.tryGet<kj::Rc<workerd::File>>()) {
          // Move the opened.position to the end of the file.
          KJ_DEFER({
            auto stat = file->stat(js);
            opened->position = stat.size;
          });

          KJ_SWITCH_ONEOF(file->readAllBytes(js)) {
            KJ_CASE_ONEOF(data, jsg::BufferSource) {
              return kj::mv(data);
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              throwFsError(js, err, "freadAll"_kj);
            }
          }
          KJ_UNREACHABLE;
        } else {
          node::THROW_ERR_UV_EBADF(js, "fread"_kj);
        }
      } else {
        node::THROW_ERR_UV_EBADF(js, "fread"_kj);
      }
    }
  }
  KJ_UNREACHABLE;
}

uint32_t FileSystemModule::writeAll(jsg::Lock& js,
    kj::OneOf<int, FilePath> pathOrFd,
    jsg::BufferSource data,
    WriteAllOptions options) {
  auto& vfs = workerd::VirtualFileSystem::current(js);

  if (data.size() > kMax) {
    node::THROW_ERR_UV_EFBIG(js, "writeAll"_kj);
  }

  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalized(kj::mv(path));
      KJ_IF_SOME(node, vfs.resolve(js, normalized)) {
        // If the exclusive option is set, the file must not already exist.
        if (options.exclusive) {
          node::THROW_ERR_UV_EEXIST(js, "writeAll"_kj, "file already exists"_kj);
        }
        // The file exists, we can write to it.
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            // First let's check that the file is writable.
            auto stat = file->stat(js);
            if (!stat.writable) {
              node::THROW_ERR_UV_EPERM(js, "writeAll"_kj);
            }

            // If the append option is set, we will write to the end of the file
            // instead of overwriting it.
            if (options.append) {
              KJ_SWITCH_ONEOF(file->write(js, stat.size, data)) {
                KJ_CASE_ONEOF(written, uint32_t) {
                  return written;
                }
                KJ_CASE_ONEOF(err, workerd::FsError) {
                  throwFsError(js, err, "writeAll"_kj);
                }
              }
              KJ_UNREACHABLE;
            }

            // Otherwise, we overwrite the entire file.
            KJ_SWITCH_ONEOF(file->writeAll(js, data)) {
              KJ_CASE_ONEOF(written, uint32_t) {
                return written;
              }
              KJ_CASE_ONEOF(err, workerd::FsError) {
                throwFsError(js, err, "writeAll"_kj);
              }
            }
            KJ_UNREACHABLE;
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            node::THROW_ERR_UV_EISDIR(js, "writeAll"_kj);
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            // If we get here, then followSymLinks was set to false when open was called.
            // We can't write to a symbolic link.
            node::THROW_ERR_UV_EINVAL(js, "writeAll"_kj);
          }
          KJ_CASE_ONEOF(err, workerd::FsError) {
            throwFsError(js, err, "writeAll"_kj);
          }
        }
        KJ_UNREACHABLE;
      }
      // The file does not exist. We first need to create it, then write to it.
      // Let's make sure the parent directory exists.
      const jsg::Url& url = normalized;
      jsg::Url::Relative relative = url.getRelative();

      KJ_IF_SOME(parent, vfs.resolve(js, relative.base)) {
        // Let's make sure the parent is a directory.
        KJ_SWITCH_ONEOF(parent) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            node::THROW_ERR_UV_ENOTDIR(js, "writeAll"_kj);
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            auto stat = dir->stat(js);
            if (!stat.writable) {
              node::THROW_ERR_UV_EPERM(js, "writeAll"_kj);
            }
            auto file = workerd::File::newWritable(js, static_cast<uint32_t>(data.size()));
            KJ_SWITCH_ONEOF(file->writeAll(js, data)) {
              KJ_CASE_ONEOF(written, uint32_t) {
                KJ_IF_SOME(err, dir->add(js, relative.name, kj::mv(file))) {
                  throwFsError(js, err, "writeAll"_kj);
                }
                return written;
              }
              KJ_CASE_ONEOF(err, workerd::FsError) {
                throwFsError(js, err, "writeAll"_kj);
              }
            }
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            // If we get here, then followSymLinks was set to false when open was called.
            // We can't write to a symbolic link.
            node::THROW_ERR_UV_EINVAL(js, "writeAll"_kj);
          }
          KJ_CASE_ONEOF(err, workerd::FsError) {
            // If we got here, then the parent path was not found.
            throwFsError(js, err, "writeAll"_kj);
          }
        }
        KJ_UNREACHABLE;
      } else {
        node::THROW_ERR_UV_ENOENT(js, "writeAll"_kj);
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      KJ_IF_SOME(opened, vfs.tryGetFd(js, fd)) {
        // Otherwise, we'll overwrite the file...
        if (!opened->write) {
          node::THROW_ERR_UV_EBADF(js, "fwrite"_kj);
        }

        KJ_IF_SOME(file, opened->node.tryGet<kj::Rc<workerd::File>>()) {
          auto stat = file->stat(js);

          if (!stat.writable) {
            node::THROW_ERR_UV_EPERM(js, "fwrite"_kj);
          }

          KJ_DEFER({
            // In either case, we need to update the position of the file descriptor.
            stat = file->stat(js);
            opened->position = stat.size;
          });

          // If the file descriptor was opened in append mode, or if the append option
          // is set, then we'll use write instead to append to the end of the file.
          if (opened->append || options.append) {
            return write(js, fd, kj::arr(kj::mv(data)),
                {
                  .position = stat.size,
                });
          }

          // Otherwise, we overwrite the entire file.
          KJ_SWITCH_ONEOF(file->writeAll(js, data)) {
            KJ_CASE_ONEOF(written, uint32_t) {
              return written;
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              throwFsError(js, err, "fwriteAll"_kj);
            }
          }
          KJ_UNREACHABLE;
        } else {
          node::THROW_ERR_UV_EBADF(js, "fwrite"_kj);
        }
      } else {
        node::THROW_ERR_UV_EBADF(js, "fwrite"_kj);
      }
    }
  }

  KJ_UNREACHABLE;
}

void FileSystemModule::renameOrCopy(
    jsg::Lock& js, FilePath src, FilePath dest, RenameOrCopyOptions options) {
  // The source must exist, the destination must not.
  auto& vfs = workerd::VirtualFileSystem::current(js);
  NormalizedFilePath normalizedSrc(kj::mv(src));
  NormalizedFilePath normalizedDest(kj::mv(dest));

  const jsg::Url& destUrl = normalizedDest;
  const jsg::Url& srcUrl = normalizedSrc;

  auto opName = options.copy ? "copy"_kj : "rename"_kj;

  KJ_IF_SOME(maybeDestNode, vfs.resolve(js, destUrl)) {
    KJ_IF_SOME(err, maybeDestNode.tryGet<workerd::FsError>()) {
      throwFsError(js, err, "rename"_kj);
    }
    node::THROW_ERR_UV_EEXIST(js, opName);
  }

  auto relative = destUrl.getRelative();
  // The destination parent must exist.
  KJ_IF_SOME(parent, vfs.resolve(js, relative.base)) {
    KJ_SWITCH_ONEOF(parent) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        node::THROW_ERR_UV_ENOTDIR(js, opName);
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        kj::Maybe<kj::Rc<Directory>> srcParent;
        if (!options.copy) {
          // If we are not copying, let's make sure that the source directory is writable
          // before we actually try moving it.
          auto relative = srcUrl.getRelative();
          KJ_IF_SOME(parent, vfs.resolve(js, relative.base)) {
            KJ_SWITCH_ONEOF(parent) {
              KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
                node::THROW_ERR_UV_ENOTDIR(js, opName);
              }
              KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
                // We can only rename a file or directory if the parent is writable.
                // If the parent is not writable, we throw an error.
                auto stat = dir->stat(js);
                if (!stat.writable) {
                  node::THROW_ERR_UV_EPERM(js, opName);
                }
                srcParent = dir.addRef();
              }
              KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
                node::THROW_ERR_UV_ENOTDIR(js, opName);
              }
              KJ_CASE_ONEOF(err, workerd::FsError) {
                // If we got here, then the parent path was not found.
                throwFsError(js, err, opName);
              }
            }
          } else {
            node::THROW_ERR_UV_ENOENT(js, opName);
          }
        }

        KJ_IF_SOME(srcNode, vfs.resolve(js, normalizedSrc)) {
          // The next part is easy. We either clone or add ref the original node and add it to the
          // destination directory.
          KJ_SWITCH_ONEOF(srcNode) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              kj::OneOf<workerd::FsError, kj::Rc<workerd::File>> errOrFile =
                  options.copy ? file->clone(js) : file.addRef();
              KJ_SWITCH_ONEOF(errOrFile) {
                KJ_CASE_ONEOF(err, workerd::FsError) {
                  throwFsError(js, err, "cp"_kj);
                }
                KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
                  KJ_IF_SOME(err, dir->add(js, relative.name, kj::mv(file))) {
                    throwFsError(js, err, opName);
                  }
                }
              }
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              if (options.copy) {
                node::THROW_ERR_UV_EISDIR(js, opName);
              }
              KJ_IF_SOME(err, dir->add(js, relative.name, dir.addRef())) {
                throwFsError(js, err, opName);
              }
            }
            KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
              KJ_IF_SOME(err, dir->add(js, relative.name, link.addRef())) {
                throwFsError(js, err, opName);
              }
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              throwFsError(js, err, opName);
            }
          }

          KJ_IF_SOME(dir, srcParent) {
            auto relative = srcUrl.getRelative();
            KJ_SWITCH_ONEOF(dir->remove(js, kj::Path({relative.name}), {.recursive = true})) {
              KJ_CASE_ONEOF(_, bool) {
                // ignore the specific return value.
                return;
              }
              KJ_CASE_ONEOF(err, workerd::FsError) {
                throwFsError(js, err, "rename"_kj);
              }
            }
            KJ_UNREACHABLE;
          }
        } else {
          node::THROW_ERR_UV_ENOENT(js, opName);
        }
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        node::THROW_ERR_UV_ENOTDIR(js, opName);
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        // If we got here, then the parent path was not found.
        throwFsError(js, err, opName);
      }
    }
  } else {
    node::THROW_ERR_UV_ENOENT(js, opName);
  }
}

jsg::Optional<kj::String> FileSystemModule::mkdir(
    jsg::Lock& js, FilePath path, MkdirOptions options) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  NormalizedFilePath normalizedPath(kj::mv(path));
  const jsg::Url& url = normalizedPath;

  // The path must not already exist. However, if the path is a directory, we
  // will just return rather than throwing an error.
  KJ_IF_SOME(node, vfs.resolve(js, url, {.followLinks = false})) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        node::THROW_ERR_UV_EEXIST(js, "mkdir"_kj);
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        // The directory already exists. We will just return.
        return kj::none;
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        node::THROW_ERR_UV_EEXIST(js, "mkdir"_kj);
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        throwFsError(js, err, "mkdir"_kj);
      }
    }
    KJ_UNREACHABLE;
  };

  if (options.recursive) {
    KJ_ASSERT(!options.tmp);
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
      bool moveToNext = false;
      // Try opening the next part of the path. Note that we are not using the
      // createAs option here because we don't necessarily want to implicitly
      // create the directory if it doesn't exist. We want to create it explicitly
      // so that we can return the path to the first directory that was created
      // and tryOpen does not us if the directory already existed or was created.
      KJ_IF_SOME(node, current->tryOpen(js, kj::Path({part}))) {
        // Let's make sure the node is a directory.
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            node::THROW_ERR_UV_ENOTDIR(js, "mkdir"_kj);
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            node::THROW_ERR_UV_ENOTDIR(js, "mkdir"_kj);
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            // The node is a directory, we can continue.
            current = kj::mv(dir);
            moveToNext = true;
          }
          KJ_CASE_ONEOF(err, workerd::FsError) {
            throwFsError(js, err, "mkdir"_kj);
          }
        }
      }
      if (moveToNext) continue;

      // The node does not exist, let's create it so long as the current
      // directory is writable.
      auto stat = current->stat(js);
      if (!stat.writable) {
        node::THROW_ERR_UV_EPERM(js, "mkdir"_kj);
      }
      auto dir = workerd::Directory::newWritable();
      KJ_IF_SOME(err, current->add(js, part, dir.addRef())) {
        throwFsError(js, err, "mkdir"_kj);
      }
      current = kj::mv(dir);
      if (createdPath == kj::none) {
        createdPath = currentPath.toString(true);
      }
    }

    // Now that we have the parent directory, let's try creating the new directory.
    auto newDir = workerd::Directory::newWritable();
    KJ_IF_SOME(err, current->add(js, name.toString(false), kj::mv(newDir))) {
      throwFsError(js, err, "mkdir"_kj);
    }

    return kj::mv(createdPath);
  }

  KJ_DASSERT(!options.recursive);
  // If the recursive option is not set, we will create the directory only if
  // the parent directory exists. If the parent directory does not exist, we
  // will return an error.
  auto relative = url.getRelative();
  KJ_IF_SOME(parent, vfs.resolve(js, relative.base)) {
    KJ_SWITCH_ONEOF(parent) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        node::THROW_ERR_UV_ENOTDIR(js, "mkdir"_kj);
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        auto stat = dir->stat(js);
        if (!stat.writable) {
          node::THROW_ERR_UV_EPERM(js, "mkdir"_kj);
        }
        auto newDir = workerd::Directory::newWritable();
        if (options.tmp) {
          if (tmpFileCounter >= kMax) {
            node::THROW_ERR_UV_EPERM(js, "mkdir"_kj, "Too many temporary directories created"_kj);
          }
          auto name = kj::str(relative.name, tmpFileCounter++);
          KJ_IF_SOME(err, dir->add(js, name, kj::mv(newDir))) {
            throwFsError(js, err, "mkdir"_kj);
          }
          KJ_IF_SOME(newUrl, relative.base.resolve(name)) {
            // If we are creating a temporary directory, we return the URL of the
            // new directory.
            return kj::str(newUrl.getPathname());
          } else {
            node::THROW_ERR_UV_EINVAL(js, "mkdir"_kj, "Invalid name for temporary directory"_kj);
          }
        }

        KJ_IF_SOME(err, dir->add(js, relative.name, kj::mv(newDir))) {
          throwFsError(js, err, "mkdir"_kj);
        }

        return kj::none;
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        node::THROW_ERR_UV_ENOTDIR(js, "mkdir"_kj);
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        throwFsError(js, err, "mkdir"_kj);
      }
    }
    KJ_UNREACHABLE;
  } else {
    node::THROW_ERR_UV_ENOENT(js, "mkdir"_kj);
  }
}

void FileSystemModule::rm(jsg::Lock& js, FilePath path, RmOptions options) {
  // TODO(node-fs): Implement the force option.
  auto& vfs = workerd::VirtualFileSystem::current(js);
  NormalizedFilePath normalizedPath(kj::mv(path));
  const jsg::Url& url = normalizedPath;
  auto relative = url.getRelative();

  KJ_IF_SOME(parent, vfs.resolve(js, relative.base)) {
    KJ_IF_SOME(dir, parent.tryGet<kj::Rc<workerd::Directory>>()) {
      auto stat = dir->stat(js);
      if (!stat.writable) {
        node::THROW_ERR_UV_EPERM(js, "rm"_kj);
      }

      kj::Path name(relative.name);

      if (options.dironly) {
        // If the dironly option is set, we will only remove the entry if it is a directory.
        KJ_IF_SOME(stat, dir->stat(js, name)) {
          KJ_SWITCH_ONEOF(stat) {
            KJ_CASE_ONEOF(stat, workerd::Stat) {
              if (stat.type != workerd::FsType::DIRECTORY) {
                node::THROW_ERR_UV_ENOTDIR(js, "rm"_kj);
              }
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              throwFsError(js, err, "rm"_kj);
            }
          }
        } else {
          node::THROW_ERR_UV_ENOENT(js, "rm"_kj);
        }
      }

      KJ_SWITCH_ONEOF(dir->remove(js, name, {.recursive = options.recursive})) {
        KJ_CASE_ONEOF(res, bool) {
          // Ignore the return.
        }
        KJ_CASE_ONEOF(err, workerd::FsError) {
          throwFsError(js, err, "rm"_kj);
        }
      }
    } else {
      node::THROW_ERR_UV_ENOTDIR(js, "rm"_kj);
    }
  } else {
    node::THROW_ERR_UV_ENOENT(js, "rm"_kj);
  }
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
          KJ_IF_SOME(err, guard.checkSeen(link.get())) {
            throwFsError(js, err, "readdir"_kj);
          }
          KJ_IF_SOME(target, link->resolve(js)) {
            KJ_SWITCH_ONEOF(target) {
              KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
                // Do nothing
              }
              KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
                readdirImpl(js, vfs, dir, path.append(entry.key), options, entries);
              }
              KJ_CASE_ONEOF(err, workerd::FsError) {
                throwFsError(js, err, "readdir"_kj);
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
  auto& vfs = workerd::VirtualFileSystem::current(js);
  NormalizedFilePath normalizedPath(kj::mv(path));

  KJ_IF_SOME(node, vfs.resolve(js, normalizedPath, {.followLinks = false})) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        kj::Vector<DirEntHandle> entries;
        readdirImpl(js, vfs, dir, normalizedPath, options, entries);
        return entries.releaseAsArray();
      }
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        node::THROW_ERR_UV_ENOTDIR(js, "readdir"_kj);
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        node::THROW_ERR_UV_EINVAL(js, "readdir"_kj);
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        throwFsError(js, err, "readdir"_kj);
      }
    }
    KJ_UNREACHABLE;
  } else {
    node::THROW_ERR_UV_ENOENT(js, "readdir"_kj);
  }
}

namespace {

using MaybeFsNode = kj::Maybe<
    kj::OneOf<kj::Rc<workerd::Directory>, kj::Rc<workerd::File>, kj::Rc<workerd::SymbolicLink>>>;

MaybeFsNode getNodeOrError(jsg::Lock& js,
    const workerd::VirtualFileSystem& vfs,
    const jsg::Url& url,
    const FileSystemModule::CpOptions& options) {
  KJ_IF_SOME(node, vfs.resolve(js, url, {.followLinks = options.deferenceSymlinks})) {
    KJ_SWITCH_ONEOF(node) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        return MaybeFsNode(kj::mv(file));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        return MaybeFsNode(kj::mv(dir));
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        return MaybeFsNode(kj::mv(link));
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        throwFsError(js, err, "cp"_kj);
      }
    }
  }
  return kj::none;
}

// Copy the src symbolic link to the destination URL location.
// We've already checked that the destination either does not exist
// or we are want to overwrite it. We need to next determine if the
// destination is writable. If it is not, this will throw an error.
// If it is, we will either create a new symbolic link at the destination
// or overwrite the existing file or link if it exists.
void handleCpLink(jsg::Lock& js,
    const workerd::VirtualFileSystem& vfs,
    kj::Rc<workerd::SymbolicLink> srcLink,
    const jsg::Url& destUrl) {
  // Here, we are going to essentially create a hard link (new refcount)
  // of srcLink and destUrl, if we are allowed to do so.
  // We need to check if the destination exists and is writable.
  auto relative = destUrl.getRelative();
  // relative.base is the parent directory path.
  // relative.name is the name of the link we are creating in the parent.

  auto basePath = kj::str(relative.base.getPathname().slice(1));
  kj::Path root{};
  auto base = root.eval(basePath);

  // We need to grab the parent directory, creating it if it does not exist
  // and we are permitted to do so.
  KJ_IF_SOME(destDir,
      vfs.getRoot(js)->tryOpen(js, base,
          {
            .createAs = workerd::FsType::DIRECTORY,
            .followLinks = true,
          })) {
    // Awesome, either the destination directory existed already or we
    // successfully created it, or an error was reported.
    KJ_SWITCH_ONEOF(destDir) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        // We cannot copy into a file, so we throw an error.
        node::THROW_ERR_UV_ENOTDIR(js, "cp"_kj);
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        kj::Path path({relative.name});
        // This is the case we're looking for!
        // First, let's check to see if the target name already exists.
        // If it does, we'll remove it.
        KJ_SWITCH_ONEOF(dir->remove(js, path, {.recursive = false})) {
          KJ_CASE_ONEOF(err, workerd::FsError) {
            throwFsError(js, err, "cp"_kj);
          }
          KJ_CASE_ONEOF(b, bool) {
            // Ignore the return value, we don't actually care if the thing existed or not.
          }
        }
        // Now, we can add the symbolic link to the directory.
        KJ_IF_SOME(err, dir->add(js, relative.name, kj::mv(srcLink))) {
          // If we got here, an error was reported
          throwFsError(js, err, "cp"_kj);
        }
        // If we got here, success!
        return;
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        // This shouldn't be possible since we told tryOpen to follow links.
        // But, let's just throw an error.
        node::THROW_ERR_UV_EINVAL(js, "cp"_kj);
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        throwFsError(js, err, "cp"_kj);
      }
    }
    KJ_UNREACHABLE;
  }

  // In this case, the destDir could not be opened, treat as an error.
  node::THROW_ERR_UV_EINVAL(js, "cp"_kj);
}

void handleCpFile(jsg::Lock& js,
    const workerd::VirtualFileSystem& vfs,
    kj::Rc<workerd::File> file,
    const jsg::Url& destUrl) {
  // Here, we are going to clone the file into a new file at the destination
  // if we are allowed to do so.
  // We need to check if the destination exists and is writable.
  auto relative = destUrl.getRelative();
  // relative.base is the parent directory path.
  // relative.name is the name of the link we are creating in the parent.

  auto basePath = kj::str(relative.base.getPathname().slice(1));
  kj::Path root{};
  auto base = root.eval(basePath);

  // We need to grab the parent directory, creating it if it does not exist
  // and we are permitted to do so.
  KJ_IF_SOME(destDir,
      vfs.getRoot(js)->tryOpen(js, base,
          {
            .createAs = workerd::FsType::DIRECTORY,
            .followLinks = true,
          })) {
    // Awesome, either the destination directory existed already or we
    // successfully created it, or an error was reported.
    KJ_SWITCH_ONEOF(destDir) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        // We cannot copy into a file, so we throw an error.
        node::THROW_ERR_UV_ENOTDIR(js, "cp"_kj);
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        kj::Path path({relative.name});
        // This is the case we're looking for!
        // First, let's check to see if the target name already exists.
        // If it does, we'll remove it.
        KJ_SWITCH_ONEOF(dir->remove(js, path, {.recursive = false})) {
          KJ_CASE_ONEOF(err, workerd::FsError) {
            throwFsError(js, err, "cp"_kj);
          }
          KJ_CASE_ONEOF(b, bool) {
            // Ignore the return value, we don't actually care if the thing existed or not.
          }
        }
        // Now, we can add the symbolic link to the directory.
        KJ_SWITCH_ONEOF(file->clone(js)) {
          KJ_CASE_ONEOF(err, workerd::FsError) {
            throwFsError(js, err, "cp"_kj);
          }
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            KJ_IF_SOME(err, dir->add(js, relative.name, kj::mv(file))) {
              // If we got here, an error was reported
              throwFsError(js, err, "cp"_kj);
            }
          }
        }
        // If we got here, success!
        return;
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        // This shouldn't be possible since we told tryOpen to follow links.
        // But, let's just throw an error.
        node::THROW_ERR_UV_EINVAL(js, "cp"_kj);
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        throwFsError(js, err, "cp"_kj);
      }
    }
    KJ_UNREACHABLE;
  }

  // In this case, the destDir could not be opened, treat as an error.
  node::THROW_ERR_UV_EINVAL(js, "cp"_kj);
}

void handleCpDir(jsg::Lock& js,
    const workerd::VirtualFileSystem& vfs,
    kj::Rc<workerd::Directory> src,
    kj::Rc<workerd::Directory> dest,
    const FileSystemModule::CpOptions& options) {
  auto stat = dest->stat(js);
  if (!stat.writable) {
    node::THROW_ERR_UV_EPERM(js, "cp"_kj, "Destination directory is not writable"_kj);
  }
  if (src.get() == dest.get()) {
    node::THROW_ERR_UV_EINVAL(js, "cp"_kj, "Source and destination directories are the same"_kj);
  }

  // Here, we iterate through each of the entries in the source directory,
  // recursively copying them to the destination directory.
  for (auto& entry: *src.get()) {
    kj::StringPtr name = entry.key;
    KJ_SWITCH_ONEOF(entry.value) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        // We have a file, we will copy it to the destination directory
        // unless errorOnExist is true, force is false, and the destination already exists.

        KJ_IF_SOME(existing,
            dest->tryOpen(js, kj::Path({name}),
                {
                  .followLinks = options.deferenceSymlinks,
                })) {
          // The destination path already exists. Check to see if we can overwrite it.
          KJ_SWITCH_ONEOF(existing) {
            KJ_CASE_ONEOF(existingFile, kj::Rc<workerd::File>) {
              if (existingFile.get() == file.get()) {
                // Do nothing
              } else if (options.force) {
                KJ_SWITCH_ONEOF(dest->remove(js, kj::Path({name}), {.recursive = false})) {
                  KJ_CASE_ONEOF(err, workerd::FsError) {
                    throwFsError(js, err, "cp"_kj);
                  }
                  KJ_CASE_ONEOF(b, bool) {
                    // Ignore the return value.
                  }
                }
                KJ_SWITCH_ONEOF(file->clone(js)) {
                  KJ_CASE_ONEOF(err, workerd::FsError) {
                    throwFsError(js, err, "cp"_kj);
                  }
                  KJ_CASE_ONEOF(cloned, kj::Rc<workerd::File>) {
                    KJ_IF_SOME(err, dest->add(js, name, kj::mv(cloned))) {
                      // If we got here, an error was reported
                      throwFsError(js, err, "cp"_kj);
                    }
                  }
                }
              } else if (options.errorOnExist) {
                node::THROW_ERR_UV_EEXIST(
                    js, "cp"_kj, kj::str("Destination already exists: ", name));
              }
              // If we got here, we are not overwriting the file, so we just ignore it.
            }
            KJ_CASE_ONEOF(existingDir, kj::Rc<workerd::Directory>) {
              // We cannot overwrite a directory with a file, so we throw an error.
              node::THROW_ERR_UV_EISDIR(
                  js, "cp"_kj, kj::str("Cannot copy file to directory: ", name));
            }
            KJ_CASE_ONEOF(_, kj::Rc<workerd::SymbolicLink>) {
              // We're going to replace the existing link with the file.
              if (options.force) {
                KJ_SWITCH_ONEOF(dest->remove(js, kj::Path({name}), {.recursive = false})) {
                  KJ_CASE_ONEOF(err, workerd::FsError) {
                    throwFsError(js, err, "cp"_kj);
                  }
                  KJ_CASE_ONEOF(b, bool) {
                    // Ignore the return value.
                  }
                }
                KJ_SWITCH_ONEOF(file->clone(js)) {
                  KJ_CASE_ONEOF(err, workerd::FsError) {
                    throwFsError(js, err, "cp"_kj);
                  }
                  KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
                    KJ_IF_SOME(err, dest->add(js, name, kj::mv(file))) {
                      // If we got here, an error was reported
                      throwFsError(js, err, "cp"_kj);
                    }
                  }
                }
              } else if (options.errorOnExist) {
                node::THROW_ERR_UV_EEXIST(
                    js, "cp"_kj, kj::str("Destination already exists: ", name));
              }
              // If we got here, we are not overwriting the file, so we just ignore it.
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              throwFsError(js, err, "cp"_kj);
            }
          }
        } else {
          KJ_SWITCH_ONEOF(file->clone(js)) {
            KJ_CASE_ONEOF(err, workerd::FsError) {
              throwFsError(js, err, "cp"_kj);
            }
            KJ_CASE_ONEOF(cloned, kj::Rc<workerd::File>) {
              KJ_IF_SOME(err, dest->add(js, name, kj::mv(cloned))) {
                // If we got here, an error was reported
                throwFsError(js, err, "cp"_kj);
              }
            }
          }
        }
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        // We have a directory, we will copy it to the destination directory
        // recursively.

        // First, we need to check if the destination directory already exists.
        KJ_IF_SOME(existing,
            dest->tryOpen(js, kj::Path({name}),
                {
                  .followLinks = options.deferenceSymlinks,
                })) {
          // The destination exists. Check to see if we can overwrite it.
          KJ_SWITCH_ONEOF(existing) {
            KJ_CASE_ONEOF(existingFile, kj::Rc<workerd::File>) {
              // The destination is a file, we cannot overwrite it with a directory.
              node::THROW_ERR_UV_ENOTDIR(
                  js, "cp"_kj, kj::str("Cannot copy directory to file: ", name));
            }
            KJ_CASE_ONEOF(existingDir, kj::Rc<workerd::Directory>) {
              handleCpDir(js, vfs, kj::mv(dir), kj::mv(existingDir), options);
            }
            KJ_CASE_ONEOF(existingLink, kj::Rc<workerd::SymbolicLink>) {
              // The destination is a symbolic link, we can overwrite it with a directory.
              node::THROW_ERR_UV_EISDIR(
                  js, "cp"_kj, kj::str("Cannot copy directory to symbolic link: ", name));
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              throwFsError(js, err, "cp"_kj);
            }
          }
        } else {
          // The destination does not exist, we'll need to create a new directory.
          // then recursively copy into it.
          auto newDir = workerd::Directory::newWritable();
          KJ_IF_SOME(err, dest->add(js, name, newDir.addRef())) {
            // If we got here, an error was reported
            throwFsError(js, err, "cp"_kj);
          }
          // Now we can recursively copy the contents of the source directory into the new one.
          handleCpDir(js, vfs, kj::mv(dir), kj::mv(newDir), options);
        }
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        KJ_IF_SOME(existing,
            dest->tryOpen(js, kj::Path({name}),
                {
                  .followLinks = options.deferenceSymlinks,
                })) {
          // The destination path already exists. Check to see if we can overwrite it.
          KJ_SWITCH_ONEOF(existing) {
            KJ_CASE_ONEOF(_, kj::Rc<workerd::File>) {
              if (options.force) {
                KJ_SWITCH_ONEOF(dest->remove(js, kj::Path({name}), {.recursive = false})) {
                  KJ_CASE_ONEOF(err, workerd::FsError) {
                    throwFsError(js, err, "cp"_kj);
                  }
                  KJ_CASE_ONEOF(b, bool) {
                    // Ignore the return value.
                  }
                }
                KJ_IF_SOME(err, dest->add(js, name, link.addRef())) {
                  // If we got here, an error was reported
                  throwFsError(js, err, "cp"_kj);
                }
              } else if (options.errorOnExist) {
                node::THROW_ERR_UV_EEXIST(
                    js, "cp"_kj, kj::str("Destination already exists: ", name));
              }
              // If we got here, we are not overwriting the file, so we just ignore it.
            }
            KJ_CASE_ONEOF(existingDir, kj::Rc<workerd::Directory>) {
              // We cannot overwrite a directory with a file, so we throw an error.
              node::THROW_ERR_UV_EISDIR(
                  js, "cp"_kj, kj::str("Cannot copy link to directory: ", name));
            }
            KJ_CASE_ONEOF(existingLink, kj::Rc<workerd::SymbolicLink>) {
              if (existingLink.get() == link.get()) {
                // Do nothing
              } else if (options.force) {
                KJ_SWITCH_ONEOF(dest->remove(js, kj::Path({name}), {.recursive = false})) {
                  KJ_CASE_ONEOF(err, workerd::FsError) {
                    throwFsError(js, err, "cp"_kj);
                  }
                  KJ_CASE_ONEOF(b, bool) {
                    // Ignore the return value.
                  }
                }
                KJ_IF_SOME(err, dest->add(js, name, link.addRef())) {
                  // If we got here, an error was reported
                  throwFsError(js, err, "cp"_kj);
                }
              } else if (options.errorOnExist) {
                node::THROW_ERR_UV_EEXIST(
                    js, "cp"_kj, kj::str("Destination already exists: ", name));
              }
              // If we got here, we are not overwriting the file, so we just ignore it.
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              throwFsError(js, err, "cp"_kj);
            }
          }
        } else KJ_IF_SOME(err, dest->add(js, name, link.addRef())) {
          // If we got here, an error was reported
          throwFsError(js, err, "cp"_kj);
        }
      }
    }
  }
}

void handleCpDir(jsg::Lock& js,
    const workerd::VirtualFileSystem& vfs,
    kj::Rc<workerd::Directory> src,
    const jsg::Url& dest,
    const FileSystemModule::CpOptions& options) {
  // For this variation of handleCpDir, the dest needs to be created as a directory.
  // The assumption here is that the destination does not yet exist. Let's create it.

  auto basePath = kj::str(dest.getPathname().slice(1));
  kj::Path root{};
  auto path = root.eval(basePath);

  KJ_IF_SOME(destDir,
      vfs.getRoot(js)->tryOpen(js, path,
          {
            .createAs = workerd::FsType::DIRECTORY,
            .followLinks = true,
          })) {
    KJ_SWITCH_ONEOF(destDir) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        // We cannot copy into a file, so we throw an error.
        node::THROW_ERR_UV_ENOTDIR(js, "cp"_kj);
      }
      KJ_CASE_ONEOF(destination, kj::Rc<workerd::Directory>) {
        // Nice... we have our destination directory. Continue to copy the contents.
        return handleCpDir(js, vfs, kj::mv(src), kj::mv(destination), options);
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        // This shouldn't be possible since we told tryOpen to follow links.
        // But, let's just throw an error.
        node::THROW_ERR_UV_EINVAL(js, "cp"_kj);
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        throwFsError(js, err, "cp"_kj);
      }
    }
    KJ_UNREACHABLE;
  }

  // If we got here, then for some reason we could not open/create the destination
  // directory. Since we passed createAs, we shouldn't really be able to get here.
  node::THROW_ERR_UV_EINVAL(js, "cp"_kj);
}

void cpImpl(jsg::Lock& js,
    const workerd::VirtualFileSystem& vfs,
    const jsg::Url& src,
    const jsg::Url& dest,
    const FileSystemModule::CpOptions& options) {

  // Cannot copy a file to itself.
  JSG_REQUIRE(!src.equal(dest,
                  jsg::Url::EquivalenceOption::IGNORE_FRAGMENTS |
                      jsg::Url::EquivalenceOption::IGNORE_SEARCH |
                      jsg::Url::EquivalenceOption::NORMALIZE_PATH),
      Error, "Source and destination paths must not be the same"_kj);

  // Step 1: If deferenceSymlinks is true, the we will be following symbolic links. If
  // it is false, we won't be.

  auto maybeSrcNode = getNodeOrError(js, vfs, src, options);
  auto maybeDestNode = getNodeOrError(js, vfs, dest, options);

  KJ_IF_SOME(sourceNode, maybeSrcNode) {
    KJ_SWITCH_ONEOF(sourceNode) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        KJ_IF_SOME(node, maybeDestNode) {
          KJ_SWITCH_ONEOF(node) {
            KJ_CASE_ONEOF(_, kj::Rc<workerd::File>) {
              // If options.force is true, we will overwrite the destination file.
              if (options.force) {
                return handleCpFile(js, vfs, kj::mv(file), dest);
              }
              // Otherwise, if options.errorOnExist is true, we will throw an error.
              if (options.errorOnExist) {
                node::THROW_ERR_FS_CP_EEXIST(js);
              }
              // Otherwise, we skip this file and do nothing.
              return;
            }
            KJ_CASE_ONEOF(_, kj::Rc<workerd::Directory>) {
              // Simple case: user is trying to copy a file over a directory
              // which is not allowed.
              node::THROW_ERR_FS_CP_NON_DIR_TO_DIR(js);
            }
            KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
              if (options.deferenceSymlinks) {
                KJ_IF_SOME(target, link->resolve(js)) {
                  KJ_SWITCH_ONEOF(target) {
                    KJ_CASE_ONEOF(targetFile, kj::Rc<workerd::File>) {
                      KJ_SWITCH_ONEOF(file->clone(js)) {
                        KJ_CASE_ONEOF(err, workerd::FsError) {
                          throwFsError(js, err, "cp"_kj);
                        }
                        KJ_CASE_ONEOF(clonedFile, kj::Rc<workerd::File>) {
                          KJ_IF_SOME(err, targetFile->replace(js, kj::mv(clonedFile))) {
                            throwFsError(js, err, "cp"_kj);
                          }
                        }
                      }
                      return;
                    }
                    KJ_CASE_ONEOF(_, kj::Rc<workerd::Directory>) {
                      node::THROW_ERR_UV_EISDIR(js, "cp"_kj);
                    }
                    KJ_CASE_ONEOF(err, workerd::FsError) {
                      throwFsError(js, err, "cp"_kj);
                    }
                  }
                }
                node::THROW_ERR_UV_ENOENT(js, "cp"_kj);
              }

              // We would only get here if deferenceSymlinks is false.
              // In this case, if errorOnExist is true and force is false,
              // we will throw an error.
              if (options.force) {
                // Copy the file contents to the destination, replacing
                // the symbolic link with a copy of the file.
                return handleCpFile(js, vfs, kj::mv(file), dest);
              }
              if (options.errorOnExist) {
                node::THROW_ERR_FS_CP_EEXIST(js);
              }

              // Otherwise, we skip this file and do nothing.
              return;
            }
          }
          KJ_UNREACHABLE;
        }

        // Yay! we can just copy the file contents to the destination.
        // If the path to the destination does not exist, we will create it
        // if possible.
        return handleCpFile(js, vfs, kj::mv(file), dest);
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        // The source is a directory. The options.recursive option must be set
        // to true or we will fail.
        if (!options.recursive) {
          node::THROW_ERR_FS_EISDIR(js);
        }

        KJ_IF_SOME(dest, maybeDestNode) {
          KJ_SWITCH_ONEOF(dest) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              // Simple case: user is trying to copy a directory over a file
              // which is not allowed.
              node::THROW_ERR_FS_CP_DIR_TO_NON_DIR(js);
            }
            KJ_CASE_ONEOF(destDir, kj::Rc<workerd::Directory>) {
              // So Node.js has a bit of an inconsistency here when copying directories.
              // When copying a file over a file, we will check the errorOnExist and force
              // options, failing if the destination file exists and errorOnExist is true,
              // unless the force option is set. If both are false, we skip the copy.
              // However, the same logic is not applied to copying a directory. If the
              // destination directory exists, we will still proceed to copy the source
              // directory into the destination directory, only applying the force and
              // errorOnExist options to individual files within the directories.
              // See: https://github.com/nodejs/node/issues/58947
              return handleCpDir(js, vfs, kj::mv(dir), kj::mv(destDir), options);
            }
            KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
              // Also a simple case, user is trying to copy a directory over
              // an existing symbolic link, which we do not allow.
              node::THROW_ERR_UV_ENOTDIR(js, "cp"_kj);
            }
          }
          KJ_UNREACHABLE;
        }

        // Yay! we can just copy the file contents to the destination.
        // If the path to the destination does not exist, we will create it
        // if possible.
        return handleCpDir(js, vfs, kj::mv(dir), dest, options);
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        // If we got here, then the source is itself a symbolic link.
        // The destination, if we do copy it, will also be a symbolic link to
        // the same target. The options.errorOnExist and options.force still
        // apply here, but we will not follow the symbolic link at all.
        KJ_IF_SOME(node, maybeDestNode) {
          KJ_SWITCH_ONEOF(node) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              if (options.force) {
                return handleCpLink(js, vfs, kj::mv(link), dest);
              }

              if (options.errorOnExist) {
                node::THROW_ERR_FS_CP_EEXIST(js);
              }

              // Otherwise we skip this file and do nothing.
              return;
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              // Simple case: user is trying to copy a symbolic link over a directory
              // which is not allowed.
              node::THROW_ERR_UV_ENOTDIR(js, "cp"_kj);
            }
            KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
              if (options.force) {
                return handleCpLink(js, vfs, kj::mv(link), dest);
              }

              if (options.errorOnExist) {
                node::THROW_ERR_FS_CP_EEXIST(js);
              }

              // Otherwise we skip this file and do nothing.
              return;
            }
          }
          KJ_UNREACHABLE;
        }

        // Yay! we can just copy fhe symbolic link to the destination.
        // If the path to the destination does not exist, we will create it
        // if possible.
        return handleCpLink(js, vfs, kj::mv(link), dest);
      }
    }
    KJ_UNREACHABLE;
  }

  // If we got here, the sourceNode does not exist.
  node::THROW_ERR_UV_ENOENT(js, "cp"_kj);
}
}  // namespace

void FileSystemModule::cp(jsg::Lock& js, FilePath src, FilePath dest, CpOptions options) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  NormalizedFilePath normalizedSrc(kj::mv(src));
  NormalizedFilePath normalizedDest(kj::mv(dest));
  // TODO(node-fs): Support the preserveTimestamps option.
  cpImpl(js, vfs, normalizedSrc, normalizedDest, options);
}

// =======================================================================================

jsg::Ref<FileFdHandle> FileFdHandle::constructor(jsg::Lock& js, int fd) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  return js.alloc<FileFdHandle>(js, vfs, fd);
}

FileFdHandle::FileFdHandle(jsg::Lock& js, const workerd::VirtualFileSystem& vfs, int fd)
    : fdHandle(vfs.wrapFd(js, fd)) {}

void FileFdHandle::close(jsg::Lock& js) {
  fdHandle = kj::none;
}

FileFdHandle::~FileFdHandle() noexcept {
  if (fdHandle != kj::none) {
    // We can safely close the file descriptor without an explicit lock
    // because we are in the destructor of a jsg::Object which should
    // only be destroyed when the isolate lock is held per the rules of
    // the jsg::Ref<T> holder and the deferred destruction queue.
    fdHandle = kj::none;

    // In Node.js, closing the file descriptor on destruction is an
    // error (it has been a deprecated behavior for a long time and
    // is being upgraded to a catchable error in Node.js moving
    // forward). However, throwing an error in our implementation is
    // of questionable value since it's not clear exactly what the
    // user is supposed to do about it beyond making sure to explicitly
    // close the file descriptor before the object is destroyed.
    // If we have an active IoContext, then we'll go ahead and log
    // a warning.
    // In preview, let's try to warn the developer about the problem.
    if (IoContext::hasCurrent()) {
      IoContext::current().logWarning(
          kj::str("A FileHandle was destroyed without being closed. This is "
                  "not recommended and may lead to file descriptors being held "
                  "far longer than necessary. Please make sure to explicitly close "
                  "the FileHandle object explicitly before it is destroyed."));
    }
  }
}

// =======================================================================================
// Implementation of the Web File System API

namespace {
constexpr bool isValidFileName(kj::StringPtr name) {
  return name.size() > 0 && name != "."_kj && name != ".."_kj && name.find("/"_kj) == kj::none &&
      name.find("\\"_kj) == kj::none;
}

jsg::Ref<jsg::DOMException> fsErrorToDomException(jsg::Lock& js, workerd::FsError error) {
  switch (error) {
    case workerd::FsError::NOT_DIRECTORY: {
      return js.domException(kj::str("NotSupportedError"), kj::str("Not a directory"));
    }
    case workerd::FsError::NOT_EMPTY: {
      return js.domException(kj::str("InvalidModificationError"), kj::str("Directory not empty"));
    }
    case workerd::FsError::READ_ONLY: {
      return js.domException(kj::str("InvalidStateError"), kj::str("Read-only file system"));
    }
    case workerd::FsError::NOT_PERMITTED: {
      return js.domException(kj::str("NotAllowedError"), kj::str("Operation not permitted"));
    }
    case workerd::FsError::NOT_PERMITTED_ON_DIRECTORY: {
      return js.domException(
          kj::str("NotAllowedError"), kj::str("Operation not permitted on a directory"));
    }
    case workerd::FsError::ALREADY_EXISTS: {
      return js.domException(kj::str("InvalidStateError"), kj::str("File already exists"));
    }
    case workerd::FsError::TOO_MANY_OPEN_FILES: {
      return js.domException(kj::str("QuotaExceededError"),
          kj::str("Too many open files, please close some files and try again"));
    }
    case workerd::FsError::FAILED: {
      return js.domException(kj::str("UnknownError"), kj::str("File system operation failed"));
    }
    case workerd::FsError::NOT_SUPPORTED: {
      return js.domException(kj::str("NotSupportedError"), kj::str("Operation not supported"));
    }
    case workerd::FsError::INVALID_PATH: {
      return js.domException(kj::str("TypeMismatchError"), kj::str("Invalid file path"));
    }
    case workerd::FsError::FILE_SIZE_LIMIT_EXCEEDED: {
      return js.domException(kj::str("QuotaExceededError"),
          kj::str("File size limit exceeded, please reduce the file size and try again"));
    }
    case workerd::FsError::SYMLINK_DEPTH_EXCEEDED: {
      return js.domException(kj::str("InvalidStateError"),
          kj::str("Symbolic link depth exceeded, please check the symbolic links"));
    }
    default: {
      return js.domException(
          kj::str("UnknownError"), kj::str("Unknown file system error: ", static_cast<int>(error)));
    }
  }
  KJ_UNREACHABLE;
}
}  // namespace

FileSystemHandle::FileSystemHandle(
    const workerd::VirtualFileSystem& vfs, jsg::Url&& locator, jsg::USVString name)
    : vfs(vfs),
      locator(kj::mv(locator)),
      name(kj::mv(name)) {}

jsg::Promise<kj::StringPtr> FileSystemHandle::getUniqueId(
    jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler) {
  KJ_IF_SOME(item, vfs.resolve(js, getLocator(), {})) {
    KJ_SWITCH_ONEOF(item) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        return js.resolvedPromise(file->getUniqueId(js));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        return js.resolvedPromise(dir->getUniqueId(js));
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        return js.resolvedPromise(link->getUniqueId(js));
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        return js.rejectedPromise<kj::StringPtr>(
            deHandler.wrap(js, fsErrorToDomException(js, err)));
      }
    }
    KJ_UNREACHABLE;
  }
  auto ex = js.domException(kj::str("NotFoundError"), kj::str("The entry was not found."));
  return js.rejectedPromise<kj::StringPtr>(deHandler.wrap(js, kj::mv(ex)));
}

jsg::Promise<bool> FileSystemHandle::isSameEntry(jsg::Lock& js, jsg::Ref<FileSystemHandle> other) {
  // Per the spec, two handles are the same if they refer to the same entry (that is,
  // have the same locator). It does not matter if they are different actual entries.
  return getKind(js) == other->getKind(js) &&
          locator.equal(other->getLocator(),
              jsg::Url::EquivalenceOption::IGNORE_FRAGMENTS |
                  jsg::Url::EquivalenceOption::IGNORE_SEARCH |
                  jsg::Url::EquivalenceOption::NORMALIZE_PATH)
      ? js.resolvedPromise(true)
      : js.resolvedPromise(false);
}

jsg::Promise<void> FileSystemHandle::remove(jsg::Lock& js,
    jsg::Optional<RemoveOptions> options,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler) {

  if (!canBeModifiedCurrently(js)) {
    auto ex = js.domException(kj::str("NoModificationAllowedError"),
        kj::str("Cannot remove a handle that is not writable or not a directory."));
    return js.rejectedPromise<void>(deHandler.wrap(js, kj::mv(ex)));
  }

  auto relative = getLocator().getRelative(jsg::Url::RelativeOption::STRIP_TAILING_SLASHES);
  auto opts = options.orDefault(RemoveOptions{});
  auto recursive = opts.recursive.orDefault(false);
  KJ_IF_SOME(parent, vfs.resolve(js, relative.base, workerd::VirtualFileSystem::ResolveOptions{})) {
    KJ_SWITCH_ONEOF(parent) {
      KJ_CASE_ONEOF(parentDir, kj::Rc<workerd::Directory>) {
        // Webfs requires that the entry exists before we try to remove it.
        kj::Path path({name});
        if (parentDir->stat(js, path) == kj::none) {
          auto ex = js.domException(kj::str("NotFoundError"), kj::str("The entry was not found."));
          return js.rejectedPromise<void>(deHandler.wrap(js, kj::mv(ex)));
        }

        KJ_SWITCH_ONEOF(parentDir->remove(js, path, {.recursive = recursive})) {
          KJ_CASE_ONEOF(err, workerd::FsError) {
            return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
          }
          KJ_CASE_ONEOF(removed, bool) {
            if (!removed) {
              auto ex =
                  js.domException(kj::str("NotFoundError"), kj::str("The entry was not found."));
              return js.rejectedPromise<void>(deHandler.wrap(js, kj::mv(ex)));
            }
            return js.resolvedPromise();
          }
        }
      }
      KJ_CASE_ONEOF(_, kj::Rc<workerd::File>) {
        return js.rejectedPromise<void>(
            deHandler.wrap(js, fsErrorToDomException(js, workerd::FsError::NOT_DIRECTORY)));
      }
      KJ_CASE_ONEOF(_, kj::Rc<workerd::SymbolicLink>) {
        return js.rejectedPromise<void>(
            deHandler.wrap(js, fsErrorToDomException(js, workerd::FsError::NOT_DIRECTORY)));
      }
      KJ_CASE_ONEOF(err, workerd::FsError) {
        return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
      }
    }
    KJ_UNREACHABLE;
  }
  auto ex = js.domException(kj::str("NotFoundError"), kj::str("The entry was not found."));
  return js.rejectedPromise<void>(deHandler.wrap(js, kj::mv(ex)));
}

bool FileSystemHandle::canBeModifiedCurrently(jsg::Lock& js) const {
  auto pathname = getLocator().getPathname();
  if (pathname.endsWith("/"_kj)) {
    auto cloned = getLocator().clone();
    cloned.setPathname(pathname.slice(0, pathname.size() - 1));
    return !getVfs().isLocked(js, cloned);
  }
  return !getVfs().isLocked(js, getLocator());
}

jsg::Promise<jsg::Ref<FileSystemDirectoryHandle>> StorageManager::getDirectory(
    jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception) {
  auto& vfs = workerd::VirtualFileSystem::current(js);
  return js.resolvedPromise(js.alloc<FileSystemDirectoryHandle>(
      vfs, KJ_ASSERT_NONNULL(jsg::Url::tryParse("file:///"_kj)), jsg::USVString()));
}

FileSystemDirectoryHandle::FileSystemDirectoryHandle(
    const workerd::VirtualFileSystem& vfs, jsg::Url locator, jsg::USVString name)
    : FileSystemHandle(vfs, kj::mv(locator), kj::mv(name)) {}

jsg::Promise<jsg::Ref<FileSystemFileHandle>> FileSystemDirectoryHandle::getFileHandle(jsg::Lock& js,
    jsg::USVString name,
    jsg::Optional<FileSystemGetFileOptions> options,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception) {
  if (!isValidFileName(name)) {
    return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(js.typeError("Invalid file name"));
  }
  kj::Maybe<FsType> createAs;
  KJ_IF_SOME(opts, options) {
    if (opts.create) createAs = FsType::FILE;
  }

  KJ_IF_SOME(existing, getVfs().resolve(js, getLocator(), {})) {
    KJ_SWITCH_ONEOF(existing) {
      KJ_CASE_ONEOF(err, workerd::FsError) {
        return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(
            exception.wrap(js, fsErrorToDomException(js, err)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        auto locator = KJ_ASSERT_NONNULL(getLocator().tryResolve(name));
        auto relative = locator.getRelative();
        KJ_IF_SOME(node,
            dir->tryOpen(js, kj::Path({relative.name}),
                Directory::OpenOptions{
                  .createAs = createAs,
                })) {
          KJ_SWITCH_ONEOF(node) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              return js.resolvedPromise(
                  js.alloc<FileSystemFileHandle>(getVfs(), kj::mv(locator), kj::mv(name)));
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              auto ex = js.domException(
                  kj::str("TypeMismatchError"), kj::str("File name is a directory"));
              return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(
                  exception.wrap(js, kj::mv(ex)));
            }
            KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
              auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Not a file"));
              return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(
                  exception.wrap(js, kj::mv(ex)));
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(
                  exception.wrap(js, fsErrorToDomException(js, err)));
            }
          }
          KJ_UNREACHABLE;
        }

        auto ex = js.domException(kj::str("NotFoundError"), kj::str("Not found"));
        return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(exception.wrap(js, kj::mv(ex)));
      }
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Not a directory"));
        return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(exception.wrap(js, kj::mv(ex)));
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Not a directory"));
        return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(exception.wrap(js, kj::mv(ex)));
      }
    }
    KJ_UNREACHABLE;
  }

  auto ex = js.domException(kj::str("NotFoundError"), kj::str("Directory not found"));
  return js.rejectedPromise<jsg::Ref<FileSystemFileHandle>>(exception.wrap(js, kj::mv(ex)));
}

jsg::Promise<jsg::Ref<FileSystemDirectoryHandle>> FileSystemDirectoryHandle::getDirectoryHandle(
    jsg::Lock& js,
    jsg::USVString name,
    jsg::Optional<FileSystemGetDirectoryOptions> options,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception) {
  if (!isValidFileName(name)) {
    return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(
        js.typeError("Invalid directory name"));
  }

  kj::Maybe<FsType> createAs;
  KJ_IF_SOME(opts, options) {
    if (opts.create) createAs = FsType::DIRECTORY;
  }

  KJ_IF_SOME(existing, getVfs().resolve(js, getLocator(), {})) {
    KJ_SWITCH_ONEOF(existing) {
      KJ_CASE_ONEOF(err, workerd::FsError) {
        return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(
            exception.wrap(js, fsErrorToDomException(js, err)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        auto locator = KJ_ASSERT_NONNULL(getLocator().tryResolve(name));
        auto relative = locator.getRelative();
        KJ_IF_SOME(node,
            dir->tryOpen(js, kj::Path({relative.name}),
                Directory::OpenOptions{
                  .createAs = createAs,
                })) {
          KJ_SWITCH_ONEOF(node) {
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              return js.resolvedPromise(js.alloc<FileSystemDirectoryHandle>(getVfs(),
                  KJ_ASSERT_NONNULL(locator.resolve(kj::str(locator.getPathname(), "/"))),
                  kj::mv(name)));
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::File>) {
              auto ex =
                  js.domException(kj::str("TypeMismatchError"), kj::str("File name is a file"));
              return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(
                  exception.wrap(js, kj::mv(ex)));
            }
            KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
              auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Not a directory"));
              return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(
                  exception.wrap(js, kj::mv(ex)));
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(
                  exception.wrap(js, fsErrorToDomException(js, err)));
            }
          }
          KJ_UNREACHABLE;
        }
        // Could not open or create the directory.
        auto ex =
            js.domException(kj::str("NotFoundError"), kj::str("Directory not opened or created"));
        return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(
            exception.wrap(js, kj::mv(ex)));
      }
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Not a directory"));
        return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(
            exception.wrap(js, kj::mv(ex)));
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Not a directory"));
        return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(
            exception.wrap(js, kj::mv(ex)));
      }
    }
    KJ_UNREACHABLE;
  }

  auto ex = js.domException(kj::str("NotFoundError"), kj::str("Directory not found"));
  return js.rejectedPromise<jsg::Ref<FileSystemDirectoryHandle>>(exception.wrap(js, kj::mv(ex)));
}

jsg::Promise<void> FileSystemDirectoryHandle::removeEntry(jsg::Lock& js,
    jsg::USVString name,
    jsg::Optional<FileSystemRemoveOptions> options,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception) {
  if (!isValidFileName(name)) {
    return js.rejectedPromise<void>(js.typeError("Invalid name"));
  }
  auto opts = options.orDefault(FileSystemRemoveOptions{});

  KJ_IF_SOME(existing, getVfs().resolve(js, getLocator(), {})) {
    KJ_SWITCH_ONEOF(existing) {
      KJ_CASE_ONEOF(err, workerd::FsError) {
        return js.rejectedPromise<void>(exception.wrap(js, fsErrorToDomException(js, err)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        kj::Path item({name});
        auto fileLocator = KJ_ASSERT_NONNULL(getLocator().tryResolve(name));
        if (getVfs().isLocked(js, fileLocator)) {
          // If the file is locked, we cannot remove it.
          auto ex = js.domException(kj::str("NoModificationAllowedError"),
              kj::str("Cannot remove an entry that is currently locked."));
          return js.rejectedPromise<void>(exception.wrap(js, kj::mv(ex)));
        }

        KJ_SWITCH_ONEOF(dir->remove(js, item,
                            workerd::Directory::RemoveOptions{
                              .recursive = opts.recursive,
                            })) {
          KJ_CASE_ONEOF(res, bool) {
            if (res) {
              return js.resolvedPromise();
            }
            // If the entry was not found, we throw a NotFoundError.
            auto ex = js.domException(kj::str("NotFoundError"), kj::str("File not found"));
            return js.rejectedPromise<void>(exception.wrap(js, kj::mv(ex)));
          }
          KJ_CASE_ONEOF(error, workerd::FsError) {
            return js.rejectedPromise<void>(exception.wrap(js, fsErrorToDomException(js, error)));
          }
        }
        KJ_UNREACHABLE;
      }
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Not a directory"));
        return js.rejectedPromise<void>(exception.wrap(js, kj::mv(ex)));
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Not a directory"));
        return js.rejectedPromise<void>(exception.wrap(js, kj::mv(ex)));
      }
    }
    KJ_UNREACHABLE;
  }

  auto ex = js.domException(kj::str("NotFoundError"), kj::str("Not found"));
  return js.rejectedPromise<void>(exception.wrap(js, kj::mv(ex)));
}

jsg::Promise<kj::Array<jsg::USVString>> FileSystemDirectoryHandle::resolve(
    jsg::Lock& js, jsg::Ref<FileSystemHandle> possibleDescendant) {
  JSG_FAIL_REQUIRE(Error, "Not implemented");
}

namespace {
kj::Array<jsg::Ref<FileSystemHandle>> collectEntries(const workerd::VirtualFileSystem& vfs,
    jsg::Lock& js,
    kj::Rc<workerd::Directory>& inner,
    const jsg::Url& parentLocator) {
  kj::Vector<jsg::Ref<FileSystemHandle>> entries;
  for (auto& entry: *inner.get()) {
    KJ_SWITCH_ONEOF(entry.value) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        auto locator = KJ_ASSERT_NONNULL(parentLocator.tryResolve(entry.key));
        entries.add(
            js.alloc<FileSystemFileHandle>(vfs, kj::mv(locator), js.accountedUSVString(entry.key)));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        auto locator = KJ_ASSERT_NONNULL(parentLocator.tryResolve(kj::str(entry.key, "/")));
        entries.add(js.alloc<FileSystemDirectoryHandle>(
            vfs, kj::mv(locator), js.accountedUSVString(entry.key)));
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        SymbolicLinkRecursionGuardScope guardScope;
        KJ_IF_SOME(_, guardScope.checkSeen(link.get())) {
          // Throw a DOMException indicating that the symbolic link is recursive.
          JSG_FAIL_REQUIRE(DOMOperationError, "Symbolic link recursion detected"_kj);
        }
        KJ_IF_SOME(res, link->resolve(js)) {
          KJ_SWITCH_ONEOF(res) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              auto locator = KJ_ASSERT_NONNULL(parentLocator.tryResolve(entry.key));
              entries.add(js.alloc<FileSystemFileHandle>(
                  vfs, kj::mv(locator), js.accountedUSVString(entry.key)));
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              auto locator = KJ_ASSERT_NONNULL(parentLocator.tryResolve(kj::str(entry.key, "/")));
              entries.add(js.alloc<FileSystemDirectoryHandle>(
                  vfs, kj::mv(locator), js.accountedUSVString(entry.key)));
            }
            KJ_CASE_ONEOF(_, workerd::FsError) {
              JSG_FAIL_REQUIRE(DOMOperationError, "Symbolic link recursion detected"_kj);
            }
          }
        }
      }
    }
  }
  return entries.releaseAsArray();
}

auto resolveDirectoryHandle(jsg::Lock& js, const VirtualFileSystem& vfs, const jsg::Url& locator) {
  auto pathname = locator.getPathname();
  if (pathname.endsWith("/"_kj)) {
    pathname = pathname.first(pathname.size() - 1);
    auto cloned = locator.clone();
    cloned.setPathname(pathname);
    return vfs.resolve(js, cloned, {});
  }
  // Otherwise fall-back to the original locator.
  return vfs.resolve(js, locator, {});
}
}  // namespace

jsg::Ref<FileSystemDirectoryHandle::EntryIterator> FileSystemDirectoryHandle::entries(
    jsg::Lock& js) {
  KJ_IF_SOME(existing, resolveDirectoryHandle(js, getVfs(), getLocator())) {
    KJ_SWITCH_ONEOF(existing) {
      KJ_CASE_ONEOF(err, workerd::FsError) {
        JSG_FAIL_REQUIRE(DOMOperationError, "Failed to read directory: ", static_cast<int>(err));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        return js.alloc<EntryIterator>(
            IteratorState(JSG_THIS, collectEntries(getVfs(), js, dir, getLocator())));
      }
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        JSG_FAIL_REQUIRE(DOMTypeMismatchError, "Not a directory");
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        JSG_FAIL_REQUIRE(DOMTypeMismatchError, "Not a directory");
      }
    }
    KJ_UNREACHABLE;
  }

  // The directory was not found. However, for some weird reason the spec requires that
  // we still return an iterator here but it needs to throw a NotFoundError when next
  // is actually called.
  auto ex = js.domException(kj::str("NotFoundError"), kj::str("Not found"));
  auto handle = jsg::JsValue(KJ_ASSERT_NONNULL(ex.tryGetHandle(js)));
  return js.alloc<EntryIterator>(IteratorState(jsg::JsRef(js, handle)));
}

jsg::Ref<FileSystemDirectoryHandle::KeyIterator> FileSystemDirectoryHandle::keys(jsg::Lock& js) {
  KJ_IF_SOME(existing, resolveDirectoryHandle(js, getVfs(), getLocator())) {
    KJ_SWITCH_ONEOF(existing) {
      KJ_CASE_ONEOF(err, workerd::FsError) {
        JSG_FAIL_REQUIRE(DOMOperationError, "Failed to read directory: ", static_cast<int>(err));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        return js.alloc<KeyIterator>(
            IteratorState(JSG_THIS, collectEntries(getVfs(), js, dir, getLocator())));
      }
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        JSG_FAIL_REQUIRE(DOMTypeMismatchError, "Not a directory");
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        JSG_FAIL_REQUIRE(DOMTypeMismatchError, "Not a directory");
      }
    }
    KJ_UNREACHABLE;
  }

  // The directory was not found. However, for some weird reason the spec requires that
  // we still return an iterator here but it needs to throw a NotFoundError when next
  // is actually called.
  auto ex = js.domException(kj::str("NotFoundError"), kj::str("Not found"));
  auto handle = jsg::JsValue(KJ_ASSERT_NONNULL(ex.tryGetHandle(js)));
  return js.alloc<KeyIterator>(IteratorState(jsg::JsRef(js, handle)));
}

jsg::Ref<FileSystemDirectoryHandle::ValueIterator> FileSystemDirectoryHandle::values(
    jsg::Lock& js) {
  KJ_IF_SOME(existing, resolveDirectoryHandle(js, getVfs(), getLocator())) {
    KJ_SWITCH_ONEOF(existing) {
      KJ_CASE_ONEOF(err, workerd::FsError) {
        JSG_FAIL_REQUIRE(DOMOperationError, "Failed to read directory: ", static_cast<int>(err));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        return js.alloc<ValueIterator>(
            IteratorState(JSG_THIS, collectEntries(getVfs(), js, dir, getLocator())));
      }
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        JSG_FAIL_REQUIRE(DOMTypeMismatchError, "Not a directory");
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        JSG_FAIL_REQUIRE(DOMTypeMismatchError, "Not a directory");
      }
    }
    KJ_UNREACHABLE;
  }

  // The directory was not found. However, for some weird reason the spec requires that
  // we still return an iterator here but it needs to throw a NotFoundError when next
  // is actually called.
  auto ex = js.domException(kj::str("NotFoundError"), kj::str("Not found"));
  auto handle = jsg::JsValue(KJ_ASSERT_NONNULL(ex.tryGetHandle(js)));
  return js.alloc<ValueIterator>(IteratorState(jsg::JsRef(js, handle)));
}

void FileSystemDirectoryHandle::forEach(jsg::Lock& js,
    jsg::Function<void(
        jsg::USVString, jsg::Ref<FileSystemHandle>, jsg::Ref<FileSystemDirectoryHandle>)> callback,
    jsg::Optional<jsg::Value> thisArg,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& exception) {

  KJ_IF_SOME(existing, resolveDirectoryHandle(js, getVfs(), getLocator())) {
    KJ_SWITCH_ONEOF(existing) {
      KJ_CASE_ONEOF(err, workerd::FsError) {
        js.throwException(js.v8Ref(exception.wrap(js, fsErrorToDomException(js, err))));
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        auto receiver = js.v8Undefined();
        KJ_IF_SOME(arg, thisArg) {
          auto handle = arg.getHandle(js);
          if (!handle->IsNullOrUndefined()) {
            receiver = handle;
          }
        }
        callback.setReceiver(js.v8Ref(receiver));

        for (auto& entry: collectEntries(getVfs(), js, dir, getLocator())) {
          callback(js, js.accountedUSVString(entry->getName(js)), entry.addRef(), JSG_THIS);
        }
        return;
      }
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        JSG_FAIL_REQUIRE(DOMTypeMismatchError, "Not a directory");
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        JSG_FAIL_REQUIRE(DOMTypeMismatchError, "Not a directory");
      }
    }
    KJ_UNREACHABLE;
  }

  JSG_FAIL_REQUIRE(DOMNotFoundError, "Not found");
}

FileSystemFileHandle::FileSystemFileHandle(
    const workerd::VirtualFileSystem& vfs, jsg::Url locator, jsg::USVString name)
    : FileSystemHandle(vfs, kj::mv(locator), kj::mv(name)) {}

jsg::Promise<jsg::Ref<File>> FileSystemFileHandle::getFile(
    jsg::Lock& js, const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler) {
  // TODO(node-fs): Currently this copies the file data into the new File object.
  // Alternatively, File/Blob can be modified to allow it to be backed by a
  // workerd::File such that it does not need to create a separate in-memory
  // copy of the data. We can make that optimization as a follow-up, however.

  // First, let's use the locator and vfs to see if the file actually exists.
  KJ_IF_SOME(item, getVfs().resolve(js, getLocator(), {})) {
    KJ_SWITCH_ONEOF(item) {
      KJ_CASE_ONEOF(err, workerd::FsError) {
        return js.rejectedPromise<jsg::Ref<File>>(
            deHandler.wrap(js, fsErrorToDomException(js, err)));
      }
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        auto stat = file->stat(js);
        KJ_SWITCH_ONEOF(file->readAllBytes(js)) {
          KJ_CASE_ONEOF(bytes, jsg::BufferSource) {
            return js.resolvedPromise(
                js.alloc<File>(js, kj::mv(bytes), js.accountedUSVString(getName(js)), kj::String(),
                    (stat.lastModified - kj::UNIX_EPOCH) / kj::MILLISECONDS));
          }
          KJ_CASE_ONEOF(err, workerd::FsError) {
            return js.rejectedPromise<jsg::Ref<File>>(
                deHandler.wrap(js, fsErrorToDomException(js, err)));
          }
        }
        KJ_UNREACHABLE;
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Is a directory"));
        return js.rejectedPromise<jsg::Ref<File>>(deHandler.wrap(js, kj::mv(ex)));
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Is a symbolic link"));
        return js.rejectedPromise<jsg::Ref<File>>(deHandler.wrap(js, kj::mv(ex)));
      }
    }
    KJ_UNREACHABLE;
  }

  // If the file does not exist, we reject the promise with a NotFoundError.
  auto ex = js.domException(kj::str("NotFoundError"), kj::str("Not found"));
  return js.rejectedPromise<jsg::Ref<File>>(deHandler.wrap(js, kj::mv(ex)));
}

jsg::Promise<jsg::Ref<FileSystemWritableFileStream>> FileSystemFileHandle::createWritable(
    jsg::Lock& js,
    jsg::Optional<FileSystemCreateWritableOptions> options,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler,
    const jsg::TypeHandler<FileSystemWritableData>& dataHandler) {

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

  kj::Maybe<kj::Rc<workerd::File>> fileData;
  KJ_IF_SOME(existing, getVfs().resolve(js, getLocator(), {})) {
    if (keepExistingData) {
      KJ_SWITCH_ONEOF(existing) {
        KJ_CASE_ONEOF(err, workerd::FsError) {
          return js.rejectedPromise<jsg::Ref<FileSystemWritableFileStream>>(
              deHandler.wrap(js, fsErrorToDomException(js, err)));
        }
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          KJ_SWITCH_ONEOF(file->clone(js)) {
            KJ_CASE_ONEOF(err, workerd::FsError) {
              return js.rejectedPromise<jsg::Ref<FileSystemWritableFileStream>>(
                  deHandler.wrap(js, fsErrorToDomException(js, err)));
            }
            KJ_CASE_ONEOF(cloned, kj::Rc<workerd::File>) {
              fileData = kj::mv(cloned);
            }
          }
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Is a directory"));
          return js.rejectedPromise<jsg::Ref<FileSystemWritableFileStream>>(
              deHandler.wrap(js, kj::mv(ex)));
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Is a symbolic link"));
          return js.rejectedPromise<jsg::Ref<FileSystemWritableFileStream>>(
              deHandler.wrap(js, kj::mv(ex)));
        }
      }
    } else {
      fileData = workerd::File::newWritable(js);
    }
  } else {
    auto ex = js.domException(kj::str("NotFoundError"), kj::str("File not found"));
    return js.rejectedPromise<jsg::Ref<FileSystemWritableFileStream>>(
        deHandler.wrap(js, kj::mv(ex)));
  }

  auto sharedState = kj::rc<FileSystemWritableFileStream::State>(
      js, getVfs(), JSG_THIS, KJ_ASSERT_NONNULL(kj::mv(fileData)));
  auto stream =
      js.alloc<FileSystemWritableFileStream>(newWritableStreamJsController(), sharedState.addRef());

  stream->getController().setup(js,
      UnderlyingSink{.type = kj::str("bytes"),
        .write =
            [state = sharedState.addRef(), &deHandler, &dataHandler](
                jsg::Lock& js, v8::Local<v8::Value> chunk, auto c) mutable {
    return js.tryCatch([&] {
      KJ_IF_SOME(unwrapped, dataHandler.tryUnwrap(js, chunk)) {
        return FileSystemWritableFileStream::writeImpl(
            js, kj::mv(unwrapped), *state.get(), deHandler);
      }
      return js.rejectedPromise<void>(
          js.typeError("WritableStream received a value that is not writable"));
    }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
  },
        .abort =
            [state = sharedState.addRef()](jsg::Lock& js, auto reason) mutable {
    // When aborted, we just drop any of the written data on the floor.
    state->clear();
    return js.resolvedPromise();
  },
        .close =
            [state = sharedState.addRef(), &deHandler](jsg::Lock& js) mutable {
    KJ_DEFER(state->clear());
    return js.tryCatch([&] {
      KJ_IF_SOME(temp, state->temp) {
        auto basePath = kj::str(state->file->getLocator().getPathname().slice(1));
        kj::Path root{};
        auto base = root.eval(basePath);

        KJ_IF_SOME(existing,
            state->vfs.getRoot(js)->tryOpen(js, base,
                {
                  .createAs = workerd::FsType::FILE,
                })) {
          KJ_SWITCH_ONEOF(existing) {
            KJ_CASE_ONEOF(err, workerd::FsError) {
              return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              auto ex = js.domException(kj::str("TypeMismatchError"), kj::str("Is a directory"));
              return js.rejectedPromise<void>(deHandler.wrap(js, kj::mv(ex)));
            }
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              KJ_IF_SOME(err, file->replace(js, temp.addRef())) {
                return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
              }
              return js.resolvedPromise();
            }
            KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
              auto ex =
                  js.domException(kj::str("TypeMismatchError"), kj::str("Is a symbolic link"));
              return js.rejectedPromise<void>(deHandler.wrap(js, kj::mv(ex)));
            }
          }
          KJ_UNREACHABLE;
        }
        auto ex =
            js.domException(kj::str("InvalidStateError"), kj::str("Failed to open or create file"));
        return js.rejectedPromise<void>(deHandler.wrap(js, kj::mv(ex)));
      }
      return js.resolvedPromise();
    }, [&](jsg::Value exception) { return js.rejectedPromise<void>(kj::mv(exception)); });
  }},
      kj::none);

  return js.resolvedPromise(kj::mv(stream));
}

FileSystemWritableFileStream::FileSystemWritableFileStream(
    kj::Own<WritableStreamController> controller, kj::Rc<State> sharedState)
    : WritableStream(kj::mv(controller)),
      sharedState(kj::mv(sharedState)) {}

jsg::Promise<void> FileSystemWritableFileStream::write(jsg::Lock& js,
    kj::OneOf<jsg::Ref<Blob>, jsg::BufferSource, kj::String, WriteParams> data,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler) {
  JSG_REQUIRE(!getController().isLockedToWriter(), TypeError,
      "Cannot write to a stream that is locked to a reader");
  auto writer = getWriter(js);
  KJ_DEFER(writer->releaseLock(js));
  return writeImpl(js, kj::mv(data), *sharedState.get(), deHandler);
}

jsg::Promise<void> FileSystemWritableFileStream::writeImpl(jsg::Lock& js,
    FileSystemWritableData data,
    State& state,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler) {
  KJ_IF_SOME(inner, state.temp) {
    return js.tryCatch([&] {
      KJ_SWITCH_ONEOF(data) {
        KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
          KJ_SWITCH_ONEOF(inner->write(js, state.position, blob->getData())) {
            KJ_CASE_ONEOF(written, uint32_t) {
              state.position += written;
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
            }
          }
        }
        KJ_CASE_ONEOF(buffer, jsg::BufferSource) {
          KJ_SWITCH_ONEOF(inner->write(js, state.position, buffer)) {
            KJ_CASE_ONEOF(written, uint32_t) {
              state.position += written;
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
            }
          }
        }
        KJ_CASE_ONEOF(str, kj::String) {
          KJ_SWITCH_ONEOF(inner->write(js, state.position, str)) {
            KJ_CASE_ONEOF(written, uint32_t) {
              state.position += written;
            }
            KJ_CASE_ONEOF(err, workerd::FsError) {
              return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
            }
          }
        }
        KJ_CASE_ONEOF(params, WriteParams) {
          uint32_t offset = state.position;
          KJ_IF_SOME(pos, params.position) {
            auto stat = inner->stat(js);
            if (pos > stat.size) {
              KJ_IF_SOME(err, inner->resize(js, offset)) {
                return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
              }
            }
            offset = pos;
          }

          if (params.type == "write"_kj) {
            KJ_IF_SOME(maybeData, params.data) {
              KJ_IF_SOME(data, maybeData) {
                KJ_SWITCH_ONEOF(data) {
                  KJ_CASE_ONEOF(blob, jsg::Ref<Blob>) {
                    KJ_SWITCH_ONEOF(inner->write(js, offset, blob->getData())) {
                      KJ_CASE_ONEOF(written, uint32_t) {
                        state.position = offset + written;
                        return js.resolvedPromise();
                      }
                      KJ_CASE_ONEOF(err, workerd::FsError) {
                        return js.rejectedPromise<void>(
                            deHandler.wrap(js, fsErrorToDomException(js, err)));
                      }
                    }
                    KJ_UNREACHABLE;
                  }
                  KJ_CASE_ONEOF(buffer, jsg::BufferSource) {
                    KJ_SWITCH_ONEOF(inner->write(js, offset, buffer)) {
                      KJ_CASE_ONEOF(written, uint32_t) {
                        state.position = offset + written;
                        return js.resolvedPromise();
                      }
                      KJ_CASE_ONEOF(err, workerd::FsError) {
                        return js.rejectedPromise<void>(
                            deHandler.wrap(js, fsErrorToDomException(js, err)));
                      }
                    }
                    KJ_UNREACHABLE;
                  }
                  KJ_CASE_ONEOF(str, kj::String) {
                    KJ_SWITCH_ONEOF(inner->write(js, offset, str)) {
                      KJ_CASE_ONEOF(written, uint32_t) {
                        state.position = offset + written;
                        return js.resolvedPromise();
                      }
                      KJ_CASE_ONEOF(err, workerd::FsError) {
                        return js.rejectedPromise<void>(
                            deHandler.wrap(js, fsErrorToDomException(js, err)));
                      }
                    }
                  }
                  KJ_UNREACHABLE;
                }
              } else {
                return js.rejectedPromise<void>(
                    js.typeError("write() requires a non-null data parameter"));
              }
            }

            return js.rejectedPromise<void>(deHandler.wrap(js,
                js.domException(kj::str("SyntaxError"),
                    kj::str("write() requires a non-null data parameter"))));

          } else if (params.type == "seek"_kj) {
            uint32_t pos;
            KJ_IF_SOME(s, params.position) {
              pos = s;
            } else {
              return js.rejectedPromise<void>(deHandler.wrap(js,
                  js.domException(
                      kj::str("SyntaxError"), kj::str("seek() requires a position parameter"))));
            }
            state.position = pos;
            auto stat = inner->stat(js);
            if (state.position > stat.size) {
              KJ_IF_SOME(err, inner->resize(js, state.position)) {
                return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
              }
            }
          } else if (params.type == "truncate"_kj) {
            uint32_t size = 0;
            KJ_IF_SOME(s, params.size) {
              size = s;
            } else {
              return js.rejectedPromise<void>(deHandler.wrap(js,
                  js.domException(
                      kj::str("SyntaxError"), kj::str("truncate() requires a size parameter"))));
            }
            KJ_IF_SOME(err, inner->resize(js, size)) {
              return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
            }
            auto stat = inner->stat(js);
            if (state.position > stat.size) {
              state.position = stat.size;
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

  return js.rejectedPromise<void>(js.typeError("write() after closed"));
}

jsg::Promise<void> FileSystemWritableFileStream::seek(jsg::Lock& js,
    uint32_t position,
    const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler) {
  KJ_IF_SOME(inner, sharedState->temp) {
    auto stat = inner->stat(js);
    if (position > stat.size) {
      KJ_IF_SOME(err, inner->resize(js, position)) {
        return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
      }
    }
    sharedState->position = position;
    return js.resolvedPromise();
  }

  return js.rejectedPromise<void>(js.typeError("seek() after closed"));
}

jsg::Promise<void> FileSystemWritableFileStream::truncate(
    jsg::Lock& js, uint32_t size, const jsg::TypeHandler<jsg::Ref<jsg::DOMException>>& deHandler) {
  KJ_IF_SOME(inner, sharedState->temp) {
    KJ_IF_SOME(err, inner->resize(js, size)) {
      return js.rejectedPromise<void>(deHandler.wrap(js, fsErrorToDomException(js, err)));
    }
    auto stat = inner->stat(js);
    if (sharedState->position > stat.size) {
      sharedState->position = stat.size;
    }
    return js.resolvedPromise();
  }

  return js.rejectedPromise<void>(js.typeError("seek() after closed"));
}
}  // namespace workerd::api
