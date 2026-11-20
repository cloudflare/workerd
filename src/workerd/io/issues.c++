// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "issues.h"

#include <workerd/io/issues/ffi.rs.h>

#include <capnp/message.h>
#include <kj/debug.h>

namespace workerd::issues {

namespace {

// Strings cross to Rust as bytes; Rust decodes them lossily, so this cannot throw.
::rust::Slice<const uint8_t> toRust(kj::StringPtr str) {
  return ::rust::Slice<const uint8_t>(str.asBytes().begin(), str.size());
}

// capnp::Text::Reader must be NUL-terminated, and a rust::String is not, so the text is copied.
kj::String text(const ::rust::String& str) {
  return kj::str(kj::arrayPtr(str.data(), str.size()));
}

void fill(Issue::Field::Builder builder, const FieldRecord& field) {
  builder.setName(text(field.name).asPtr());
  switch (field.kind) {
    case FieldKind::Text:
      builder.getValue().setText(text(field.text).asPtr());
      break;
    case FieldKind::Int:
      builder.getValue().setInt(field.int_value);
      break;
    case FieldKind::Float:
      builder.getValue().setFloat(field.float_value);
      break;
    case FieldKind::Bool:
      builder.getValue().setFlag(field.flag);
      break;
  }
}

Issue::Breadcrumb::State toCapnp(CrumbState state) {
  switch (state) {
    case CrumbState::Started:
      return Issue::Breadcrumb::State::STARTED;
    case CrumbState::Done:
      return Issue::Breadcrumb::State::DONE;
    case CrumbState::Failed:
      return Issue::Breadcrumb::State::FAILED;
    case CrumbState::Stateless:
      return Issue::Breadcrumb::State::NONE;
  }
  KJ_UNREACHABLE;
}

void fill(
    Issue::Builder builder, const IssueRecord& issue, const EndRecord& end, EventOutcome outcome) {
  builder.setCode(text(issue.code).asPtr());
  builder.setMessage(text(issue.message).asPtr());

  auto fields = builder.initFields(issue.fields.size());
  for (size_t i = 0; i < issue.fields.size(); i++) {
    fill(fields[i], issue.fields[i]);
  }

  auto errors = builder.initErrors(issue.errors.size());
  for (size_t i = 0; i < issue.errors.size(); i++) {
    errors[i].setName(text(issue.errors[i].name).asPtr());
    errors[i].setMessage(text(issue.errors[i].message).asPtr());
    errors[i].setStack(text(issue.errors[i].stack).asPtr());
  }

  auto attachments = builder.initAttachments(issue.attachments.size());
  for (size_t i = 0; i < issue.attachments.size(); i++) {
    attachments[i].setName(text(issue.attachments[i].name).asPtr());
    attachments[i].setBody(text(issue.attachments[i].body).asPtr());
    attachments[i].setTruncated(issue.attachments[i].truncated);
  }

  auto breadcrumbs = builder.initBreadcrumbs(issue.breadcrumbs.size());
  for (size_t i = 0; i < issue.breadcrumbs.size(); i++) {
    auto& crumb = issue.breadcrumbs[i];
    breadcrumbs[i].setAtMs(crumb.at_ms);
    breadcrumbs[i].setKind(
        crumb.is_log ? Issue::Breadcrumb::Kind::LOG : Issue::Breadcrumb::Kind::CALL);
    breadcrumbs[i].setState(toCapnp(crumb.state));
    breadcrumbs[i].setLevel(text(crumb.level).asPtr());
    breadcrumbs[i].setLabel(text(crumb.label).asPtr());
  }

  auto user = builder.initUser();
  user.setUserId(text(issue.user_id).asPtr());
  user.setSessionId(text(issue.session_id).asPtr());
  user.setAccountId(text(issue.account_id).asPtr());

  auto invocation = builder.initInvocation();
  invocation.setEvent(text(end.event).asPtr());
  invocation.setMethod(text(end.method).asPtr());
  invocation.setPath(text(end.path).asPtr());

  builder.setStatus(end.status);
  builder.setOutcome(outcome);
}

}  // namespace

IssueCollector::IssueCollector(IssueSink& sink): sink(sink), impl(new_collector()) {}
IssueCollector::~IssueCollector() noexcept(false) {}

bool IssueCollector::report(kj::StringPtr code,
    kj::StringPtr message,
    kj::ArrayPtr<const Field> fields,
    kj::ArrayPtr<const Error> errors,
    kj::ArrayPtr<Attachment> attachments) {
  if (!impl->report_issue(toRust(code), toRust(message))) return false;

  for (auto& field: fields) {
    KJ_SWITCH_ONEOF(field.value) {
      KJ_CASE_ONEOF(value, kj::StringPtr) {
        impl->add_field_string(toRust(field.name), toRust(value));
      }
      KJ_CASE_ONEOF(value, int64_t) {
        impl->add_field_int(toRust(field.name), value);
      }
      KJ_CASE_ONEOF(value, double) {
        impl->add_field_float(toRust(field.name), value);
      }
      KJ_CASE_ONEOF(value, bool) {
        impl->add_field_bool(toRust(field.name), value);
      }
    }
  }
  for (auto& error: errors) {
    impl->add_error(
        toRust(error.name), toRust(error.message), toRust(error.stack.orDefault(""_kj)));
  }
  for (auto& attachment: attachments) {
    impl->add_attachment(toRust(attachment.name), toRust(attachment.render()));
  }
  return true;
}

void IssueCollector::breadcrumb(BreadcrumbKind kind, kj::StringPtr detail, kj::StringPtr label) {
  impl->breadcrumb(static_cast<uint8_t>(kind), toRust(detail), toRust(label));
}

void IssueCollector::noteUserContext(kj::StringPtr key, kj::StringPtr value) {
  impl->note_user_context(toRust(key), toRust(value));
}

void IssueCollector::setEvent(kj::StringPtr event) {
  impl->set_event(toRust(event));
}

void IssueCollector::setRequest(kj::StringPtr method, kj::StringPtr path) {
  impl->set_request(toRust(method), toRust(path));
}

void IssueCollector::setStatus(uint32_t status) {
  impl->set_status(static_cast<uint16_t>(status));
}

void IssueCollector::end(EventOutcome outcome) {
  auto end = impl->end();
  for (auto& issue: end.issues) {
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      capnp::MallocMessageBuilder message;
      fill(message.initRoot<Issue>(), issue, end, outcome);
      sink.write(message.getRoot<Issue>().asReader());
    })) {
      KJ_LOG(ERROR, "issue sink threw", exception);
    }
  }
}

}  // namespace workerd::issues
