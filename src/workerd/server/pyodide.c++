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
    kj::Network& network,
    kj::Timer& timer) {
  if (pyConfig.pyodideBundleManager.getPyodideBundle(version) != kj::none) {
    co_return pyConfig.pyodideBundleManager.getPyodideBundle(version);
  }

  auto maybePyodideBundleFile = getPyodideBundleFile(pyConfig.pyodideDiskCacheRoot, version);
  KJ_IF_SOME(pyodideBundleFile, maybePyodideBundleFile) {
    auto body = pyodideBundleFile->readAllBytes();
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

  writePyodideBundleFileToDisk(pyConfig.pyodideDiskCacheRoot, version, body);

  pyConfig.pyodideBundleManager.setPyodideBundleData(kj::str(version), kj::mv(body));

  co_return pyConfig.pyodideBundleManager.getPyodideBundle(version);
}

// Downloads a package with retry logic (up to 3 attempts with 5-second delays)
kj::Promise<kj::Maybe<kj::Array<byte>>> downloadPackageWithRetry(kj::HttpClient& client,
    kj::Timer& timer,
    kj::HttpHeaderTable& headerTable,
    kj::StringPtr url,
    kj::StringPtr path) {
  constexpr uint retryLimit = 3;
  kj::HttpHeaders headers(headerTable);

  for (uint retryCount = 0; retryCount < retryLimit; ++retryCount) {
    if (retryCount > 0) {
      // Sleep for 5 seconds before retrying
      co_await timer.afterDelay(5 * kj::SECONDS);
      KJ_LOG(INFO, "Retrying package download", path, "attempt", retryCount + 1, "of", retryLimit);
    }

    try {
      auto req = client.request(kj::HttpMethod::GET, url, headers);
      auto res = co_await req.response;

      if (res.statusCode != 200) {
        KJ_LOG(WARNING, "Failed to download package", path, res.statusCode, "attempt",
            retryCount + 1, "of", retryLimit);
        continue;  // Try again in the next iteration
      }

      // Request succeeded, read the body
      co_return co_await res.body->readAllBytes();
    } catch (kj::Exception& e) {
      if (retryCount + 1 >= retryLimit) {
        // This was our last attempt
        KJ_LOG(WARNING, "Failed to download package after all retry attempts", path, e, "attempts",
            retryLimit);
      } else {
        KJ_LOG(WARNING, "Failed to download package", path, e, "attempt", retryCount + 1, "of",
            retryLimit, "will retry");
      }
    }
  }

  co_return kj::none;  // All retry attempts failed
}

// Loads a single Python package, either from disk cache or by downloading it
kj::Promise<void> loadPyodidePackage(const api::pyodide::PythonConfig& pyConfig,
    const api::pyodide::PyodidePackageManager& pyodidePackageManager,
    kj::StringPtr packagesVersion,
    kj::StringPtr filename,
    kj::Network& network,
    kj::Timer& timer) {

  auto path = kj::str(packagesVersion, "/", filename);
  // First check if we already have this package in memory
  if (pyodidePackageManager.getPyodidePackage(path) != kj::none) {
    co_return;
  }

  // Then check disk cache
  KJ_IF_SOME(diskCachePath, pyConfig.packageDiskCacheRoot) {
    auto parsedPath = kj::Path::parse(filename);
    if (diskCachePath->exists(parsedPath)) {
      try {
        auto file = diskCachePath->openFile(parsedPath);
        auto blob = file->readAllBytes();

        // Decompress the package
        kj::ArrayInputStream ais(blob);
        kj::GzipInputStream gzip(ais);
        auto decompressed = gzip.readAllBytes();

        // Store in memory
        pyodidePackageManager.setPyodidePackageData(kj::str(path), kj::mv(decompressed));
        co_return;
      } catch (kj::Exception& e) {
        // Something went wrong while reading or processing the file
        KJ_LOG(WARNING, "Failed to read or process package from disk cache", path, e);
      }
    }
  }

  // Need to fetch from network
  kj::HttpHeaderTable table;
  kj::TlsContext::Options tlsOptions;
  tlsOptions.useSystemTrustStore = true;
  kj::Own<kj::TlsContext> tlsContext = kj::heap<kj::TlsContext>(kj::mv(tlsOptions));

  auto tlsNetwork = tlsContext->wrapNetwork(network);
  auto client = kj::newHttpClient(timer, table, network, *tlsNetwork);

  kj::String url = kj::str(api::pyodide::PYTHON_PACKAGES_URL, path);

  auto maybeBody = co_await downloadPackageWithRetry(*client, timer, table, url, path);
  KJ_IF_SOME(body, maybeBody) {
    // Successfully downloaded the package
    // Save the compressed data to disk cache (if enabled)
    KJ_IF_SOME(diskCachePath, pyConfig.packageDiskCacheRoot) {
      try {
        auto parsedPath = kj::Path::parse(path);
        auto file = diskCachePath->openFile(parsedPath,
            kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT);
        file->writeAll(body);
      } catch (kj::Exception& e) {
        KJ_LOG(WARNING, "Failed to write package to disk cache", e);
      }
    }

    // Now decompress and store in memory
    kj::ArrayInputStream ais(body);
    kj::GzipInputStream gzip(ais);
    auto decompressed = gzip.readAllBytes();

    pyodidePackageManager.setPyodidePackageData(kj::str(path), kj::mv(decompressed));
  } else {
    KJ_FAIL_ASSERT("Failed to download package after all retry attempts", path);
  }

  co_return;
}

kj::Promise<void> fetchPyodidePackages(const api::pyodide::PythonConfig& pyConfig,
    const api::pyodide::PyodidePackageManager& pyodidePackageManager,
    kj::ArrayPtr<kj::String> pythonRequirements,
    workerd::PythonSnapshotRelease::Reader pythonSnapshotRelease,
    kj::Network& network,
    kj::Timer& timer) {
  auto packagesVersion = pythonSnapshotRelease.getPackages();

  auto pyodideLock = api::pyodide::getPyodideLock(pythonSnapshotRelease);
  if (pyodideLock == kj::none) {
    KJ_LOG(WARNING, "No lock file found for Python packages version", packagesVersion);
    co_return;
  }

  auto filenames = api::pyodide::getPythonPackageFiles(
      KJ_ASSERT_NONNULL(pyodideLock), pythonRequirements, packagesVersion);

  kj::Vector<kj::Promise<void>> promises(filenames.size());
  for (const auto& filename: filenames) {
    promises.add(loadPyodidePackage(
        pyConfig, pyodidePackageManager, packagesVersion, filename, network, timer));
  }

  co_await kj::joinPromisesFailFast(promises.releaseAsArray());
}

}  // namespace workerd::server
