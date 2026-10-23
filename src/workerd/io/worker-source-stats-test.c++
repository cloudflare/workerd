#include "worker-source-stats.h"

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("Service Worker globals contribute bytes but not modules") {
  static const kj::byte DATA[] = {0, 1, 2, 3};
  WorkerSource source(WorkerSource::ScriptSource{
    .mainScript = "main script"_kj,
    .mainScriptName = "worker.js"_kj,
    .globals = kj::arr(
        WorkerSource::Module{.name = "wasm"_kj, .content = WorkerSource::WasmModule{.body = DATA}},
        WorkerSource::Module{
          .name = "text"_kj, .content = WorkerSource::TextModule{.body = "text"_kj}},
        WorkerSource::Module{.name = "data"_kj, .content = WorkerSource::DataModule{.body = DATA}}),
  });

  auto stats = computeScriptSourceStats(source);
  KJ_EXPECT(stats.jsBytes == 11);
  KJ_EXPECT(stats.wasmBytes == 4);
  KJ_EXPECT(stats.otherBytes == 8);
  KJ_EXPECT(stats.moduleCount == 0);
}

KJ_TEST("Module source statistics cover every module kind") {
  static const kj::byte DATA[] = {0, 1, 2, 3};
  WorkerSource source(WorkerSource::ModulesSource{
    .mainModule = "esm"_kj,
    .modules = kj::arr(WorkerSource::Module{.name = "esm"_kj,
                         .content = WorkerSource::EsModule{.body = "ES module"_kj.asArray()}},
        WorkerSource::Module{
          .name = "cjs"_kj, .content = WorkerSource::CommonJsModule{.body = "CommonJS"_kj}},
        WorkerSource::Module{.name = "wasm"_kj, .content = WorkerSource::WasmModule{.body = DATA}},
        WorkerSource::Module{
          .name = "text"_kj, .content = WorkerSource::TextModule{.body = "text"_kj}},
        WorkerSource::Module{.name = "data"_kj, .content = WorkerSource::DataModule{.body = DATA}},
        WorkerSource::Module{
          .name = "json"_kj, .content = WorkerSource::JsonModule{.body = "{}"_kj}},
        WorkerSource::Module{
          .name = "python"_kj, .content = WorkerSource::PythonModule{.body = "python"_kj}},
        WorkerSource::Module{
          .name = "requirement"_kj, .content = WorkerSource::ObsoletePythonRequirement{}},
        WorkerSource::Module{
          .name = "capnp"_kj, .content = WorkerSource::CapnpModule{.typeId = 123}}),
    .isPython = false,
  });

  auto stats = computeScriptSourceStats(source);
  KJ_EXPECT(stats.jsBytes == 17);
  KJ_EXPECT(stats.wasmBytes == 4);
  KJ_EXPECT(stats.otherBytes == 16);
  KJ_EXPECT(stats.moduleCount == 9);
}

KJ_TEST("Empty module bundles have zero source statistics") {
  WorkerSource source(WorkerSource::ModulesSource{.isPython = false});
  auto stats = computeScriptSourceStats(source);
  KJ_EXPECT(stats.jsBytes == 0);
  KJ_EXPECT(stats.wasmBytes == 0);
  KJ_EXPECT(stats.otherBytes == 0);
  KJ_EXPECT(stats.moduleCount == 0);
}

}  // namespace
}  // namespace workerd
