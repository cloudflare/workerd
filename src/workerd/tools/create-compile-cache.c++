#include <workerd/jsg/compile-cache.h>
#include <workerd/jsg/setup.h>
#include <workerd/tools/compile-cache.capnp.h>

#include <capnp/serialize.h>
#include <kj/filesystem.h>
#include <kj/main.h>

namespace workerd::tools {
namespace {

constexpr int resourceLineOffset = 0;
constexpr int resourceColumnOffset = 0;
constexpr bool resourceIsSharedCrossOrigin = false;
constexpr int scriptId = -1;
constexpr bool resourceIsOpaque = false;
constexpr bool isWasm = false;
constexpr bool isModule = true;

struct CompilerCacheContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(CompilerCacheContext) {}
};

JSG_DECLARE_ISOLATE_TYPE(CompileCacheIsolate, CompilerCacheContext);

// CompileCacheCreator receives an argument of a text file where each line
// represents the path of the file to create compile caches for.
class CompileCacheCreator {
public:
  explicit CompileCacheCreator(kj::ProcessContext& context)
      : context(context),
        ccIsolate(system, kj::heap<jsg::IsolateObserver>(), params) {};

  kj::MainFunc getMain() {
    return kj::MainBuilder(
        context, "Process a file list", "This binary processes the specified file list.")
        .expectArg("<file_path>", KJ_BIND_METHOD(*this, setFilePath))
        .callAfterParsing(KJ_BIND_METHOD(*this, run))
        .build();
  }

  void readFiles() {
    auto fs = kj::newDiskFilesystem();
    auto& dir = fs->getCurrent();
    auto fileList = dir.openFile(filePath);
    auto fileListContent = fileList->mmap(0, fileList->stat().size);

    size_t start = 0;
    size_t end = 0;

    while (end < fileListContent.size()) {
      while (end < fileListContent.size() && fileListContent[end] != '\n') {
        end++;
      }

      auto line = fileListContent.slice(start, end);

      if (line.size() > 0) {
        auto strLine = kj::StringPtr(line.asChars().begin(), line.size());
        auto space = KJ_REQUIRE_NONNULL(strLine.findFirst(' '));
        auto path = kj::str(strLine.first(space).asChars());
        auto out = kj::str(strLine.slice(space + 1));

        auto file = dir.openFile(kj::Path::parse(path));
        auto content = file->mmap(0, file->stat().size);

        file_contents.add(Target{
          .sourcePath = kj::mv(path),
          .sourceContent = kj::str(content),
          .outputPath = kj::mv(out),
        });
      }

      end++;
      start = end;
    }
  }

  kj::MainBuilder::Validity run() {
    readFiles();

    auto options = v8::ScriptCompiler::kNoCompileOptions;
    auto fs = kj::newDiskFilesystem();
    auto& dir = fs->getCurrent();

    ccIsolate.runInLockScope([&](CompileCacheIsolate::Lock& isolateLock) {
      JSG_WITHIN_CONTEXT_SCOPE(isolateLock,
          isolateLock.newContext<CompilerCacheContext>().getHandle(isolateLock),
          [&](jsg::Lock& js) {
        for (auto& target: file_contents) {
          v8::ScriptOrigin origin(jsg::v8StrIntern(js.v8Isolate, target.sourcePath),
              resourceLineOffset, resourceColumnOffset, resourceIsSharedCrossOrigin, scriptId, {},
              resourceIsOpaque, isWasm, isModule);

          auto contentStr = jsg::newExternalOneByteString(js, target.sourceContent);
          auto source = v8::ScriptCompiler::Source(contentStr, origin, nullptr);
          auto module =
              jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source, options));

          auto output = dir.openFile(kj::Path::parse(target.outputPath),
              kj::WriteMode::CREATE | kj::WriteMode::CREATE_PARENT);
          auto codeCache = v8::ScriptCompiler::CreateCodeCache(module->GetUnboundModuleScript());
          output->writeAll(kj::arrayPtr(codeCache->data, codeCache->length));
          delete codeCache;
        }
      });
    });

    return true;
  }

private:
  kj::ProcessContext& context;
  kj::Path filePath{};

  jsg::V8System system{};
  v8::Isolate::CreateParams params{};
  CompileCacheIsolate ccIsolate;

  kj::MainBuilder::Validity setFilePath(kj::StringPtr path) {
    filePath = kj::Path::parse(path);
    return true;
  }

  struct Target {
    kj::String sourcePath;
    kj::String sourceContent;
    kj::String outputPath;
  };

  // Key is the path of the file, and value is the content.
  kj::Vector<Target> file_contents{};
};

}  // namespace
}  // namespace workerd::tools

KJ_MAIN(workerd::tools::CompileCacheCreator)
