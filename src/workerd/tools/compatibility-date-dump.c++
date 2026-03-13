// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tool to dump all compatibility flags with their dates as JSON.
// Used by CI to validate that new flags have dates sufficiently far in the future.

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/tools/compatibility-date-dump.schema.capnp.h>

#include <unistd.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/io.h>
#include <kj/main.h>

namespace workerd {
namespace tools {
namespace {

struct FlagEntry {
  kj::StringPtr field;
  kj::StringPtr enableFlag;
  kj::Maybe<kj::StringPtr> disableFlag;
  kj::Maybe<kj::StringPtr> date;
  kj::StringPtr dateSource;
};

class CompatibilityDateDump {
 public:
  explicit CompatibilityDateDump(kj::ProcessContext& context): context(context) {}

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "compatibility-date-dump",
        "Dumps all compatibility flags with their dates as JSON.\n"
        "Output format: {\"flags\": [{\"field\": \"name\", "
        "\"enableFlag\": \"flag\", \"date\": \"YYYY-MM-DD\", "
        "\"dateSource\": \"source\"}, ...]}")
        .callAfterParsing(KJ_BIND_METHOD(*this, run))
        .build();
  }

  kj::MainBuilder::Validity run() {
    kj::FdOutputStream out(STDOUT_FILENO);
    kj::Vector<FlagEntry> entries;

    auto schema = capnp::Schema::from<CompatibilityFlags>();

    for (auto field: schema.getFields()) {
      kj::StringPtr fieldName = field.getProto().getName();
      kj::Maybe<kj::StringPtr> enableFlag;
      kj::Maybe<kj::StringPtr> disableFlag;
      kj::Maybe<kj::StringPtr> date;
      kj::StringPtr dateSource = "";

      for (auto annotation: field.getProto().getAnnotations()) {
        if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
          enableFlag = annotation.getValue().getText();
        } else if (annotation.getId() == COMPAT_DISABLE_FLAG_ANNOTATION_ID) {
          disableFlag = annotation.getValue().getText();
        } else if (annotation.getId() == COMPAT_ENABLE_DATE_ANNOTATION_ID) {
          date = annotation.getValue().getText();
          dateSource = "compatEnableDate";
        } else if (annotation.getId() == IMPLIED_BY_AFTER_DATE_ANNOTATION_ID) {
          auto value = annotation.getValue();
          auto s = value.getStruct().getAs<workerd::ImpliedByAfterDate>();
          date = s.getDate();
          dateSource = "impliedByAfterDate";
        }
      }

      KJ_IF_SOME(flag, enableFlag) {
        entries.add(FlagEntry{
          .field = fieldName,
          .enableFlag = flag,
          .disableFlag = disableFlag,
          .date = date,
          .dateSource = dateSource,
        });
      }
    }

    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<FlagInfoList>();
    auto list = root.initFlags(entries.size());

    for (size_t i = 0; i < entries.size(); ++i) {
      auto entry = entries[i];
      auto item = list[i];

      item.setField(entry.field);
      item.setEnableFlag(entry.enableFlag);

      KJ_IF_SOME(dflag, entry.disableFlag) {
        item.setDisableFlag(dflag);
      } else {
        item.setDisableFlag("");
      }

      KJ_IF_SOME(d, entry.date) {
        item.setDate(d);
        item.setDateSource(entry.dateSource);
      } else {
        item.setDate("");
        item.setDateSource("");
      }
    }

    capnp::JsonCodec json;
    auto encoded = json.encode(root);
    out.write({encoded.asBytes(), "\n"_kj.asBytes()});
    return true;
  }

 private:
  kj::ProcessContext& context;
};

}  // namespace
}  // namespace tools
}  // namespace workerd

KJ_MAIN(workerd::tools::CompatibilityDateDump)
