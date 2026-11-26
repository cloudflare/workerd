// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/http.h>
#include <workerd/api/js-readable-stream.h>
#include <workerd/io/io-own.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/modules-new.h>
#include <workerd/jsg/modules.h>
#include <workerd/util/strong-bool.h>

#include <kj/async-io.h>
#include <kj/compat/http.h>

namespace workerd::api {

struct ExportedHandler;

struct EmailSendResult {
  kj::String messageId;

  JSG_STRUCT(messageId);
};

// `name` is intentionally not wrapped in `jsg::Optional`: callers omitting `name`
// (e.g. `{ email: "foo@bar.com" }`) must receive a synchronous TypeError, not a silent default.
struct EmailAddress {
  jsg::NonCoercible<kj::String> name;
  jsg::NonCoercible<kj::String> email;

  JSG_STRUCT(name, email)
};

struct EmailAttachments {
  kj::String filename;
  kj::String disposition;
  kj::String type;
  jsg::Optional<kj::String> contentId;
  kj::OneOf<kj::String, jsg::JsRef<jsg::JsBufferSource>> content;

  JSG_STRUCT(filename, disposition, type, contentId, content);
};

// Reply builder: a reply always goes back to the original sender, so unlike
// EmailMessageBuilder there are no to/cc/bcc fields.
struct ReplyEmailMessageBuilder {
  kj::OneOf<kj::String, EmailAddress> from;
  kj::String subject;
  jsg::Optional<jsg::Dict<kj::String>> headers;
  jsg::Optional<kj::String> text;
  jsg::Optional<kj::String> html;
  jsg::Optional<kj::OneOf<kj::String, EmailAddress>> replyTo;

  jsg::Optional<kj::Array<EmailAttachments>> attachments;

  JSG_STRUCT(from, subject, headers, text, html, replyTo, attachments);
};

// A recipient field (to/cc/bcc) accepts: a single string, a single named-address
// object, or an array whose elements may each be either a string or a named-address object.

// `kj::Array<...>` MUST come before `EmailAddress`: JSG's OneOf tries fallible types
// in declaration order, and `StructWrapper` accepts JS arrays (`DictWrapper` excludes them).

// If the order were reversed, a JS array would be mis-unwrapped as
// `EmailAddress{name:"undefined", email:"undefined"}` via undefined→ToString coercion.
using EmailRecipients =
    kj::OneOf<kj::String, kj::Array<kj::OneOf<kj::String, EmailAddress>>, EmailAddress>;

struct EmailMessageBuilder {

  jsg::Optional<EmailRecipients> to;
  kj::OneOf<kj::String, EmailAddress> from;
  kj::String subject;
  jsg::Optional<EmailRecipients> cc;
  jsg::Optional<EmailRecipients> bcc;
  jsg::Optional<jsg::Dict<kj::String>> headers;
  jsg::Optional<kj::String> text;
  jsg::Optional<kj::String> html;
  jsg::Optional<kj::OneOf<kj::String, EmailAddress>> replyTo;

  jsg::Optional<kj::Array<EmailAttachments>> attachments;

  JSG_STRUCT(to, from, subject, cc, bcc, headers, text, html, replyTo, attachments);
};

// =======================================================================================
// Email service interface
//
// The runtime embedder provides the email service: it delivers incoming email to Workers and
// carries out the forwards, replies and sends they request. The types below are the interface
// between the JavaScript API and that service.

// An error reported by the email service.
struct EmailServiceError {
  kj::String message;
  kj::Maybe<kj::String> code;
};

// The email service accepted a message. `messageId` is absent if the service did not assign one.
struct EmailServiceAccepted {
  kj::Maybe<kj::String> messageId;
};

using EmailServiceResponse = kj::OneOf<EmailServiceAccepted, EmailServiceError>;

// A message to be transmitted as raw bytes, written to `data`.
struct RawEmailTransmission {
  kj::Own<kj::AsyncOutputStream> data;
  kj::Promise<EmailServiceResponse> response;
};

// A message composed by a message builder. The text and html bodies are written to their own
// streams, which are always provided even if the builder has no such body, and each attachment
// is written to the stream at the same index in `attachments`.
struct BuilderEmailTransmission {
  kj::Own<kj::AsyncOutputStream> text;
  kj::Own<kj::AsyncOutputStream> html;
  kj::Array<kj::Own<kj::AsyncOutputStream>> attachments;
  kj::Promise<EmailServiceResponse> response;
};

WD_STRONG_BOOL(EnableEmailQueuing);

// Forwards and replies to one incoming email.
class EmailForwarder {
 public:
  virtual ~EmailForwarder() noexcept(false) = default;

  // Forwards the incoming email, unaltered apart from the added `headers`, to `rcptTo`.
  // Resolves to the error if the email service refuses.
  virtual kj::Promise<kj::Maybe<EmailServiceError>> forward(
      kj::StringPtr rcptTo, kj::Maybe<const kj::HttpHeaders&> headers) = 0;

  virtual RawEmailTransmission reply(kj::StringPtr mailFrom, kj::StringPtr rcptTo) = 0;
  virtual BuilderEmailTransmission replyWithBuilder(const ReplyEmailMessageBuilder& message) = 0;
};

// Sends email on behalf of a `send_email` binding. The binding is shared by every request to the
// Worker, so implementations obtain anything request-specific from IoContext::current().
class EmailSender {
 public:
  virtual ~EmailSender() noexcept(false) = default;

  virtual RawEmailTransmission send(
      kj::StringPtr mailFrom, kj::StringPtr rcptTo, EnableEmailQueuing enableQueuing) = 0;
  virtual BuilderEmailTransmission sendWithBuilder(const EmailMessageBuilder& message) = 0;
};

struct EmailHeader {
  kj::String name;
  kj::String value;
};

// An email delivered to a Worker's `email` handler.
struct IncomingEmail {
  // RFC5321 envelope.
  kj::String mailFrom;
  kj::String rcptTo;

  // RFC5322 header fields. `headersFullValues` carries every address in the To and CC fields;
  // which list the Worker sees depends on the `set_forwardable_email_full_headers` flag.
  kj::Array<EmailHeader> headers;
  kj::Array<EmailHeader> headersFullValues;

  // The raw message, and its total size.
  kj::Own<kj::AsyncInputStream> data;
  uint64_t dataSize;

  // Only authenticated messages may be forwarded.
  bool canBeForwarded;

  IoOwn<EmailForwarder> forwarder;
};

// =======================================================================================
// JavaScript API

class EmailMessage: public jsg::Object {
 public:
  // `raw` may be a null JsReadableStream, meaning no raw data was provided (yet); see
  // ForwardableEmailMessage, which assigns it after construction.
  explicit EmailMessage(kj::String from, kj::String to, JsReadableStream raw)
      : from(kj::mv(from)),
        to(kj::mv(to)),
        raw(kj::mv(raw)) {};

  // TODO: add support for TypedArrays in the future. Will need to also receive the encoding,
  //      and we should strive to use a single param, which can be an options object.
  static jsg::Ref<EmailMessage> constructor(jsg::Lock& js,
      kj::String from,
      kj::String to,
      kj::OneOf<JsReadableStream, jsg::NonCoercible<kj::String>> raw) {

    KJ_SWITCH_ONEOF(raw) {
      KJ_CASE_ONEOF(rawStream, JsReadableStream) {
        return js.alloc<EmailMessage>(kj::mv(from), kj::mv(to), kj::mv(rawStream));
      }
      KJ_CASE_ONEOF(rawString, jsg::NonCoercible<kj::String>) {
        return js.alloc<EmailMessage>(
            kj::mv(from), kj::mv(to), JsReadableStream(js, kj::mv(rawString.value)));
      }
    }

    KJ_UNREACHABLE;
  }

  kj::StringPtr getFrom() {
    return from;
  }
  kj::StringPtr getTo() {
    return to;
  }

  JsReadableStream getRaw(jsg::Lock& js) {
    KJ_ASSERT(!raw.isNull(), "raw data not provided");
    return raw.addRef(js);
  };

  JSG_RESOURCE_TYPE(EmailMessage) {
    JSG_READONLY_PROTOTYPE_PROPERTY(from, getFrom);
    JSG_READONLY_PROTOTYPE_PROPERTY(to, getTo);
  }

  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

 protected:
  kj::String from;
  kj::String to;
  JsReadableStream raw;

  // There's a slight possibility of the ReadableStream capturing a reference to this
  // EmailMessage instance, so we have to make sure to implement gc tracing here.
  void visitForGc(jsg::GcVisitor& visitor);
};

struct EmailEventResult final: public kj::Refcounted {
  kj::Maybe<kj::String> reject;
};

class ForwardableEmailMessage final: public EmailMessage {
 public:
  explicit ForwardableEmailMessage(
      jsg::Lock& js, IncomingEmail email, IoOwn<EmailEventResult> result);

  static jsg::Ref<ForwardableEmailMessage> constructor() = delete;

  double getRawDataSize() {
    return rawDataSize;
  }

  bool getCanBeForwarded() {
    return canBeForwarded;
  }

  jsg::Ref<Headers> getHeaders(jsg::Lock& js) {
    return headers.addRef();
  }

  void setReject(kj::String reason) {
    result->reject = kj::mv(reason);
  }

  jsg::Promise<void> forward(
      jsg::Lock& js, kj::String rcptTo, jsg::Optional<jsg::Ref<Headers>> maybeHeaders);
  jsg::Promise<jsg::Optional<EmailSendResult>> reply(
      jsg::Lock& js, kj::OneOf<jsg::Ref<EmailMessage>, ReplyEmailMessageBuilder> message);

  JSG_RESOURCE_TYPE(ForwardableEmailMessage) {
    JSG_INHERIT(EmailMessage);
    JSG_READONLY_PROTOTYPE_PROPERTY(headers, getHeaders);
    JSG_READONLY_PROTOTYPE_PROPERTY(raw, getRaw);
    JSG_READONLY_PROTOTYPE_PROPERTY(rawSize, getRawDataSize);
    JSG_READONLY_PROTOTYPE_PROPERTY(canBeForwarded, getCanBeForwarded);

    JSG_METHOD(setReject);
    JSG_METHOD(forward);
    JSG_METHOD(reply);
  }

 private:
  jsg::Ref<Headers> headers;
  IoOwn<EmailForwarder> forwarder;
  IoOwn<EmailEventResult> result;
  double rawDataSize;
  bool canBeForwarded;

  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(headers);
  }
};

class EmailEvent final: public ExtendableEvent {
 public:
  explicit EmailEvent(jsg::Ref<ForwardableEmailMessage> message)
      : ExtendableEvent("email"),
        message(kj::mv(message)) {}

  static jsg::Ref<EmailEvent> constructor(kj::String type) = delete;

  jsg::Ref<ForwardableEmailMessage> getMessage(jsg::Lock& js) {
    return message.addRef();
  }

  JSG_RESOURCE_TYPE(EmailEvent) {
    JSG_INHERIT(ExtendableEvent);

    JSG_READONLY_PROTOTYPE_PROPERTY(message, getMessage);
  }

 private:
  jsg::Ref<ForwardableEmailMessage> message;
  void visitForGc(jsg::GcVisitor& visitor) {
    visitor.visit(message);
  }
};

// Extension of ExportedHandler covering email handers.
struct EmailExportedHandler {
  using EmailHandler = kj::Promise<void>(jsg::Ref<ForwardableEmailMessage> message,
      jsg::Value env,
      jsg::Optional<jsg::Ref<ExecutionContext>> ctx);
  jsg::LenientOptional<jsg::Function<EmailHandler>> email;

  JSG_STRUCT(email);
};

// Delivers `email` to the Worker's `email` handler, or to its `email` event listeners if
// `exportedHandler` is none. The handler can reject the email through `result`.
void startEmailEvent(Worker::Lock& lock,
    IoContext& context,
    EventTarget& globalEventTarget,
    IncomingEmail email,
    kj::Maybe<ExportedHandler&> exportedHandler,
    const jsg::TypeHandler<EmailExportedHandler>& handlerHandler,
    IoOwn<EmailEventResult> result);

// Here we will expose types needed for email related operations.
class EmailModule final: public jsg::Object {
 public:
  EmailModule() = default;
  EmailModule(jsg::Lock&, const jsg::Url&) {}
  JSG_RESOURCE_TYPE(EmailModule) {
    JSG_NESTED_TYPE(EmailMessage);
    JSG_TS_ROOT();
  }
};

class SendEmail final: public jsg::Object {
 public:
  explicit SendEmail(kj::Own<EmailSender> sender): sender(kj::mv(sender)) {}

  jsg::Promise<jsg::Optional<EmailSendResult>> send(
      jsg::Lock& js, kj::OneOf<jsg::Ref<EmailMessage>, EmailMessageBuilder> message);

  JSG_RESOURCE_TYPE(SendEmail) {
    JSG_METHOD(send);
  }

 private:
  kj::Own<EmailSender> sender;
};

template <typename TypeWrapper>
void registerEmailModules(jsg::ModuleRegistryImpl<TypeWrapper>& registry, auto featureFlags) {
  registry.template addBuiltinModule<EmailModule>(
      "cloudflare-internal:email", jsg::ModuleRegistry::Type::INTERNAL);
}

template <typename TypeWrapper>
void registerEmailModuleBundle(jsg::modules::ModuleRegistry::Builder& builder) {
  jsg::modules::ModuleBundle::BuiltinBuilder builtins(
      jsg::modules::ModuleBundle::BuiltinBuilder::Type::BUILTIN_ONLY);
  builtins.addObject<EmailModule, TypeWrapper>("cloudflare-internal:email"_url);
  builder.add(builtins.finish());
}

// clang-format off
#define EW_EMAIL_ISOLATE_TYPES                                                                     \
  api::EmailAttachments,                                                                           \
  api::EmailAddress,                                                                               \
  api::EmailMessageBuilder,                                                                        \
  api::ReplyEmailMessageBuilder,                                                                   \
  api::EmailSendResult,                                                                            \
  api::SendEmail,                                                                                  \
  api::EmailMessage,                                                                               \
  api::ForwardableEmailMessage,                                                                    \
  api::EmailEvent,                                                                                 \
  api::EmailModule,                                                                                \
  api::EmailExportedHandler
// clang-format on

}  // namespace workerd::api
