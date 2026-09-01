// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/compatibility-date.capnp.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/tools/compatibility-date-dump.schema.capnp.h>

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/debug.h>
#include <kj/test.h>

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

FlagInfoList::Reader buildFlagDump(capnp::MallocMessageBuilder& message) {
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

  return root.asReader();
}

KJ_TEST("known flags exist with correct dates") {
  capnp::MallocMessageBuilder message;
  auto dump = buildFlagDump(message);
  auto flags = dump.getFlags();

  bool foundFormData = false;
  bool foundFetchRefuses = false;
  bool foundStreamsConstructors = false;

  for (auto info: flags) {
    if (info.getEnableFlag() == "formdata_parser_supports_files") {
      foundFormData = true;
      KJ_EXPECT(info.getDate() == "2021-11-03", info.getDate());
      KJ_EXPECT(info.getDateSource() == "compatEnableDate");
    }

    if (info.getEnableFlag() == "fetch_refuses_unknown_protocols") {
      foundFetchRefuses = true;
      KJ_EXPECT(info.getDate() == "2021-11-10", info.getDate());
      KJ_EXPECT(info.getDateSource() == "compatEnableDate");
    }

    if (info.getEnableFlag() == "streams_enable_constructors") {
      foundStreamsConstructors = true;
      KJ_EXPECT(info.getDate() == "2022-11-30", info.getDate());
      KJ_EXPECT(info.getDateSource() == "compatEnableDate");
    }
  }

  KJ_EXPECT(foundFormData, "formdata_parser_supports_files flag not found");
  KJ_EXPECT(foundFetchRefuses, "fetch_refuses_unknown_protocols flag not found");
  KJ_EXPECT(foundStreamsConstructors, "streams_enable_constructors flag not found");
}

KJ_TEST("all flags have required fields") {
  capnp::MallocMessageBuilder message;
  auto dump = buildFlagDump(message);
  auto flags = dump.getFlags();

  KJ_EXPECT(flags.size() > 0, "Should have at least one flag");

  for (auto info: flags) {
    KJ_EXPECT(info.getField().size() > 0, "Field name should not be empty");
    KJ_EXPECT(info.getEnableFlag().size() > 0, "Enable flag should not be empty");
  }
}

KJ_TEST("date format validation") {
  capnp::MallocMessageBuilder message;
  auto dump = buildFlagDump(message);
  auto flags = dump.getFlags();

  for (auto info: flags) {
    auto date = info.getDate();
    if (date.size() == 0) {
      continue;
    }

    KJ_EXPECT(date.size() == 10, "Date should be 10 characters: ", date);
    KJ_EXPECT(date[4] == '-', "Date should have dash at position 4: ", date);
    KJ_EXPECT(date[7] == '-', "Date should have dash at position 7: ", date);

    for (int i = 0; i < 4; i++) {
      KJ_EXPECT(date[i] >= '0' && date[i] <= '9', "Invalid year digit in: ", date);
    }

    KJ_EXPECT(date[5] >= '0' && date[5] <= '1', "Invalid month in: ", date);
    KJ_EXPECT(date[6] >= '0' && date[6] <= '9', "Invalid month in: ", date);
    KJ_EXPECT(date[8] >= '0' && date[8] <= '3', "Invalid day in: ", date);
    KJ_EXPECT(date[9] >= '0' && date[9] <= '9', "Invalid day in: ", date);
  }
}

KJ_TEST("date source consistency") {
  capnp::MallocMessageBuilder message;
  auto dump = buildFlagDump(message);
  auto flags = dump.getFlags();

  for (auto info: flags) {
    auto date = info.getDate();
    auto source = info.getDateSource();

    if (date.size() > 0) {
      KJ_EXPECT(source.size() > 0,
          "Date source should be set when date is present for field: ", info.getField());
      KJ_EXPECT(source == "compatEnableDate" || source == "impliedByAfterDate",
          "Date source should be 'compatEnableDate' or 'impliedByAfterDate', got: ", source);
    } else {
      KJ_EXPECT(source.size() == 0,
          "Date source should be empty when date is not present for field: ", info.getField());
    }
  }
}

KJ_TEST("no duplicate enable flags") {
  capnp::MallocMessageBuilder message;
  auto dump = buildFlagDump(message);
  auto flags = dump.getFlags();

  kj::HashSet<kj::StringPtr> seenFlags;
  for (auto info: flags) {
    auto flag = info.getEnableFlag();
    KJ_EXPECT(!seenFlags.contains(flag), "Duplicate enable flag found: ", flag);
    seenFlags.insert(flag);
  }
}

KJ_TEST("impliedByAfterDate flags exist") {
  capnp::MallocMessageBuilder message;
  auto dump = buildFlagDump(message);
  auto flags = dump.getFlags();

  bool foundImpliedBy = false;
  for (auto info: flags) {
    if (info.getDateSource() == "impliedByAfterDate") {
      foundImpliedBy = true;
      break;
    }
  }

  KJ_EXPECT(foundImpliedBy, "Should have at least one flag with impliedByAfterDate");
}

KJ_TEST("json codec encodes and decodes dump") {
  capnp::MallocMessageBuilder message;
  auto dump = buildFlagDump(message);

  capnp::JsonCodec json;
  auto encoded = json.encode(dump);

  capnp::MallocMessageBuilder decodedMessage;
  auto decodedBuilder = decodedMessage.initRoot<FlagInfoList>();
  json.decode(encoded.asArray(), decodedBuilder);
  auto decoded = decodedBuilder.asReader();

  KJ_EXPECT(decoded.getFlags().size() == dump.getFlags().size());
}

}  // namespace
}  // namespace tools
}  // namespace workerd
