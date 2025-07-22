// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "pyodide.h"

#include "requirements.h"

#include <workerd/api/pyodide/setup-emscripten.h>
#include <workerd/util/strings.h>

#include <pyodide/generated/pyodide_extra.capnp.h>

#include <kj/array.h>
#include <kj/common.h>
#include <kj/compat/gzip.h>
#include <kj/compat/tls.h>
#include <kj/debug.h>
#include <kj/string.h>

// for std::sort
#include <algorithm>

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

kj::Promise<void> PyodideBundleManager::getOrCreateBundlePromise(kj::String version,
    kj::Function<kj::Promise<void>()> createPromise) const {
  auto locked = bundlePromises.lockExclusive();
  auto existing = locked->find(version);
  if (existing != kj::none) {
    // Return a new branch from the existing forked promise
    return KJ_REQUIRE_NONNULL(existing).addBranch();
  }

  // Create new promise and fork it for concurrent access
  auto promise = createPromise();
  auto forked = promise.fork();
  auto branch = forked.addBranch();

  // Store the forked promise for future requests
  locked->insert(kj::str(version), kj::mv(forked));

  return branch;
}

kj::Promise<void> PyodidePackageManager::getOrCreatePackagePromise(kj::String id,
    kj::Function<kj::Promise<void>()> createPromise) const {
  auto locked = packagePromises.lockExclusive();
  auto existing = locked->find(id);
  if (existing != kj::none) {
    // Return a new branch from the existing forked promise
    return KJ_REQUIRE_NONNULL(existing).addBranch();
  }

  // Create new promise and fork it for concurrent access
  auto promise = createPromise();
  auto forked = promise.fork();
  auto branch = forked.addBranch();

  // Store the forked promise for future requests
  locked->insert(kj::str(id), kj::mv(forked));

  return branch;
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

kj::Array<kj::StringPtr> PyodideMetadataReader::getNames(
    jsg::Lock& js, jsg::Optional<kj::String> maybeExtFilter) {
  auto builder = kj::Vector<kj::StringPtr>(state->moduleInfo.names.size());
  for (auto i: kj::zeroTo(builder.capacity())) {
    KJ_IF_SOME(ext, maybeExtFilter) {
      if (!state->moduleInfo.names[i].endsWith(ext)) {
        continue;
      }
    }
    builder.add(state->moduleInfo.names[i]);
  }
  return builder.releaseAsArray();
}

kj::Array<kj::String> PythonModuleInfo::getPythonFileContents() {
  auto builder = kj::Vector<kj::String>(names.size());
  for (auto i: kj::zeroTo(names.size())) {
    if (names[i].endsWith(".py")) {
      builder.add(kj::str(contents[i].asChars()));
    }
  }
  return builder.releaseAsArray();
}

kj::HashSet<kj::String> PythonModuleInfo::getWorkerModuleSet() {
  auto result = kj::HashSet<kj::String>();
  const auto vendor = "python_modules/"_kj;
  const auto dotPy = ".py"_kj;
  const auto dotSo = ".so"_kj;
  for (auto& item: names) {
    kj::StringPtr name = item;
    if (name.startsWith(vendor)) {
      name = name.slice(vendor.size());
    }
    auto firstSlash = name.findFirst('/');
    KJ_IF_SOME(idx, firstSlash) {
      result.upsert(kj::str(name.slice(0, idx)), [](auto&&, auto&&) {});
      continue;
    }
    if (name.endsWith(dotPy)) {
      result.upsert(kj::str(name.slice(0, name.size() - dotPy.size())), [](auto&&, auto&&) {});
      continue;
    }
    if (name.endsWith(dotSo)) {
      result.upsert(kj::str(name.slice(0, name.size() - dotSo.size())), [](auto&&, auto&&) {});
      continue;
    }
  }
  return result;
}

kj::Array<kj::String> PythonModuleInfo::getPackageSnapshotImports(kj::StringPtr version) {
  auto workerFiles = this->getPythonFileContents();
  auto importedNames = parsePythonScriptImports(kj::mv(workerFiles));
  auto workerModules = getWorkerModuleSet();
  return PythonModuleInfo::filterPythonScriptImports(
      kj::mv(workerModules), kj::mv(importedNames), version);
}

kj::Array<kj::String> PyodideMetadataReader::getPackageSnapshotImports(kj::String version) {
  return state->moduleInfo.getPackageSnapshotImports(version);
}

kj::Array<jsg::JsRef<jsg::JsString>> PyodideMetadataReader::getRequirements(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<jsg::JsRef<jsg::JsString>>(state->requirements.size());
  for (auto i: kj::zeroTo(builder.capacity())) {
    builder.add(js, js.str(state->requirements[i]));
  }
  return builder.finish();
}

kj::Array<int> PyodideMetadataReader::getSizes(jsg::Lock& js) {
  auto builder = kj::heapArrayBuilder<int>(state->moduleInfo.names.size());
  for (auto i: kj::zeroTo(builder.capacity())) {
    builder.add(state->moduleInfo.contents[i].size());
  }
  return builder.finish();
}

int PyodideMetadataReader::read(jsg::Lock& js, int index, int offset, kj::Array<kj::byte> buf) {
  if (index >= state->moduleInfo.contents.size() || index < 0) {
    return 0;
  }
  auto& data = state->moduleInfo.contents[index];
  return readToTarget(data, offset, buf);
}

int PyodideMetadataReader::readMemorySnapshot(int offset, kj::Array<kj::byte> buf) {
  if (state->memorySnapshot == kj::none) {
    return 0;
  }
  return readToTarget(KJ_REQUIRE_NONNULL(state->memorySnapshot), offset, buf);
}

kj::HashSet<kj::String> PyodideMetadataReader::getTransitiveRequirements() {
  auto packages = parseLockFile(state->packagesLock);
  auto depMap = getDepMapFromPackagesLock(*packages);

  return getPythonPackageNames(*packages, depMap, state->requirements, state->packagesVersion);
}

int ArtifactBundler::readMemorySnapshot(int offset, kj::Array<kj::byte> buf) {
  if (inner->existingSnapshot == kj::none) {
    return 0;
  }
  return readToTarget(KJ_REQUIRE_NONNULL(inner->existingSnapshot), offset, buf);
}

kj::Array<kj::String> PythonModuleInfo::parsePythonScriptImports(kj::Array<kj::String> files) {
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

const kj::Array<kj::StringPtr> snapshotImports = kj::arr("_pyodide"_kj,
    "_pyodide.docstring"_kj,
    "_pyodide._core_docs"_kj,
    "traceback"_kj,
    "collections.abc"_kj,
    // Asyncio is the really slow one here. In native Python on my machine, `import asyncio` takes ~50
    // ms.
    "asyncio"_kj,
    "inspect"_kj,
    "tarfile"_kj,
    "importlib",
    "importlib.metadata"_kj,
    "re"_kj,
    "shutil"_kj,
    "sysconfig"_kj,
    "importlib.machinery"_kj,
    "pathlib"_kj,
    "site"_kj,
    "tempfile"_kj,
    "typing"_kj,
    "zipfile"_kj);

kj::Array<kj::StringPtr> PyodideMetadataReader::getBaselineSnapshotImports() {
  return kj::heapArray(snapshotImports.begin(), snapshotImports.size());
}

PyodideMetadataReader::State::State(const State& other)
    : mainModule(kj::str(other.mainModule)),
      moduleInfo(other.moduleInfo.clone()),
      requirements(KJ_MAP(req, other.requirements) { return kj::str(req); }),
      pyodideVersion(kj::str(other.pyodideVersion)),
      packagesVersion(kj::str(other.packagesVersion)),
      packagesLock(kj::str(other.packagesLock)),
      isWorkerdFlag(other.isWorkerdFlag),
      isTracingFlag(other.isTracingFlag),
      snapshotToDisk(other.snapshotToDisk),
      createBaselineSnapshot(other.createBaselineSnapshot),
      memorySnapshot(other.memorySnapshot.map(
          [](auto& snapshot) { return kj::heapArray<kj::byte>(snapshot); })) {}

kj::Own<PyodideMetadataReader::State> PyodideMetadataReader::State::clone() {
  return kj::heap<PyodideMetadataReader::State>(*this);
}

void PyodideMetadataReader::State::verifyNoMainModuleInVendor() {
  // Verify that we don't have module named after the main module in the `python_modules` subdir.
  // mainModule includes the .py extension, so we need to extract the base name
  kj::ArrayPtr<const char> mainModuleBase = mainModule;
  if (mainModule.endsWith(".py")) {
    mainModuleBase = mainModuleBase.slice(0, mainModuleBase.size() - 3);
  }

  for (auto& name: moduleInfo.names) {
    if (name.startsWith(kj::str("python_modules/", mainModule))) {
      JSG_FAIL_REQUIRE(
          Error, kj::str("Python module python_modules/", mainModule, " clashes with main module"));
    }
    if (name == kj::str("python_modules/", mainModuleBase, "/__init__.py")) {
      JSG_FAIL_REQUIRE(Error,
          kj::str("Python module python_modules/", mainModuleBase,
              "/__init__.py clashes with main module"));
    }
    if (name == kj::str("python_modules/", mainModuleBase, ".so")) {
      JSG_FAIL_REQUIRE(Error,
          kj::str("Python module python_modules/", mainModuleBase, ".so clashes with main module"));
    }
  }
}

kj::Array<kj::String> PythonModuleInfo::filterPythonScriptImports(
    kj::HashSet<kj::String> workerModules,
    kj::ArrayPtr<kj::String> imports,
    kj::StringPtr version) {
  auto baselineSnapshotImportsSet = kj::HashSet<kj::StringPtr>();
  for (auto& pkgImport: snapshotImports) {
    baselineSnapshotImportsSet.upsert(kj::mv(pkgImport), [](auto&&, auto&&) {});
  }

  kj::HashSet<kj::String> filteredImportsSet;
  filteredImportsSet.reserve(imports.size());
  for (auto& pkgImport: imports) {
    auto firstDot = pkgImport.findFirst('.').orDefault(pkgImport.size());
    auto firstComponent = pkgImport.slice(0, firstDot);
    // Skip duplicates
    if (filteredImportsSet.contains(pkgImport)) [[unlikely]] {
      continue;
    }

    // don't include modules that we provide and that are likely to be imported by most
    // workers.
    if (firstComponent == "js"_kj.asArray() || firstComponent == "asgi"_kj.asArray() ||
        firstComponent == "workers"_kj.asArray()) {
      continue;
    }
    if (version == "0.26.0a2") {
      if (firstComponent == "pyodide"_kj.asArray() || firstComponent == "httpx"_kj.asArray() ||
          firstComponent == "openai"_kj.asArray() || firstComponent == "starlette"_kj.asArray() ||
          firstComponent == "urllib3"_kj.asArray()) {
        continue;
      }
    }

    // Don't include anything that went into the baseline snapshot
    if (baselineSnapshotImportsSet.contains(pkgImport)) {
      continue;
    }

    // Don't include imports from worker files
    if (workerModules.contains(firstComponent)) {
      continue;
    }
    filteredImportsSet.upsert(kj::mv(pkgImport), [](auto&&, auto&&) {});
  }

  auto filteredImportsBuilder = kj::heapArrayBuilder<kj::String>(filteredImportsSet.size());
  for (auto& pkgImport: filteredImportsSet) {
    filteredImportsBuilder.add(kj::mv(pkgImport));
  }
  return filteredImportsBuilder.finish();
}

kj::Maybe<kj::String> getPyodideLock(PythonSnapshotRelease::Reader pythonSnapshotRelease) {
  for (auto pkgLock: *PACKAGE_LOCKS) {
    if (pkgLock.getPackageDate() == pythonSnapshotRelease.getPackages()) {
      return kj::str(pkgLock.getLock());
    }
  }

  return kj::none;
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

// TODO: DiskCache is currently only used for --python-save-snapshot. Can we use ArtifactBundler for
// this instead and remove DiskCache completely?
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
  return emscriptenRuntime.emscriptenRuntime.getHandle(js);
}

void SetupEmscripten::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(emscriptenRuntime.emscriptenRuntime);
}

}  // namespace workerd::api::pyodide

#include "workerd/io/compatibility-date.h"

#include <capnp/dynamic.h>
#include <capnp/schema.h>

namespace workerd {

struct PythonSnapshotParsedField {
  PythonSnapshotRelease::Reader pythonSnapshotRelease;
  capnp::StructSchema::Field field;
};

kj::Array<const PythonSnapshotParsedField> makePythonSnapshotFieldTable(
    capnp::StructSchema::FieldList fields) {
  kj::Vector<PythonSnapshotParsedField> table(fields.size());

  for (auto field: fields) {
    bool isPythonField = false;

    for (auto annotation: field.getProto().getAnnotations()) {
      if (annotation.getId() == PYTHON_SNAPSHOT_RELEASE_ANNOTATION_ID) {
        isPythonField = true;
        break;
      }
    }
    if (!isPythonField) {
      continue;
    }

    auto name = field.getProto().getName();
    kj::Maybe<PythonSnapshotRelease::Reader> pythonSnapshotRelease;
    for (auto release: *RELEASES) {
      if (release.getFlagName() == name) {
        pythonSnapshotRelease = release;
        break;
      }
    }
    table.add(PythonSnapshotParsedField{
      .pythonSnapshotRelease = KJ_REQUIRE_NONNULL(pythonSnapshotRelease),
      .field = field,
    });
  }

  return table.releaseAsArray();
}

kj::Maybe<PythonSnapshotRelease::Reader> getPythonSnapshotRelease(
    CompatibilityFlags::Reader featureFlags) {
  uint latestFieldOrdinal = 0;
  kj::Maybe<PythonSnapshotRelease::Reader> result;

  static const auto fieldTable =
      makePythonSnapshotFieldTable(capnp::Schema::from<CompatibilityFlags>().getFields());

  for (auto field: fieldTable) {
    bool isEnabled = capnp::toDynamic(featureFlags).get(field.field).as<bool>();
    if (!isEnabled) {
      continue;
    }

    // We pick the flag with the highest ordinal value that is enabled and has a
    // pythonSnapshotRelease annotation.
    //
    // The fieldTable is probably ordered by the ordinal anyway, but doesn't hurt to be explicit
    // here.
    if (latestFieldOrdinal < field.field.getIndex()) {
      latestFieldOrdinal = field.field.getIndex();
      result = field.pythonSnapshotRelease;
    }
  }

  return result;
}

kj::String getPythonBundleName(PythonSnapshotRelease::Reader pyodideRelease) {
  if (pyodideRelease.getPyodide() == "dev") {
    return kj::str("dev");
  }
  return kj::str(pyodideRelease.getPyodide(), "_", pyodideRelease.getPyodideRevision(), "_",
      pyodideRelease.getBackport());
}

namespace api::pyodide {

// Returns a string containing the contents of the hashset, delimited by ", "
kj::String hashsetToString(const kj::HashSet<kj::String>& set) {
  if (set.size() == 0) {
    return kj::String();
  }

  kj::Vector<kj::StringPtr> elems;
  for (const auto& e: set) {
    elems.add(e);
  }

  // Sort the elements for consistent output
  auto array = elems.releaseAsArray();
  std::sort(array.begin(), array.end());

  return kj::str(kj::delimited(array, ", "_kjc));
}

kj::Array<kj::String> getPythonPackageFiles(kj::StringPtr lockFileContents,
    kj::ArrayPtr<kj::String> requirements,
    kj::StringPtr packagesVersion) {
  auto packages = parseLockFile(lockFileContents);
  auto depMap = getDepMapFromPackagesLock(*packages);

  auto allRequirements = getPythonPackageNames(*packages, depMap, requirements, packagesVersion);

  // Add the file names of all the requirements to our result array.
  kj::Vector<kj::String> res;
  for (const auto& ent: *packages) {
    auto name = ent.getName();
    auto obj = ent.getValue().getObject();
    auto fileName = kj::str(getField(obj, "file_name").getString());

    auto maybeRow = allRequirements.find(name);
    KJ_IF_SOME(row, maybeRow) {
      allRequirements.erase(row);
      res.add(kj::mv(fileName));
    } else if (packagesVersion == "20240829.4") {
      auto packageType = getField(obj, "package_type").getString();
      if (packageType == "cpython_module") {
        res.add(kj::mv(fileName));
      }
    }
  }

  if (allRequirements.size() != 0) {
    JSG_FAIL_REQUIRE(Error,
        "Requested Python package(s) that are not supported: ", hashsetToString(allRequirements));
  }

  return res.releaseAsArray();
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
kj::Promise<void> loadPyodidePackage(const PythonConfig& pyConfig,
    const PyodidePackageManager& pyodidePackageManager,
    kj::StringPtr packagesVersion,
    kj::StringPtr filename,
    kj::Network& network,
    kj::Timer& timer) {

  auto path = kj::str("python-package-bucket/", packagesVersion, "/", filename);
  // First check if we already have this package in memory
  if (pyodidePackageManager.getPyodidePackage(path) != kj::none) {
    co_return;
  }

  // Use ForkedPromise to handle concurrent requests for the same package
  co_await pyodidePackageManager.getOrCreatePackagePromise(kj::str(path),
      [&pyConfig, &pyodidePackageManager, path = kj::str(path), &network, &timer]() -> kj::Promise<void> {
        // Check if another concurrent request already loaded it
        if (pyodidePackageManager.getPyodidePackage(path) != kj::none) {
          co_return;
        }

        // Then check disk cache
        KJ_IF_SOME(diskCachePath, pyConfig.packageDiskCacheRoot) {
          auto parsedPath = kj::Path::parse(path);

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

        kj::String url = kj::str(PYTHON_PACKAGES_URL, path);

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
      });

  co_return;
}

kj::Promise<void> fetchPyodidePackages(const PythonConfig& pyConfig,
    const PyodidePackageManager& pyodidePackageManager,
    kj::ArrayPtr<kj::String> pythonRequirements,
    workerd::PythonSnapshotRelease::Reader pythonSnapshotRelease,
    kj::Network& network,
    kj::Timer& timer) {
  auto packagesVersion = pythonSnapshotRelease.getPackages();

  auto pyodideLock = getPyodideLock(pythonSnapshotRelease);
  if (pyodideLock == kj::none) {
    KJ_LOG(WARNING, "No lock file found for Python packages version", packagesVersion);
    co_return;
  }

  auto filenames =
      getPythonPackageFiles(KJ_ASSERT_NONNULL(pyodideLock), pythonRequirements, packagesVersion);

  kj::Vector<kj::Promise<void>> promises;
  for (const auto& filename: filenames) {
    promises.add(loadPyodidePackage(
        pyConfig, pyodidePackageManager, packagesVersion, filename, network, timer));
  }

  co_await kj::joinPromisesFailFast(promises.releaseAsArray());
}

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
    const PythonConfig& pyConfig, kj::String version, kj::Network& network, kj::Timer& timer) {
  // First check if bundle is already available
  if (pyConfig.pyodideBundleManager.getPyodideBundle(version) != kj::none) {
    co_return pyConfig.pyodideBundleManager.getPyodideBundle(version);
  }

  // Use ForkedPromise to handle concurrent requests for the same bundle
  co_await pyConfig.pyodideBundleManager.getOrCreateBundlePromise(kj::str(version),
      [&pyConfig, version = kj::str(version), &network, &timer]() -> kj::Promise<void> {
        // Check if another concurrent request already loaded it
        if (pyConfig.pyodideBundleManager.getPyodideBundle(version) != kj::none) {
          co_return;
        }

        auto maybePyodideBundleFile = getPyodideBundleFile(pyConfig.pyodideDiskCacheRoot, version);
        KJ_IF_SOME(pyodideBundleFile, maybePyodideBundleFile) {
          auto body = pyodideBundleFile->readAllBytes();
          pyConfig.pyodideBundleManager.setPyodideBundleData(kj::str(version), kj::mv(body));
          co_return;
        }

        if (version == "dev") {
          // the "dev" version is special and indicates we're using the tip-of-tree version built for testing
          // so we shouldn't fetch it from the internet, only check for its existence in the disk cache
          co_return;
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
        KJ_ASSERT(res.statusCode == 200,
            kj::str("Request for Pyodide bundle at ", url, " failed with HTTP status ", res.statusCode));
        auto body = co_await res.body->readAllBytes();

        writePyodideBundleFileToDisk(pyConfig.pyodideDiskCacheRoot, version, body);

        pyConfig.pyodideBundleManager.setPyodideBundleData(kj::str(version), kj::mv(body));
      });

  co_return pyConfig.pyodideBundleManager.getPyodideBundle(version);
}

}  // namespace api::pyodide

}  // namespace workerd
