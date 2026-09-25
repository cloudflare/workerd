// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "config-compiler.h"

#include "schema-file.h"

#include <workerd/server/cli/bridge.rs.h>
#include <workerd/server/workerd.capnp.h>

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/filesystem.h>

using namespace kj_rs;

namespace workerd::server::cli {
namespace {

// Fails compilation with a message for the user; reported as the bridge call's error.
#define CONFIG_ERROR(...) KJ_FAIL_REQUIRE(kj::str(__VA_ARGS__))

class ConfigCompiler final: public SchemaFileImpl::ErrorReporter {
 public:
  explicit ConfigCompiler(const Process& process): process(process) {
    // We don't want to force people to specify top-level file IDs in `workerd` config files, as
    // those IDs would be totally irrelevant.
    schemaParser.setFileIdsRequired(false);
  }

  // The command line checked that the directory exists.
  void addImportPath(kj::StringPtr pathStr) {
    importPath.add(fs->getCurrentPath().evalNative(pathStr));
  }

  void parseConfigFile(kj::StringPtr pathStr) {
    auto path = fs->getCurrentPath().evalNative(pathStr);
    auto file = KJ_UNWRAP_OR(fs->getRoot().tryOpenFile(path), CONFIG_ERROR("No such file."));

    // Use stat() to check that we have a file vs a directory which will fail to mmap
    auto metadata = file->stat();
    if (metadata.type != kj::FsNode::Type::FILE) {
      CONFIG_ERROR("Config path is not a file.");
    }

    schemaParser.loadCompiledTypeAndDependencies<config::Config>();

    parsedSchema = schemaParser.parseFile(kj::heap<SchemaFileImpl>(fs->getRoot(),
        fs->getCurrentPath(), kj::mv(path), nullptr, importPath, kj::mv(file), *this));

    // Construct a list of top-level constants of type `Config`. If there is exactly one,
    // we can use it by default.
    for (auto nested: parsedSchema.getAllNested()) {
      if (nested.getProto().isConst()) {
        auto constSchema = nested.asConst();
        auto type = constSchema.getType();
        if (type.isStruct() &&
            type.asStruct().getProto().getId() == capnp::typeId<config::Config>()) {
          topLevelConfigConstants.add(constSchema);
        }
      }
    }
  }

  void setConstName(kj::StringPtr name) {
    auto parent = parsedSchema;

    for (;;) {
      auto dotPos = KJ_UNWRAP_OR(name.findFirst('.'), break);
      auto parentName = name.first(dotPos);
      parent = KJ_UNWRAP_OR(parent.findNested(kj::str(parentName)),
          CONFIG_ERROR(name, ": No such constant is defined in the config file (the parent scope '",
              parentName, "' does not exist)."));
      name = name.slice(dotPos + 1);
    }

    auto node = KJ_UNWRAP_OR(parsedSchema.findNested(name),
        CONFIG_ERROR(name, ": No such constant is defined in the config file."));

    if (!node.getProto().isConst()) {
      CONFIG_ERROR(name, ": Symbol is not a constant.");
    }

    auto constSchema = node.asConst();
    auto type = constSchema.getType();
    if (!type.isStruct() || type.asStruct().getProto().getId() != capnp::typeId<config::Config>()) {
      CONFIG_ERROR(name, ": Constant is not of type 'Config'.");
    }

    config = constSchema.as<config::Config>();
  }

  // The chosen config as an encoded message, or nothing if there were parse errors.
  ::rust::Vec<uint64_t> encodeConfig() {
    if (!errors.empty()) return {};
    auto reader = getConfig();
    capnp::MallocMessageBuilder builder(reader.totalSize().wordCount + 2);
    builder.setRoot(reader);
    auto words = capnp::messageToFlatArray(builder);
    return kj::arrayPtr(reinterpret_cast<const uint64_t*>(words.begin()), words.size())
        .as<RustCopy>();
  }

  ::rust::Vec<ConfigParseError> errors;

 private:
  // Registers the files the config depends on for --watch. Borrowed for this compilation only.
  const Process& process;

  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  kj::Vector<kj::Path> importPath;
  capnp::SchemaParser schemaParser;
  capnp::ParsedSchema parsedSchema;
  kj::Vector<capnp::ConstSchema> topLevelConfigConstants;
  kj::Maybe<config::Config::Reader> config;

  config::Config::Reader getConfig() {
    KJ_IF_SOME(c, config) {
      return c;
    }
    // No `<const-name>` was given. See if we can infer the correct constant...
    if (topLevelConfigConstants.empty()) {
      CONFIG_ERROR("The config file does not define any top-level constants of type 'Config'.");
    } else if (topLevelConfigConstants.size() == 1) {
      return config.emplace(topLevelConfigConstants[0].as<config::Config>());
    } else {
      auto names = KJ_MAP(cnst, topLevelConfigConstants) { return cnst.getShortDisplayName(); };
      CONFIG_ERROR(
          "The config file defines multiple top-level constants of type 'Config', so you must "
          "specify which one to use with <const-name>. The options are: ",
          kj::strArray(names, ", "));
    }
  }

  void reportParsingError(kj::StringPtr file,
      capnp::SchemaFile::SourcePos start,
      capnp::SchemaFile::SourcePos end,
      kj::StringPtr message) override {
    errors.push_back(ConfigParseError{
      .file = kj::str(file).as<RustCopyUncheckedUtf8>(),
      .line = start.line + 1,
      .column = start.column + 1,
      .end_column = start.line == end.line && start.column < end.column ? end.column + 1 : 0,
      .message = kj::str(message).as<RustCopyUncheckedUtf8>(),
    });
  }

  void reportFileRead(kj::PathPtr path) override {
    process.watch_file(path.toNativeString(true).asBytes().as<Rust>());
  }
};

}  // namespace

CompiledConfig compile_config(::rust::Str path,
    ::rust::Slice<const ::rust::String> importPaths,
    kj::Maybe<::rust::Str> constName,
    const Process& process) {
  ConfigCompiler compiler(process);
  for (auto& importPath: importPaths) {
    compiler.addImportPath(kj::str(importPath));
  }
  compiler.parseConfigFile(kj::str(path));
  KJ_IF_SOME(name, constName) {
    compiler.setConstName(kj::str(name));
  }
  return CompiledConfig{
    .message = compiler.encodeConfig(),
    .errors = kj::mv(compiler.errors),
  };
}

}  // namespace workerd::server::cli
