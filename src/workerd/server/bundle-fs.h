#pragma once

#include <workerd/io/worker-fs.h>
#include <workerd/io/worker-source.h>

namespace workerd::server {

// Create a VirtualFileSystem directory from the workerd bundle
// configuration. Each type of module in the bundle is represented as a file.
// The directory structure is created based on the module names. For example,
// if the bundle contains a module with the name "foo/bar/baz", it will be
// represented as a directory "foo" with a subdirectory "bar" and a file
// "baz" inside it. The directory structure and files are read-only. All
// timestamps are set to the Unix epoch.
kj::Rc<Directory> getBundleDirectory(const WorkerSource& source);

}  // namespace workerd::server
