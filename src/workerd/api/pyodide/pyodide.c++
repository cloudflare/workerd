// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "pyodide.h"

#include <workerd/api/pyodide/setup-emscripten.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/util/string-buffer.h>
#include <workerd/util/strings.h>

#include <pyodide/generated/pyodide_extra.capnp.h>

#include <kj/array.h>
#include <kj/common.h>
#include <kj/debug.h>
#include <kj/string.h>

namespace workerd::api::pyodide {

// singleton that owns bundle

const kj::Maybe<jsg::Bundle::Reader> PyodideBundleManager::getPyodideBundle(
    kj::StringPtr version) const {
  return bundles.lockShared()->find(version).map(
      [](const MessageBundlePair& t) { return t.bundle; });
}

void PyodideBundleManager::setPyodideBundleData(
    kj::String version, kj::Array<unsigned char> data) const {
  auto wordArray = kj::arrayPtr(
      reinterpret_cast<const capnp::word*>(data.begin()), data.size() / sizeof(capnp::word));
  // We're going to reuse this in the ModuleRegistry for every Python isolate, so set the traversal
  // limit to infinity or else eventually a new Python isolate will fail.
  auto messageReader = kj::heap<capnp::FlatArrayMessageReader>(
      wordArray, capnp::ReaderOptions{.traversalLimitInWords = kj::maxValue})
                           .attach(kj::mv(data));
  auto bundle = messageReader->getRoot<jsg::Bundle>();
  bundles.lockExclusive()->insert(
      kj::mv(version), {.messageReader = kj::mv(messageReader), .bundle = bundle});
}

const kj::Maybe<const kj::Array<unsigned char>&> PyodidePackageManager::getPyodidePackage(
    kj::StringPtr id) const {
  return packages.lockShared()->find(id);
}

void PyodidePackageManager::setPyodidePackageData(
    kj::String id, kj::Array<unsigned char> data) const {
  packages.lockExclusive()->insert(kj::mv(id), kj::mv(data));
}

static int readToTarget(
    kj::ArrayPtr<const kj::byte> source, int offset, kj::ArrayPtr<kj::byte> buf) {
  int size = source.size();
  if (offset >= size || offset < 0) {
    return 0;
  }
  int toCopy = buf.size();
  if (size - offset < toCopy) {
    toCopy = size - offset;
  }
  memcpy(buf.begin(), source.begin() + offset, toCopy);
  return toCopy;
}

int ReadOnlyBuffer::read(jsg::Lock& js, int offset, kj::Array<kj::byte> buf) {
  return readToTarget(source, offset, buf);
}

kj::Array<jsg::JsRef<jsg::JsString>> PyodideMetadataReader::getNames(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<jsg::JsRef<jsg::JsString>>(this->names.size());
  for (auto i: kj::zeroTo(builder.capacity())) {
    builder.add(js, js.str(this->names[i]));
  }
  return builder.finish();
}

kj::Array<jsg::JsRef<jsg::JsString>> PyodideMetadataReader::getWorkerFiles(
    jsg::Lock& js, kj::String ext) {
  auto builder = kj::Vector<jsg::JsRef<jsg::JsString>>(this->names.size());
  for (auto i: kj::zeroTo(this->names.size())) {
    if (this->names[i].endsWith(ext)) {
      builder.add(js, js.str(this->contents[i]));
    }
  }
  return builder.releaseAsArray();
}

kj::Array<jsg::JsRef<jsg::JsString>> PyodideMetadataReader::getRequirements(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<jsg::JsRef<jsg::JsString>>(this->requirements.size());
  for (auto i: kj::zeroTo(builder.capacity())) {
    builder.add(js, js.str(this->requirements[i]));
  }
  return builder.finish();
}

kj::Array<int> PyodideMetadataReader::getSizes(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<int>(this->names.size());
  for (auto i: kj::zeroTo(builder.capacity())) {
    builder.add(this->contents[i].size());
  }
  return builder.finish();
}

int PyodideMetadataReader::read(jsg::Lock& js, int index, int offset, kj::Array<kj::byte> buf) {
  if (index >= contents.size() || index < 0) {
    return 0;
  }
  auto& data = contents[index];
  return readToTarget(data, offset, buf);
}

int PyodideMetadataReader::readMemorySnapshot(int offset, kj::Array<kj::byte> buf) {
  if (memorySnapshot == kj::none) {
    return 0;
  }
  return readToTarget(KJ_REQUIRE_NONNULL(memorySnapshot), offset, buf);
}

int ArtifactBundler::readMemorySnapshot(int offset, kj::Array<kj::byte> buf) {
  if (existingSnapshot == kj::none) {
    return 0;
  }
  return readToTarget(KJ_REQUIRE_NONNULL(existingSnapshot), offset, buf);
}

kj::Array<kj::String> ArtifactBundler::parsePythonScriptImports(kj::Array<kj::String> files) {
  auto result = kj::Vector<kj::String>();

  for (auto& file: files) {
    // Returns the number of characters skipped. When `oneOf` is not found, skips to the end of
    // the string.
    auto skipUntil = [](kj::StringPtr str, std::initializer_list<char> oneOf, int start) -> int {
      int result = 0;
      while (start + result < str.size()) {
        char c = str[start + result];
        for (char expected: oneOf) {
          if (c == expected) {
            return result;
          }
        }

        result++;
      }

      return result;
    };

    // Skips while current character is in `oneOf`. Returns the number of characters skipped.
    auto skipWhile = [](kj::StringPtr str, std::initializer_list<char> oneOf, int start) -> int {
      int result = 0;
      while (start + result < str.size()) {
        char c = str[start + result];
        bool found = false;
        for (char expected: oneOf) {
          if (c == expected) {
            result++;
            found = true;
            break;
          }
        }

        if (!found) {
          break;
        }
      }

      return result;
    };

    // Skips one of the characters (specified in `oneOf`) at the current position. Otherwise
    // throws. Returns the number of characters skipped.
    auto skipChar = [](kj::StringPtr str, std::initializer_list<char> oneOf, int start) -> int {
      for (char expected: oneOf) {
        if (str[start] == expected) {
          return 1;
        }
      }

      KJ_FAIL_REQUIRE("Expected ", oneOf, "but received", str[start]);
    };

    auto parseKeyword = [](kj::StringPtr str, kj::StringPtr ident, int start) -> bool {
      int i = 0;
      for (; i < ident.size() && start + i < str.size(); i++) {
        if (str[start + i] != ident[i]) {
          return false;
        }
      }

      return i == ident.size();
    };

    // Returns the size of the import identifier or 0 if no identifier exists at `start`.
    auto parseIdent = [](kj::StringPtr str, int start) -> int {
      // https://docs.python.org/3/reference/lexical_analysis.html#identifiers
      //
      // We also accept `.` because import idents can contain it.
      // TODO: We don't currently support unicode, but if we see packages that utilize it we will
      // implement that support.
      if (isDigit(str[start])) {
        return 0;
      }
      int i = 0;
      for (; start + i < str.size(); i++) {
        char c = str[start + i];
        bool validIdentChar = isAlpha(c) || isDigit(c) || c == '_' || c == '.';
        if (!validIdentChar) {
          return i;
        }
      }

      return i;
    };

    int i = 0;
    while (i < file.size()) {
      switch (file[i]) {
        case 'i':
        case 'f': {
          auto keywordToParse = file[i] == 'i' ? "import"_kj : "from"_kj;
          if (!parseKeyword(file, keywordToParse, i)) {
            // We cannot simply skip the current char here, doing so would mean that
            // `iimport x` would be parsed as a valid import.
            i += skipUntil(file, {'\n', '\r', '"', '\''}, i);
            continue;
          }
          i += keywordToParse.size();  // skip "import" or "from"

          while (i < file.size()) {
            // Python expects a `\` to be paired with a newline, but we don't have to be as strict
            // here because we rely on the fact that the script has gone through validation already.
            i += skipWhile(
                file, {'\r', '\n', ' ', '\t', '\\'}, i);  // skip whitespace and backslash.

            if (file[i] == '.') {
              // ignore relative imports
              break;
            }

            int identLen = parseIdent(file, i);
            KJ_REQUIRE(identLen > 0);

            kj::String ident = kj::heapString(file.slice(i, i + identLen));
            if (ident[identLen - 1] != '.') {  // trailing period means the import is invalid
              result.add(kj::mv(ident));
            }

            i += identLen;

            // If "import" statement then look for comma.
            if (keywordToParse == "import") {
              i += skipWhile(
                  file, {'\r', '\n', ' ', '\t', '\\'}, i);  // skip whitespace and backslash.
              // Check if next char is a comma.
              if (file[i] == ',') {
                i += 1;  // Skip comma.
                // Allow while loop to continue
              } else {
                // No more idents, so break out of loop.
                break;
              }
            } else {
              // The "from" statement doesn't support commas.
              break;
            }
          }
          break;
        }
        case '"':
        case '\'': {
          char quote = file[i];
          // Detect multi-line string literals `"""` and skip until the corresponding ending `"""`.
          if (i + 2 < file.size() && file[i + 1] == quote && file[i + 2] == quote) {
            i += 3;  // skip start quotes.
            // skip until terminating quotes.
            while (i + 2 < file.size() && file[i + 1] != quote && file[i + 2] != quote) {
              if (file[i] == quote) {
                i++;
              }
              i += skipUntil(file, {quote}, i);
            }
            i += 3;  // skip terminating quotes.
          } else if (i + 2 < file.size() && file[i + 1] == '\\' &&
              (file[i + 2] == '\n' || file[i + 2] == '\r')) {
            // Detect string literal with backslash.
            i += 3;  // skip `"\<NL>`
            // skip until quote, but ignore `\"`.
            while (file[i] != quote && file[i - 1] != '\\') {
              if (file[i] == quote) {
                i++;
              }
              i += skipUntil(file, {quote}, i);
            }
            i += 1;  // skip quote.
          } else {
            i += 1;  // skip quote.
          }

          // skip until EOL so that we don't mistakenly parse and capture `"import x`.
          i += skipUntil(file, {'\n', '\r', '"', '\''}, i);
          break;
        }
        default:
          // Skip to the next line or " or '
          i += skipUntil(file, {'\n', '\r', '"', '\''}, i);
          if (file[i] == '"' || file[i] == '\'') {
            continue;  // Allow the quotes to be handled above.
          }
          if (file[i] != '\0') {
            i += skipChar(file, {'\n', '\r'}, i);  // skip newline.
          }
      }
    }
  }

  return result.releaseAsArray();
}

// This is equivalent to `pkgImport.replace('.', '/') + ".py"`.
kj::String importToModuleFilename(kj::StringPtr pkgImport) {
  auto result = kj::heapString(pkgImport.size() + 3);
  for (auto i = 0; i < pkgImport.size(); i++) {
    if (pkgImport[i] == '.') {
      result[i] = '/';
    } else {
      result[i] = pkgImport[i];
    }
  }

  result[pkgImport.size()] = '.';
  result[pkgImport.size() + 1] = 'p';
  result[pkgImport.size() + 2] = 'y';
  return result;
}

kj::Array<kj::String> ArtifactBundler::filterPythonScriptImports(
    kj::HashSet<kj::String> workerModules, kj::Array<kj::String> imports) {
  auto baselineSnapshotImportsSet = kj::HashSet<kj::StringPtr>();
  for (auto& pkgImport: ArtifactBundler::getSnapshotImports()) {
    baselineSnapshotImportsSet.insert(kj::mv(pkgImport));
  }

  kj::HashSet<kj::String> filteredImportsSet;
  filteredImportsSet.reserve(imports.size());
  for (auto& pkgImport: imports) {
    // Skip duplicates
    if (filteredImportsSet.contains(pkgImport)) [[unlikely]] {
      continue;
    }

    // don't include js or pyodide.
    if (pkgImport == "js" || pkgImport == "pyodide") {
      continue;
    }

    // Don't include anything that went into the baseline snapshot
    if (baselineSnapshotImportsSet.contains(pkgImport)) {
      continue;
    }

    // Don't include imports from worker files
    if (workerModules.contains(importToModuleFilename(pkgImport))) {
      continue;
    }
    filteredImportsSet.insert(kj::mv(pkgImport));
  }

  auto filteredImportsBuilder = kj::heapArrayBuilder<kj::String>(filteredImportsSet.size());
  for (auto& pkgImport: filteredImportsSet) {
    filteredImportsBuilder.add(kj::mv(pkgImport));
  }
  return filteredImportsBuilder.finish();
}

kj::Array<kj::String> ArtifactBundler::filterPythonScriptImportsJs(
    kj::Array<kj::String> locals, kj::Array<kj::String> imports) {
  kj::HashSet<kj::String> localsSet;
  for (auto& local: locals) {
    localsSet.insert(kj::mv(local));
  }
  return ArtifactBundler::filterPythonScriptImports(kj::mv(localsSet), kj::mv(imports));
}

kj::Array<kj::StringPtr> ArtifactBundler::getSnapshotImports() {
  kj::StringPtr imports[] = {"_pyodide.docstring"_kj, "_pyodide._core_docs"_kj, "traceback"_kj,
    "collections.abc"_kj,
    // Asyncio is the really slow one here. In native Python on my machine, `import asyncio` takes ~50
    // ms.
    "asyncio"_kj, "inspect"_kj, "tarfile"_kj, "importlib.metadata"_kj, "re"_kj, "shutil"_kj,
    "sysconfig"_kj, "importlib.machinery"_kj, "pathlib"_kj, "site"_kj, "tempfile"_kj, "typing"_kj,
    "zipfile"_kj};
  kj::Vector<kj::StringPtr> result;
  for (auto pkgImport: imports) {
    result.add(pkgImport);
  }
  return result.releaseAsArray();
}

kj::Maybe<kj::String> getPyodideLock(PythonSnapshotRelease::Reader pythonSnapshotRelease) {
  for (auto pkgLock: *PACKAGE_LOCKS) {
    if (pkgLock.getPackageDate() == pythonSnapshotRelease.getPackages()) {
      return kj::str(pkgLock.getLock());
    }
  }

  return kj::none;
}

jsg::Ref<PyodideMetadataReader> makePyodideMetadataReader(Worker::Reader conf,
    const PythonConfig& pythonConfig,
    PythonSnapshotRelease::Reader pythonRelease) {
  auto modules = conf.getModules();
  auto mainModule = kj::str(modules.begin()->getName());
  int numFiles = 0;
  int numRequirements = 0;
  for (auto module: modules) {
    switch (module.which()) {
      case Worker::Module::TEXT:
      case Worker::Module::DATA:
      case Worker::Module::JSON:
      case Worker::Module::PYTHON_MODULE:
        numFiles++;
        break;
      case Worker::Module::PYTHON_REQUIREMENT:
        numRequirements++;
        break;
      default:
        break;
    }
  }

  auto names = kj::heapArrayBuilder<kj::String>(numFiles);
  auto contents = kj::heapArrayBuilder<kj::Array<kj::byte>>(numFiles);
  auto requirements = kj::heapArrayBuilder<kj::String>(numRequirements);
  for (auto module: modules) {
    switch (module.which()) {
      case Worker::Module::TEXT:
        contents.add(kj::heapArray(module.getText().asBytes()));
        break;
      case Worker::Module::DATA:
        contents.add(kj::heapArray(module.getData().asBytes()));
        break;
      case Worker::Module::JSON:
        contents.add(kj::heapArray(module.getJson().asBytes()));
        break;
      case Worker::Module::PYTHON_MODULE:
        KJ_REQUIRE(module.getName().endsWith(".py"));
        contents.add(kj::heapArray(module.getPythonModule().asBytes()));
        break;
      case Worker::Module::PYTHON_REQUIREMENT:
        requirements.add(kj::str(module.getName()));
        continue;
      default:
        continue;
    }
    names.add(kj::str(module.getName()));
  }
  bool snapshotToDisk = pythonConfig.createSnapshot || pythonConfig.createBaselineSnapshot;
  if (pythonConfig.loadSnapshotFromDisk && snapshotToDisk) {
    KJ_FAIL_ASSERT(
        "Doesn't make sense to pass both --python-save-snapshot and --python-load-snapshot");
  }
  kj::Maybe<kj::Array<kj::byte>> memorySnapshot = kj::none;
  if (pythonConfig.loadSnapshotFromDisk) {
    auto& root = KJ_REQUIRE_NONNULL(pythonConfig.packageDiskCacheRoot);
    kj::Path path("snapshot.bin");
    auto maybeFile = root->tryOpenFile(path);
    if (maybeFile == kj::none) {
      KJ_FAIL_REQUIRE("Expected to find snapshot.bin in the package cache directory");
    }
    memorySnapshot = KJ_REQUIRE_NONNULL(maybeFile)->readAllBytes();
  }
  auto lock = KJ_ASSERT_NONNULL(getPyodideLock(pythonRelease),
      kj::str("No lock file defined for Python packages release ", pythonRelease.getPackages()));

  // clang-format off
  return jsg::alloc<PyodideMetadataReader>(
    kj::mv(mainModule),
    names.finish(),
    contents.finish(),
    requirements.finish(),
    kj::str(pythonRelease.getPackages()),
    kj::mv(lock),
    true      /* isWorkerd */,
    false     /* isTracing */,
    snapshotToDisk,
    pythonConfig.createBaselineSnapshot,
    false,    /* usePackagesInArtifactBundler */
    kj::mv(memorySnapshot)
  );
  // clang-format on
}

const kj::Maybe<kj::Own<const kj::Directory>> DiskCache::NULL_CACHE_ROOT = kj::none;

jsg::Optional<kj::Array<kj::byte>> DiskCache::get(jsg::Lock& js, kj::String key) {
  KJ_IF_SOME(root, cacheRoot) {
    kj::Path path(key);
    auto file = root->tryOpenFile(path);

    KJ_IF_SOME(f, file) {
      return f->readAllBytes();
    } else {
      return kj::none;
    }
  } else {
    return kj::none;
  }
}

void DiskCache::put(jsg::Lock& js, kj::String key, kj::Array<kj::byte> data) {
  KJ_IF_SOME(root, cacheRoot) {
    kj::Path path(key);
    auto file = root->tryOpenFile(path, kj::WriteMode::CREATE | kj::WriteMode::MODIFY);

    KJ_IF_SOME(f, file) {
      f->writeAll(data);
    } else {
      KJ_LOG(ERROR, "DiskCache: Failed to open file", key);
    }
  } else {
    return;
  }
}

jsg::JsValue SetupEmscripten::getModule(jsg::Lock& js) {
  js.installJspi();
  js.v8Context()->SetSecurityToken(emscriptenRuntime.contextToken.getHandle(js));
  return emscriptenRuntime.emscriptenRuntime.getHandle(js);
}

void SetupEmscripten::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(const_cast<EmscriptenRuntime&>(emscriptenRuntime).emscriptenRuntime);
}

bool hasPythonModules(capnp::List<server::config::Worker::Module>::Reader modules) {
  for (auto module: modules) {
    if (module.isPythonModule()) {
      return true;
    }
  }
  return false;
}

}  // namespace workerd::api::pyodide
