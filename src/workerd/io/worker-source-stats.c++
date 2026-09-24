#include "worker-source-stats.h"

namespace workerd {
namespace {

void addModuleSourceStats(
    const WorkerSource::Module& module, IsolateObserver::ScriptSourceStats& stats) {
  KJ_SWITCH_ONEOF(module.content) {
    KJ_CASE_ONEOF(esm, WorkerSource::EsModule) {
      stats.jsBytes += esm.body.size();
    }
    KJ_CASE_ONEOF(cjs, WorkerSource::CommonJsModule) {
      stats.jsBytes += cjs.body.size();
    }
    KJ_CASE_ONEOF(wasm, WorkerSource::WasmModule) {
      stats.wasmBytes += wasm.body.size();
    }
    KJ_CASE_ONEOF(text, WorkerSource::TextModule) {
      stats.otherBytes += text.body.size();
    }
    KJ_CASE_ONEOF(data, WorkerSource::DataModule) {
      stats.otherBytes += data.body.size();
    }
    KJ_CASE_ONEOF(json, WorkerSource::JsonModule) {
      stats.otherBytes += json.body.size();
    }
    KJ_CASE_ONEOF(python, WorkerSource::PythonModule) {
      stats.otherBytes += python.body.size();
    }
    KJ_CASE_ONEOF(_, WorkerSource::ObsoletePythonRequirement) {}
    KJ_CASE_ONEOF(_, WorkerSource::CapnpModule) {
      // This module contains only a type ID. Its schemas are stored separately in capnpSchemas,
      // so there is no source body to include in the byte totals.
    }
  }
}

}  // namespace

IsolateObserver::ScriptSourceStats computeScriptSourceStats(const WorkerSource& source) {
  IsolateObserver::ScriptSourceStats stats;
  KJ_SWITCH_ONEOF(source.variant) {
    KJ_CASE_ONEOF(script, WorkerSource::ScriptSource) {
      stats.jsBytes += script.mainScript.size();
      for (auto& global: script.globals) {
        addModuleSourceStats(global, stats);
      }
    }
    KJ_CASE_ONEOF(modules, WorkerSource::ModulesSource) {
      for (auto& module: modules.modules) {
        ++stats.moduleCount;
        addModuleSourceStats(module, stats);
      }
    }
  }
  return stats;
}

}  // namespace workerd
