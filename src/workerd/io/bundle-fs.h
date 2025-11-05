#pragma once

#include <workerd/io/worker-fs.h>
#include <workerd/io/worker-source.h>

namespace workerd {

// Create a VirtualFileSystem directory from the bundle configuration. Each type of
// module in the bundle is represented as a file. The directory structure is created
// based on the module names. For example, if the bundle contains a module with the
// name "foo/bar/baz", it will be represented as a directory "foo" with a subdirectory
// "bar" and a file "baz" inside it. The directory structure and files are read-only.
// All timestamps are set to the Unix epoch.
//
// Callers are expected to ensure that the pointers held by the WorkerSource remain
// valid for the lifetime of the returned Directory.
kj::Rc<Directory> getBundleDirectory(const WorkerSource& source);

}  // namespace workerd
