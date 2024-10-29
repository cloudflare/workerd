#include <workerd/jsg/compile-cache.h>
#include <workerd/jsg/setup.h>

#include <kj/filesystem.h>
#include <kj/main.h>

namespace workerd::api {
namespace {
JSG_DECLARE_ISOLATE_TYPE(CompileCacheIsolate);

constexpr int resourceLineOffset = 0;
constexpr int resourceColumnOffset = 0;
constexpr bool resourceIsSharedCrossOrigin = false;
constexpr int scriptId = -1;
constexpr bool resourceIsOpaque = false;
constexpr bool isWasm = false;
constexpr bool isModule = true;

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

      auto path = fileListContent.slice(start, end);

      if (path.size() > 0) {
        auto file =
            dir.openFile(kj::Path::parse(kj::StringPtr(path.asChars().begin(), path.size())));
        auto content = file->mmap(0, file->stat().size);

        file_contents.add(kj::tuple(kj::heapString(path.asChars()), kj::mv(content)));
      }

      end++;
      start = end;
    }
  }

  kj::MainBuilder::Validity run() {
    readFiles();

    const auto& compileCache = jsg::CompileCache::get();
    auto options = v8::ScriptCompiler::kNoCompileOptions;

    ccIsolate.runInLockScope([&](CompileCacheIsolate::Lock& js) {
      for (auto& entry: file_contents) {
        kj::StringPtr name = kj::get<0>(entry);
        kj::ArrayPtr<const kj::byte> content = kj::get<1>(entry);

        v8::ScriptOrigin origin(jsg::v8StrIntern(js.v8Isolate, name), resourceLineOffset,
            resourceColumnOffset, resourceIsSharedCrossOrigin, scriptId, {}, resourceIsOpaque,
            isWasm, isModule);

        auto contentStr = jsg::newExternalOneByteString(js, content.asChars());
        auto source = v8::ScriptCompiler::Source(contentStr, origin, nullptr);
        auto module = jsg::check(v8::ScriptCompiler::CompileModule(js.v8Isolate, &source, options));

        compileCache.add(name, module->GetUnboundModuleScript());
      }
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

  // Key is the path of the file, and value is the content.
  kj::Vector<kj::Tuple<kj::String, kj::Array<const kj::byte>>> file_contents{};
};

}  // namespace
}  // namespace workerd::api

KJ_MAIN(workerd::api::CompileCacheCreator)
