#include <workerd/io/compatibility-date.h>

#include <capnp/dynamic.h>
#include <capnp/schema.h>
#include <kj/debug.h>
#include <kj/filesystem.h>
#include <kj/string.h>
#include <kj/vector.h>

int main(int argc, char* argv[]) {
  KJ_REQUIRE(argc == 3, "expected maximum compatibility date and output paths");

  auto filesystem = kj::newDiskFilesystem();
  auto maximumDatePath = filesystem->getCurrentPath().evalNative(argv[1]);
  auto maximumDate = filesystem->getRoot().openFile(maximumDatePath)->readAllText();

  kj::Vector<kj::String> output;
  output.add(kj::str("@0xd9e8b7e1607f5c54;\n\n"));

  auto schema = capnp::Schema::from<workerd::CompatibilityFlags>();
  for (auto field: schema.getFields()) {
    bool isPythonSnapshotRelease = false;
    kj::Maybe<kj::StringPtr> impliedDate;

    for (auto annotation: field.getProto().getAnnotations()) {
      if (annotation.getId() == workerd::PYTHON_SNAPSHOT_RELEASE_ANNOTATION_ID) {
        isPythonSnapshotRelease = true;
      } else if (annotation.getId() == workerd::IMPLIED_BY_AFTER_DATE_ANNOTATION_ID) {
        impliedDate = annotation.getValue().getStruct().as<workerd::ImpliedByAfterDate>().getDate();
      }
    }

    if (!isPythonSnapshotRelease) {
      continue;
    }

    kj::StringPtr date = "2024-03-01"_kj;
    KJ_IF_SOME(value, impliedDate) {
      date = value;
    }
    if (maximumDate < date) {
      date = maximumDate;
    }

    output.add(kj::str("const ", field.getProto().getName(), " :Text = \"", date, "\";\n"));
  }

  auto outputPath = filesystem->getCurrentPath().evalNative(argv[2]);
  auto outputFile =
      filesystem->getRoot().replaceFile(outputPath, kj::WriteMode::CREATE | kj::WriteMode::MODIFY);
  auto content = kj::strArray(output, "");
  outputFile->get().writeAll(content.asBytes());
  outputFile->commit();
  return 0;
}
