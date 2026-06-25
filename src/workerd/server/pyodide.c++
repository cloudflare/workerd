// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "pyodide.h"

#include <workerd/api/pyodide/pyodide.h>

#include <kj/array.h>
#include <kj/common.h>
#include <kj/compat/gzip.h>
#include <kj/compat/tls.h>
#include <kj/debug.h>
#include <kj/string.h>

namespace workerd::server {

// Helper functions for bundle file operations
kj::Path getPyodideBundleFileName(kj::StringPtr version) {
  return kj::Path(kj::str("pyodide_", version, ".capnp.bin"));
}

kj::Maybe<kj::Own<const kj::ReadableFile>> getPyodideBundleFile(
    const kj::Maybe<kj::Own<const kj::Directory>>& maybeDir, kj::StringPtr version) {
  KJ_IF_SOME(dir, maybeDir) {
    kj::Path filename = getPyodideBundleFileName(version);
    auto file = dir->tryOpenFile(filename);

    return file;
  }

  return kj::none;
}

void writePyodideBundleFileToDisk(const kj::Maybe<kj::Own<const kj::Directory>>& maybeDir,
    kj::StringPtr version,
    kj::ArrayPtr<byte> bytes) {
  KJ_IF_SOME(dir, maybeDir) {
    kj::Path filename = getPyodideBundleFileName(version);
    auto replacer = dir->replaceFile(filename, kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

    replacer->get().writeAll(bytes);
    replacer->commit();
  }
}

// Used to preload the Pyodide bundle during workerd startup
kj::Promise<kj::Maybe<jsg::Bundle::Reader>> fetchPyodideBundle(
    const api::pyodide::PythonConfig& pyConfig,
    kj::String version,
    kj::StringPtr integrity,
    kj::Network& network,
    kj::Timer& timer) {
  if (pyConfig.pyodideBundleManager.getPyodideBundle(version) != kj::none) {
    co_return pyConfig.pyodideBundleManager.getPyodideBundle(version);
  }

  auto maybePyodideBundleFile = getPyodideBundleFile(pyConfig.pyodideDiskCacheRoot, version);
  KJ_IF_SOME(pyodideBundleFile, maybePyodideBundleFile) {
    auto body = pyodideBundleFile->readAllBytes();
    api::pyodide::verifyPyodideBundleIntegrity(version, integrity, body);
    pyConfig.pyodideBundleManager.setPyodideBundleData(kj::str(version), kj::mv(body));
    co_return pyConfig.pyodideBundleManager.getPyodideBundle(version);
  }

  if (version == "dev") {
    // the "dev" version is special and indicates we're using the tip-of-tree version built for testing
    // so we shouldn't fetch it from the internet, only check for its existence in the disk cache
    co_return kj::none;
  }

  kj::String url =
      kj::str("https://pyodide-capnp-bin.edgeworker.net/pyodide_", version, ".capnp.bin");
  KJ_LOG(INFO, "Loading Pyodide bundle from internet", url);
  kj::HttpHeaderTable table;

  kj::TlsContext::Options options;
  options.useSystemTrustStore = true;

  kj::Own<kj::TlsContext> tls = kj::heap<kj::TlsContext>(kj::mv(options));
  auto tlsNetwork = tls->wrapNetwork(network);
  auto client = kj::newHttpClient(timer, table, network, *tlsNetwork);

  kj::HttpHeaders headers(table);

  auto req = client->request(kj::HttpMethod::GET, url.asPtr(), headers);

  auto res = co_await req.response;
  KJ_ASSERT(res.statusCode == 200, "Request for Pyodide bundle failed", url);
  auto body = co_await res.body->readAllBytes();

  api::pyodide::verifyPyodideBundleIntegrity(version, integrity, body);

  writePyodideBundleFileToDisk(pyConfig.pyodideDiskCacheRoot, version, body);

  pyConfig.pyodideBundleManager.setPyodideBundleData(kj::str(version), kj::mv(body));

  co_return pyConfig.pyodideBundleManager.getPyodideBundle(version);
}

}  // namespace workerd::server
