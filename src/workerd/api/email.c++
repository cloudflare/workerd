// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "email.h"

#include <workerd/api/deferred-proxy.h>
#include <workerd/api/global-scope.h>
#include <workerd/api/system-streams.h>
#include <workerd/io/features.h>
#include <workerd/io/io-context.h>
#include <workerd/io/tracer.h>
#include <workerd/util/header-validation.h>

namespace workerd::api {

namespace {

bool isValidHeaderName(kj::StringPtr name) {
  if (name.size() == 0) return false;
  for (char c: name) {
    if (!util::isHttpTokenChar(c)) return false;
  }
  return true;
}

jsg::JsValue makeEmailServiceError(jsg::Lock& js, const EmailServiceError& error) {
  auto jsError = js.error(error.message);
  KJ_IF_SOME(code, error.code) {
    KJ_ASSERT_NONNULL(jsError.tryCast<jsg::JsObject>()).set(js, "code", js.str(code));
  }
  return jsError;
}

jsg::Promise<jsg::Optional<EmailSendResult>> handleEmailServiceResponse(
    jsg::Lock& js, EmailServiceResponse response) {
  KJ_SWITCH_ONEOF(response) {
    KJ_CASE_ONEOF(accepted, EmailServiceAccepted) {
      KJ_IF_SOME(messageId, accepted.messageId) {
        return js.resolvedPromise(
            jsg::Optional<EmailSendResult>(EmailSendResult{.messageId = kj::mv(messageId)}));
      }
      // Backwards compatibility - no message ID available
      return js.resolvedPromise(jsg::Optional<EmailSendResult>(kj::none));
    }
    KJ_CASE_ONEOF(error, EmailServiceError) {
      return js.rejectedPromise<jsg::Optional<EmailSendResult>>(makeEmailServiceError(js, error));
    }
  }
  KJ_UNREACHABLE;
}

// Waits for the email service's response to a message whose content is being written by
// `contentPromise`. The content is not complete until the service has received it, so a failure
// to write the content only matters if it occurs before the response.
template <typename T>
jsg::Promise<jsg::Optional<EmailSendResult>> awaitEmailServiceResponse(jsg::Lock& js,
    TraceContext traceContext,
    kj::Promise<T> contentPromise,
    kj::Promise<EmailServiceResponse> responsePromise) {
  auto& context = IoContext::current();
  auto joinedPromise =
      contentPromise.then([](T) -> kj::Promise<EmailServiceResponse> {
    return kj::NEVER_DONE;
  }).exclusiveJoin(kj::mv(responsePromise));

  return context.attachSpans(js,
      context.awaitIo(js, kj::mv(joinedPromise))
          .then(js,
              [](jsg::Lock& js, EmailServiceResponse response) {
    return handleEmailServiceResponse(js, kj::mv(response));
  }),
      kj::mv(traceContext));
}

jsg::Promise<jsg::Optional<EmailSendResult>> driveMailSend(jsg::Lock& js,
    kj::LiteralStringConst operationName,
    jsg::Ref<EmailMessage> message,
    RawEmailTransmission transmission) {
  auto& context = IoContext::current();
  auto traceContext = context.makeUserTraceSpan(operationName);

  auto raw = message->getRaw(js);
  JSG_REQUIRE(
      !raw.isDisturbed(js) && !raw.isLocked(js), Error, "ReadableStream has already been consumed");

  auto stream = newSystemStream(kj::mv(transmission.data), StreamEncoding::IDENTITY, context);
  auto pumpPromise = raw.pumpTo(js, kj::mv(stream), EndStream::YES);

  return awaitEmailServiceResponse(
      js, kj::mv(traceContext), kj::mv(pumpPromise), kj::mv(transmission.response));
}

kj::Promise<DeferredProxy<void>> makeStreamPumpPromise(
    jsg::Lock& js, kj::Own<kj::AsyncOutputStream> output, kj::ArrayPtr<kj::byte> source) {
  auto& context = IoContext::current();
  auto stream = newSystemStream(kj::mv(output), StreamEncoding::IDENTITY, context);
  // The caller is responsible for keeping source alive until the pump is done.
  return JsReadableStream::create(js, context, newMemorySource(source))
      .pumpTo(js, kj::mv(stream), EndStream::YES);
};

// Writes an optional text or html body. With no body, dropping the stream ends it empty.
kj::Promise<DeferredProxy<void>> writeOptionalBody(
    jsg::Lock& js, kj::Own<kj::AsyncOutputStream> output, jsg::Optional<kj::String>& body) {
  KJ_IF_SOME(b, body) {
    return makeStreamPumpPromise(js, kj::mv(output), b.asBytes());
  }
  return newNoopDeferredProxy();
}

kj::ArrayPtr<kj::byte> getArrayPtrFromContent(
    jsg::Lock& js, kj::OneOf<kj::String, jsg::JsRef<jsg::JsBufferSource>>& content) {
  KJ_SWITCH_ONEOF(content) {
    KJ_CASE_ONEOF(string, kj::String) {
      return string.asBytes();
    }
    KJ_CASE_ONEOF(buffer, jsg::JsRef<jsg::JsBufferSource>) {
      return buffer.getHandle(js).asArrayPtr();
    }
  }
  KJ_UNREACHABLE;
}

// Writes the content of a message builder (which can be in the MBs range, and is split into
// multiple fields) to the streams of a transmission the email service has already accepted the
// metadata of. Templated over the builder type so it serves both EmailMessageBuilder (send) and
// ReplyEmailMessageBuilder (reply); only the text/html/attachments fields are used.
template <typename Builder>
jsg::Promise<jsg::Optional<EmailSendResult>> driveEmailBuilderSend(jsg::Lock& js,
    kj::LiteralStringConst operationName,
    Builder message,
    BuilderEmailTransmission transmission) {
  auto& context = IoContext::current();
  auto traceContext = context.makeUserTraceSpan(operationName);

  size_t attachmentCount = 0;
  KJ_IF_SOME(attachments, message.attachments) {
    attachmentCount = attachments.size();
  }
  KJ_ASSERT(transmission.attachments.size() == attachmentCount);

  auto streamPromiseArrayBuilder =
      kj::heapArrayBuilder<kj::Promise<DeferredProxy<void>>>(2 + attachmentCount);

  streamPromiseArrayBuilder.add(writeOptionalBody(js, kj::mv(transmission.text), message.text));
  streamPromiseArrayBuilder.add(writeOptionalBody(js, kj::mv(transmission.html), message.html));

  KJ_IF_SOME(attachments, message.attachments) {
    for (auto i: kj::indices(attachments)) {
      streamPromiseArrayBuilder.add(makeStreamPumpPromise(js, kj::mv(transmission.attachments[i]),
          getArrayPtrFromContent(js, attachments[i].content)));
    }
  }

  auto streamPromise = kj::joinPromisesFailFast(streamPromiseArrayBuilder.finish())
                           // NOTE(lduarte): this move is explicit. we want to keep message alive
                           // while we are sending the stream(s) contents. Otherwise, message
                           // would be freed at the end of the scope resulting in a use-after-free.
                           .attach(kj::mv(message));

  return awaitEmailServiceResponse(
      js, kj::mv(traceContext), kj::mv(streamPromise), kj::mv(transmission.response));
}

// Number of recipients a single to/cc/bcc field resolves to: a string or
// `EmailAddress` is one recipient, an array is its length.
size_t recipientCount(EmailRecipients& addresses) {
  KJ_SWITCH_ONEOF(addresses) {
    KJ_CASE_ONEOF(s, kj::String) {
      return 1;
    }
    KJ_CASE_ONEOF(o, EmailAddress) {
      return 1;
    }
    KJ_CASE_ONEOF(a, kj::Array<kj::OneOf<kj::String, EmailAddress>>) {
      return a.size();
    }
  }
  KJ_UNREACHABLE;
}

}  // namespace

ForwardableEmailMessage::ForwardableEmailMessage(
    jsg::Lock& js, IncomingEmail email, IoOwn<EmailEventResult> result)
    : EmailMessage(kj::mv(email.mailFrom), kj::mv(email.rcptTo), JsReadableStream()),
      headers(nullptr),
      forwarder(kj::mv(email.forwarder)),
      result(kj::mv(result)),
      rawDataSize(email.dataSize),
      canBeForwarded(email.canBeForwarded) {

  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders kjHeaders(headerTable);

  auto flags = FeatureFlags::get(js);
  kj::ArrayPtr<const EmailHeader> incomingHeaders;
  if (flags.getEnableForwardableEmailFullHeaders()) {
    incomingHeaders = email.headersFullValues.asPtr();
  } else {
    incomingHeaders = email.headers.asPtr();
  }

  for (auto& h: incomingHeaders) {
    // Here we validate header names and values, which can ran afoul of strict validation rules.
    // Since we still forward the raw email to the user, where even invalid headers will be present,
    // we opt to just skip them here in order to be able to build this header map which will just
    // contain strictly valid header fields.
    if (!isValidHeaderName(h.name) || !util::isValidHeaderValue(h.value)) {
      continue;
    };
    kjHeaders.addPtrPtr(h.name, h.value);
  }
  this->headers = js.alloc<Headers>(js, kjHeaders, Headers::Guard::IMMUTABLE);

  auto& context = IoContext::current();
  auto stream = newSystemStream(kj::mv(email.data), StreamEncoding::IDENTITY, context);
  this->raw = JsReadableStream::create(js, context, kj::mv(stream));
}

jsg::Promise<void> ForwardableEmailMessage::forward(
    jsg::Lock& js, kj::String rcptTo, jsg::Optional<jsg::Ref<Headers>> maybeHeaders) {
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders kjHeaders(headerTable);
  kj::Maybe<const kj::HttpHeaders&> extraHeaders;
  KJ_IF_SOME(headers, maybeHeaders) {
    headers->shallowCopyTo(kjHeaders);
    extraHeaders = kjHeaders;
  }

  auto& context = IoContext::current();
  return context.awaitIo(js, forwarder->forward(rcptTo, extraHeaders))
      .then(js, [](jsg::Lock& js, kj::Maybe<EmailServiceError> maybeError) {
    KJ_IF_SOME(error, maybeError) {
      if (error.code == kj::none) {
        JSG_FAIL_REQUIRE(Error, error.message);
      }
      js.throwException(makeEmailServiceError(js, error));
    }
  });
}

jsg::Promise<jsg::Optional<EmailSendResult>> ForwardableEmailMessage::reply(
    jsg::Lock& js, kj::OneOf<jsg::Ref<EmailMessage>, ReplyEmailMessageBuilder> messageOrBuilder) {
  KJ_SWITCH_ONEOF(messageOrBuilder) {
    KJ_CASE_ONEOF(message, jsg::Ref<EmailMessage>) {
      auto transmission = forwarder->reply(message->getFrom(), message->getTo());
      return driveMailSend(js, "reply_email"_kjc, kj::mv(message), kj::mv(transmission));
    }
    KJ_CASE_ONEOF(message, ReplyEmailMessageBuilder) {
      auto transmission = forwarder->replyWithBuilder(message);
      return driveEmailBuilderSend(
          js, "reply_email_builder"_kjc, kj::mv(message), kj::mv(transmission));
    }
  }
  KJ_UNREACHABLE;
}

void startEmailEvent(Worker::Lock& lock,
    IoContext& context,
    EventTarget& globalEventTarget,
    IncomingEmail email,
    kj::Maybe<ExportedHandler&> exportedHandler,
    const jsg::TypeHandler<EmailExportedHandler>& handlerHandler,
    IoOwn<EmailEventResult> result) {
  auto isolate = lock.getIsolate();
  jsg::Lock& js = lock;
  auto message = js.alloc<ForwardableEmailMessage>(js, kj::mv(email), kj::mv(result));

  KJ_IF_SOME(h, exportedHandler) {
    auto emailHandler = KJ_ASSERT_NONNULL(handlerHandler.tryUnwrap(lock, h.self.getHandle(lock)));
    KJ_IF_SOME(f, emailHandler.email) {
      auto promise = f(lock, message.addRef(), h.env.addRef(isolate), h.getCtx()).then([&context] {
        KJ_IF_SOME(t, context.getWorkerTracer()) {
          t.setReturn(context.now());
        }
      });
      IoContext::current().addWaitUntil(kj::mv(promise));
    } else {
      lock.logWarningOnce("Received an EmailEvent but we lack a handler for EmailEvents. "
                          "Did you remember to export an email() function?");
      JSG_FAIL_REQUIRE(Error, "Handler does not export an email() function.");
    }
  } else {
    if (globalEventTarget.getHandlerCount("email") == 0) {
      lock.logWarningOnce("Received an EmailEvent but we lack an event listener for email events. "
                          "Did you remember to call addEventListener(\"email\", ...)?");
      JSG_FAIL_REQUIRE(Error, "No event listener registered for email messages.");
    }
    globalEventTarget.dispatchEventImpl(lock, js.alloc<EmailEvent>(message.addRef()));
  }
}

jsg::Promise<jsg::Optional<EmailSendResult>> SendEmail::send(
    jsg::Lock& js, kj::OneOf<jsg::Ref<EmailMessage>, EmailMessageBuilder> messageOrBuilder) {
  KJ_SWITCH_ONEOF(messageOrBuilder) {
    KJ_CASE_ONEOF(message, jsg::Ref<EmailMessage>) {
      auto enableQueuing = EnableEmailQueuing(FeatureFlags::get(js).getEmailSendingQueuing());
      auto transmission = sender->send(message->getFrom(), message->getTo(), enableQueuing);
      return driveMailSend(js, "send_email"_kjc, kj::mv(message), kj::mv(transmission));
    }
    KJ_CASE_ONEOF(message, EmailMessageBuilder) {
      size_t recipientTotal = 0;
      KJ_IF_SOME(to, message.to) {
        recipientTotal += recipientCount(to);
      }
      KJ_IF_SOME(cc, message.cc) {
        recipientTotal += recipientCount(cc);
      }
      KJ_IF_SOME(bcc, message.bcc) {
        recipientTotal += recipientCount(bcc);
      }
      JSG_REQUIRE(recipientTotal > 0, TypeError,
          "Email must have at least one recipient in \"to\", \"cc\", or \"bcc\".");

      auto transmission = sender->sendWithBuilder(message);
      return driveEmailBuilderSend(
          js, "send_email_builder"_kjc, kj::mv(message), kj::mv(transmission));
    }
  };
  KJ_UNREACHABLE;
}

void EmailMessage::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("from", from);
  tracker.trackField("to", to);
  raw.visitForMemoryInfo(tracker);
}

void EmailMessage::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(raw);
}

}  // namespace workerd::api
