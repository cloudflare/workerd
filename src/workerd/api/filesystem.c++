#include "filesystem.h"

#include "blob.h"
#include "url-standard.h"
#include "url.h"

#include <workerd/api/streams/standard.h>

#include <kj/time.h>

namespace workerd::api {

// =======================================================================================
// Implementation of cloudflare-internal:filesystem in support of node:fs

// TODO(node-fs): The functions here are implemented but may not be fully
// correct or performant. The implementation is a work in progress that
// will be improved on incrementally.

namespace {
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
      NormalizedFilePath normalized(kj::mv(path));
      auto node = JSG_REQUIRE_NONNULL(vfs.resolve(js, normalized), Error, "File not found"_kj);

      KJ_SWITCH_ONEOF(node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          file->setLastModified(js, lastModified);
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          // We currently do not support setting the last modified time
          // on directories or links.
          JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          if (!options.followSymlinks.orDefault(true)) {
            // We currently do not support setting the last modified time
            // on directories or links.
            JSG_FAIL_REQUIRE(Error, "Illegal operation on a link");
          }

          SymbolicLinkRecursionGuardScope guard;
          guard.checkSeen(link.get());
          auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
          KJ_SWITCH_ONEOF(resolved) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              file->setLastModified(js, lastModified);
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              // We currently do not support setting the last modified time
              // on directories or links.
              JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
            }
          }
        }
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
      KJ_SWITCH_ONEOF(opened.node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          file->setLastModified(js, lastModified);
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          // We currently do not support setting the last modified time
          // on directories or links.
          JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          if (!options.followSymlinks.orDefault(true)) {
            // We currently do not support setting the last modified time
            // on directories or links.
            JSG_FAIL_REQUIRE(Error, "Illegal operation on a link");
          }

          SymbolicLinkRecursionGuardScope guard;
          guard.checkSeen(link.get());
          auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
          KJ_SWITCH_ONEOF(resolved) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              file->setLastModified(js, lastModified);
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              // We currently do not support setting the last modified time
              // on directories or links.
              JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
            }
          }
        }
      }
    }
  }
}

void FileSystemModule::resize(jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd, uint32_t size) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalized(kj::mv(path));
      auto node = JSG_REQUIRE_NONNULL(vfs.resolve(js, normalized), Error, "File not found"_kj);
      KJ_SWITCH_ONEOF(node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          file->resize(js, size);
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          // We do not support resizing directories.
          JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          SymbolicLinkRecursionGuardScope guard;
          guard.checkSeen(link.get());
          auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
          KJ_SWITCH_ONEOF(resolved) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              file->resize(js, size);
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              // We do not support resizing directories.
              JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
            }
          }
        }
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
      JSG_REQUIRE(opened.write, Error, "Illegal operation on a read-only file descriptor"_kj);
      KJ_SWITCH_ONEOF(opened.node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          file->resize(js, size);
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          // We do not support resizing directories.
          JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          SymbolicLinkRecursionGuardScope guard;
          guard.checkSeen(link.get());
          auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
          KJ_SWITCH_ONEOF(resolved) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              file->resize(js, size);
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              // We do not support resizing directories.
              JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
            }
          }
        }
      }
    }
  }
}

void FileSystemModule::link(jsg::Lock& js, FilePath path, FilePath target) {
  NormalizedFilePath existingPath(kj::mv(path));
  NormalizedFilePath targetPath(kj::mv(target));

  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);

  // We need the existing path to actually exist...
  auto existing = JSG_REQUIRE_NONNULL(vfs.resolve(js, existingPath), Error, "File not found"_kj);

  const jsg::Url& targetUrl = targetPath;
  JSG_REQUIRE(vfs.resolve(js, targetUrl) == kj::none, Error, "Target path already exists"_kj);

  // Get the parent directory of the target path.
  auto targetDir =
      JSG_REQUIRE_NONNULL(jsg::Url::tryParse("."_kj, targetUrl.getHref()), Error, "Invalid URL"_kj);

  // The parent directory of the target path has to exist.
  auto maybeParentDir =
      JSG_REQUIRE_NONNULL(vfs.resolve(js, targetDir), Error, "Parent directory not found"_kj);
  // The parent must be a directory.
  auto& parentDir = JSG_REQUIRE_NONNULL(maybeParentDir.tryGet<kj::Rc<workerd::Directory>>(), Error,
      "Parent path is not a directory"_kj);

  // Now, we'll grab the last part of the target path and use that as the name of the link.
  KJ_IF_SOME(pos, targetUrl.getPathname().findLast('/')) {
    auto name = kj::str(targetUrl.getPathname().slice(pos + 1));
    KJ_SWITCH_ONEOF(existing) {
      KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
        parentDir->add(js, name, file.addRef());
      }
      KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
        parentDir->add(js, name, dir.addRef());
      }
      KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
        parentDir->add(js, name, link.addRef());
      }
    }
  }
}

void FileSystemModule::symlink(jsg::Lock& js, FilePath path, FilePath target) {
  NormalizedFilePath existingPath(kj::mv(path));
  NormalizedFilePath targetPath(kj::mv(target));

  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);

  // Unlike link, we don't require the existing path to actually exist.
  auto symlink = vfs.newSymbolicLink(js, existingPath);

  const jsg::Url& targetUrl = targetPath;
  JSG_REQUIRE(vfs.resolve(js, targetUrl) == kj::none, Error, "Target path already exists"_kj);

  // Get the parent directory of the target path.
  auto targetDir =
      JSG_REQUIRE_NONNULL(jsg::Url::tryParse("."_kj, targetUrl.getHref()), Error, "Invalid URL"_kj);

  // The parent directory of the target path has to exist.
  auto maybeParentDir =
      JSG_REQUIRE_NONNULL(vfs.resolve(js, targetDir), Error, "Parent directory not found"_kj);
  // The parent must be a directory.
  auto& parentDir = JSG_REQUIRE_NONNULL(maybeParentDir.tryGet<kj::Rc<workerd::Directory>>(), Error,
      "Parent path is not a directory"_kj);

  // Now, we'll grab the last part of the target path and use that as the name of the link.
  KJ_IF_SOME(pos, targetUrl.getPathname().findLast('/')) {
    auto name = kj::str(targetUrl.getPathname().slice(pos + 1));
    parentDir->add(js, name, kj::mv(symlink));
  }
}

void FileSystemModule::unlink(jsg::Lock& js, FilePath path) {
  NormalizedFilePath normalized(kj::mv(path));
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);

  // Let's get the parent directory of the path.
  const jsg::Url& url = normalized;
  auto parentPath =
      JSG_REQUIRE_NONNULL(jsg::Url::tryParse("."_kj, url.getHref()), Error, "Invalid URL"_kj);
  KJ_IF_SOME(resolved, vfs.resolve(js, parentPath)) {
    auto& parentDir = JSG_REQUIRE_NONNULL(
        resolved.tryGet<kj::Rc<workerd::Directory>>(), Error, "Parent path is not a directory"_kj);

    // Now, we'll grab the last part of the target path and use that as the name of the link.
    KJ_IF_SOME(pos, url.getPathname().findLast('/')) {
      auto name = kj::str(url.getPathname().slice(pos + 1));
      parentDir->remove(js, kj::Path({name}));
    }
  }
}

kj::String FileSystemModule::realpath(jsg::Lock& js, FilePath path) {
  NormalizedFilePath normalized(kj::mv(path));
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);

  const jsg::Url& url = normalized;
  auto node = JSG_REQUIRE_NONNULL(vfs.resolve(js, url, {false}), Error, "File not found"_kj);
  KJ_SWITCH_ONEOF(node) {
    KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
      // If we resolve to a file, we can just return the path as is.
      return kj::str(url.getPathname());
    }
    KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
      // If we resolve to a directory, we can just return the path as is.
      return kj::str(url.getPathname());
    }
    KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
      return kj::str(link->getTargetUrl().getPathname());
    }
  }
  KJ_UNREACHABLE;
}

kj::String FileSystemModule::readlink(jsg::Lock& js, FilePath path) {
  NormalizedFilePath normalized(kj::mv(path));
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);

  const jsg::Url& url = normalized;
  auto node = JSG_REQUIRE_NONNULL(vfs.resolve(js, url, {false}), Error, "File not found"_kj);
  KJ_SWITCH_ONEOF(node) {
    KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
      JSG_FAIL_REQUIRE(Error, "Illegal operation on a file");
    }
    KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
      JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
    }
    KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
      return kj::str(link->getTargetUrl().getPathname());
    }
  }
  KJ_UNREACHABLE;
}

int FileSystemModule::open(jsg::Lock& js, FilePath path, OpenOptions options) {
  NormalizedFilePath normalized(kj::mv(path));
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  return vfs
      .openFd(js, normalized,
          {
            .read = options.read,
            .write = options.write,
            .append = options.append,
            .exclusive = options.exclusive,
            .followLinks = true,
          })
      .fd;
}

void FileSystemModule::close(jsg::Lock& js, int fd) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  vfs.closeFd(js, fd);
}

void FileSystemModule::fsync(jsg::Lock& js, int fd) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  // We don't meaningfully implement sync or datasync in any way. Here we just want to
  // validate that the fd is valid, then do nothing else.
  JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
}

jsg::BufferSource FileSystemModule::readfile(jsg::Lock& js, kj::OneOf<int, FilePath> pathOrFd) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalized(kj::mv(path));
      auto node = JSG_REQUIRE_NONNULL(vfs.resolve(js, normalized), Error, "File not found"_kj);
      KJ_SWITCH_ONEOF(node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          return file->readAllBytes(js);
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          SymbolicLinkRecursionGuardScope guard;
          guard.checkSeen(link.get());
          auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
          KJ_SWITCH_ONEOF(resolved) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              return file->readAllBytes(js);
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
            }
          }
        }
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
      JSG_REQUIRE(opened.read, Error, "Illegal operation on a write-only file descriptor"_kj);
      KJ_SWITCH_ONEOF(opened.node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          return file->readAllBytes(js);
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          SymbolicLinkRecursionGuardScope guard;
          guard.checkSeen(link.get());
          auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
          KJ_SWITCH_ONEOF(resolved) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              return file->readAllBytes(js);
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
            }
          }
        }
      }
    }
  }
  KJ_UNREACHABLE;
}

void FileSystemModule::writeFile(jsg::Lock& js,
    kj::OneOf<int, FilePath> pathOrFd,
    jsg::BufferSource data,
    WriteFileOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);

  KJ_SWITCH_ONEOF(pathOrFd) {
    KJ_CASE_ONEOF(path, FilePath) {
      NormalizedFilePath normalized(kj::mv(path));
      KJ_IF_SOME(node, vfs.resolve(js, normalized)) {
        KJ_SWITCH_ONEOF(node) {
          KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
            if (options.append) {
              auto stat = file->stat(js);
              file->write(js, stat.size, data);
            } else {
              file->writeAll(js, data);
            }
          }
          KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
            JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
          }
          KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
            SymbolicLinkRecursionGuardScope guard;
            guard.checkSeen(link.get());
            auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
            KJ_SWITCH_ONEOF(resolved) {
              KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
                if (options.append) {
                  auto stat = file->stat(js);
                  file->write(js, stat.size, data);
                } else {
                  file->writeAll(js, data);
                }
              }
              KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
                JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
              }
            }
          }
        }
      } else {
        kj::Path path = normalized;
        // The file does not yet exist. Let's try to create it. If the file cannot
        // be created it is likely because the parent directory does not exist.
        auto shouldBeFile =
            JSG_REQUIRE_NONNULL(vfs.getRoot(js)->tryOpen(js, path,
                                    Directory::OpenOptions{.createAs = workerd::FsType::FILE}),
                Error, "Failed to create file"_kj);
        auto file = kj::mv(shouldBeFile).get<kj::Rc<workerd::File>>();
        file->writeAll(js, data);
      }
    }
    KJ_CASE_ONEOF(fd, int) {
      auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
      JSG_REQUIRE(opened.write, Error, "Illegal operation on a read-only file descriptor"_kj);
      KJ_SWITCH_ONEOF(opened.node) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          if (options.append) {
            auto stat = file->stat(js);
            file->write(js, stat.size, data);
            opened.position = stat.size + data.size();
          } else {
            file->writeAll(js, data);
            opened.position = data.size();
          }
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
        }
        KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
          SymbolicLinkRecursionGuardScope guard;
          guard.checkSeen(link.get());
          auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
          KJ_SWITCH_ONEOF(resolved) {
            KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
              if (options.append) {
                auto stat = file->stat(js);
                file->write(js, stat.size, data);
                opened.position = stat.size + data.size();
              } else {
                file->writeAll(js, data);
                opened.position = data.size();
              }
            }
            KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
              JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
            }
          }
        }
      }
    }
  }
}

uint32_t FileSystemModule::readv(
    jsg::Lock& js, int fd, kj::Array<jsg::BufferSource> buffer, ReadOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
  JSG_REQUIRE(opened.read, Error, "Illegal operation on a write-only file descriptor"_kj);

  static const auto getsize = [](kj::ArrayPtr<jsg::BufferSource> buffer) {
    uint32_t size = 0;
    for (auto& b: buffer) {
      size += b.size();
    }
    return size;
  };

  static const auto readInto = [](jsg::Lock& js, kj::ArrayPtr<jsg::BufferSource> buffer,
                                   kj::Rc<workerd::File>& file, uint32_t position) -> uint32_t {
    auto stat = file->stat(js);
    // if the file is a device, skip checking the size since there is no
    // meaningful size for a device. We'll read as much as we can.
    uint32_t remaining = !stat.device ? stat.size - position : getsize(buffer);
    uint32_t amount = 0;
    if (buffer.size() == 0 || remaining == 0) return amount;
    for (auto& b: buffer) {
      if (remaining == 0) break;
      if (b.size() == 0) continue;
      uint32_t len = file->read(js, position, b);
      KJ_ASSERT((remaining - len) >= 0, "read past end of file");
      remaining -= len;
      amount += len;
    }
    return amount;
  };

  KJ_SWITCH_ONEOF(opened.node) {
    KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
      uint32_t amount = readInto(js, buffer, file, options.position.orDefault(opened.position));
      if (options.position == kj::none) {
        // Check for rollover
        JSG_REQUIRE(
            opened.position + amount >= opened.position, Error, "Read beyond end of file"_kj);
        opened.position += amount;
      }
      return amount;
    }
    KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
      JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
    }
    KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
      SymbolicLinkRecursionGuardScope guard;
      guard.checkSeen(link.get());
      auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
      KJ_SWITCH_ONEOF(resolved) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          uint32_t amount = readInto(js, buffer, file, options.position.orDefault(opened.position));
          if (options.position == kj::none) {
            JSG_REQUIRE(
                opened.position + amount >= opened.position, Error, "Read beyond emd of file"_kj);
            opened.position += amount;
          }
          return amount;
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
        }
      }
    }
  }
  KJ_UNREACHABLE;
}

uint32_t FileSystemModule::writev(
    jsg::Lock& js, int fd, kj::Array<jsg::BufferSource> buffer, ReadOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  auto& opened = JSG_REQUIRE_NONNULL(vfs.tryGetFd(js, fd), Error, "Bad file descriptor"_kj);
  JSG_REQUIRE(opened.write, Error, "Illegal operation on a read-only file descriptor"_kj);

  static const auto writeInfo = [](jsg::Lock& js, kj::Rc<workerd::File>& file,
                                    kj::ArrayPtr<jsg::BufferSource> buffers,
                                    uint32_t position) -> uint32_t {
    uint32_t amount = 0;
    for (auto& buffer: buffers) {
      if (buffer.size() == 0) continue;
      uint32_t len = file->write(js, position, buffer);
      KJ_ASSERT(len == buffer.size(), "write did not write all bytes");
      amount += len;
      position += len;
    }
    return amount;
  };

  KJ_SWITCH_ONEOF(opened.node) {
    KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
      auto amount = writeInfo(js, file, buffer, options.position.orDefault(opened.position));
      if (options.position == kj::none) {
        // Check for rollover
        JSG_REQUIRE(
            opened.position + amount >= opened.position, Error, "Write beyond end of file"_kj);
        opened.position += amount;
      }
      return amount;
    }
    KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
      JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
    }
    KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
      SymbolicLinkRecursionGuardScope guard;
      guard.checkSeen(link.get());
      auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
      KJ_SWITCH_ONEOF(resolved) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          auto amount = writeInfo(js, file, buffer, options.position.orDefault(opened.position));
          if (options.position == kj::none) {
            // Check for rollover
            JSG_REQUIRE(
                opened.position + amount >= opened.position, Error, "Write beyond end of file"_kj);
            opened.position += amount;
          }
          return amount;
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
        }
      }
    }
  }
  KJ_UNREACHABLE;
}

void FileSystemModule::copyFile(
    jsg::Lock& js, FilePath from, FilePath to, CopyFileOptions options) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  NormalizedFilePath fromPath(kj::mv(from));
  NormalizedFilePath toPath(kj::mv(to));
  auto fromNode = JSG_REQUIRE_NONNULL(vfs.resolve(js, fromPath), Error, "File not found"_kj);

  if (options.exclusive) {
    // Check if the target file already exists.
    JSG_REQUIRE(vfs.resolve(js, toPath) == kj::none, Error, "Target file already exists"_kj);
  }

  static const auto doCopy = [](jsg::Lock& js, const workerd::VirtualFileSystem& vfs,
                                 kj::Rc<workerd::File>& fromFile, NormalizedFilePath& toPath) {
    auto stat = fromFile->stat(js);
    JSG_REQUIRE(!stat.device, Error, "Cannot copy a device"_kj);

    // Get the parent directory of the target path.
    const jsg::Url& targetUrl = toPath;
    auto targetDir = JSG_REQUIRE_NONNULL(
        jsg::Url::tryParse("."_kj, targetUrl.getHref()), Error, "Invalid URL"_kj);

    // The parent directory of the target path has to exist.
    auto maybeParentDir =
        JSG_REQUIRE_NONNULL(vfs.resolve(js, targetDir), Error, "Parent directory not found"_kj);
    // The parent must be a directory.
    auto& parentDir = JSG_REQUIRE_NONNULL(maybeParentDir.tryGet<kj::Rc<workerd::Directory>>(),
        Error, "Parent path is not a directory"_kj);

    // Now, we'll grab the last part of the target path and use that as the name of the link.
    KJ_IF_SOME(pos, targetUrl.getPathname().findLast('/')) {
      auto name = kj::str(targetUrl.getPathname().slice(pos + 1));
      parentDir->remove(js, kj::Path({name}));
      parentDir->add(js, name, fromFile->clone(js));
    }
  };

  KJ_SWITCH_ONEOF(fromNode) {
    KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
      doCopy(js, vfs, file, toPath);
    }
    KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
      JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
    }
    KJ_CASE_ONEOF(link, kj::Rc<workerd::SymbolicLink>) {
      SymbolicLinkRecursionGuardScope guard;
      guard.checkSeen(link.get());
      auto resolved = JSG_REQUIRE_NONNULL(link->resolve(js), Error, "File not found"_kj);
      KJ_SWITCH_ONEOF(resolved) {
        KJ_CASE_ONEOF(file, kj::Rc<workerd::File>) {
          doCopy(js, vfs, file, toPath);
        }
        KJ_CASE_ONEOF(dir, kj::Rc<workerd::Directory>) {
          JSG_FAIL_REQUIRE(Error, "Illegal operation on a directory");
        }
      }
    }
  }
}

void FileSystemModule::rename(jsg::Lock& js, FilePath from, FilePath to) {
  auto& vfs = JSG_REQUIRE_NONNULL(
      workerd::VirtualFileSystem::tryGetCurrent(js), Error, "No current virtual file system"_kj);
  NormalizedFilePath fromPath(kj::mv(from));
  NormalizedFilePath toPath(kj::mv(to));
  auto fromNode = JSG_REQUIRE_NONNULL(vfs.resolve(js, fromPath), Error, "File not found"_kj);
  JSG_REQUIRE(vfs.resolve(js, toPath) == kj::none, Error, "Target file already exists"_kj);

  const jsg::Url& fromUrl = fromPath;
  const jsg::Url& toUrl = toPath;
  auto sourceDir =
      JSG_REQUIRE_NONNULL(jsg::Url::tryParse("."_kj, fromUrl.getHref()), Error, "Invalid URL"_kj);
  auto targetDir =
      JSG_REQUIRE_NONNULL(jsg::Url::tryParse("."_kj, toUrl.getHref()), Error, "Invalid URL"_kj);

  // The parent directory of the target path has to exist.
  auto maybeToParentDir =
      JSG_REQUIRE_NONNULL(vfs.resolve(js, targetDir), Error, "Parent directory not found"_kj);
  // The parent must be a directory.
  auto& toParentDir = JSG_REQUIRE_NONNULL(maybeToParentDir.tryGet<kj::Rc<workerd::Directory>>(),
      Error, "Parent path is not a directory"_kj);

  auto maybeFromParentDir =
      JSG_REQUIRE_NONNULL(vfs.resolve(js, sourceDir), Error, "Parent directory not found"_kj);
  // The parent must be a directory.
  auto& fromParentDir = JSG_REQUIRE_NONNULL(maybeFromParentDir.tryGet<kj::Rc<workerd::Directory>>(),
      Error, "Parent path is not a directory"_kj);

  // Now we'll grab the last part of the original path so we can remove it from that directory.
  KJ_IF_SOME(pos, fromUrl.getPathname().findLast('/')) {
    auto name = kj::str(fromUrl.getPathname().slice(pos + 1));
    fromParentDir->remove(js, kj::Path({name}));
  }

  // Now we'll grab the last part of the target path and use to add it to the target directory.
  KJ_IF_SOME(pos, toUrl.getPathname().findLast('/')) {
    auto name = kj::str(toUrl.getPathname().slice(pos + 1));
    toParentDir->add(js, name, kj::mv(fromNode));
  }
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
