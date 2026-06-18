// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "exception.h"

#include <kj/debug.h>

namespace workerd::jsg {

kj::StringPtr stripRemoteExceptionPrefix(kj::StringPtr internalMessage) {
  while (internalMessage.startsWith("remote exception: "_kj)) {
    // Exception was passed over RPC.
    internalMessage = internalMessage.slice("remote exception: "_kj.size());
  }
  return internalMessage;
}

namespace {
constexpr auto ERROR_PREFIX_DELIM = "; "_kj;
constexpr auto ERROR_REMOTE_PREFIX = "remote."_kj;
constexpr auto ERROR_TUNNELED_PREFIX_JSG = "jsg."_kj;
constexpr auto ERROR_INTERNAL_SOURCE_PREFIX_JSG = "jsg-internal."_kj;
constexpr auto ERROR_SENTRY_PREFIX = "SENTRY_"_kj;
constexpr auto ERROR_SENTRY_DO_PREFIX = "SENTRY_DO "_kj;
constexpr auto ERROR_SENTRY_RT_PREFIX = "SENTRY_RT "_kj;

kj::StringPtr stripSentryExceptionPrefix(kj::StringPtr internalMessage) {
  if (!internalMessage.startsWith(ERROR_SENTRY_PREFIX)) {
    return internalMessage;
  }

  if (internalMessage.startsWith(ERROR_SENTRY_DO_PREFIX)) {
    return internalMessage.slice(ERROR_SENTRY_DO_PREFIX.size());
  } else if (internalMessage.startsWith(ERROR_SENTRY_RT_PREFIX)) {
    return internalMessage.slice(ERROR_SENTRY_RT_PREFIX.size());
  }
  return internalMessage;
}

}  // namespace

TunneledErrorType tunneledErrorType(kj::StringPtr internalMessage) {
  // A tunneled error in an internal message is prefixed by one of the following patterns,
  // anchored at the beginning of the message:
  //   jsg.
  //   expected <...>; jsg.
  //   broken.<...>; jsg.
  // where <...> is some failed expectation from e.g. a KJ_REQUIRE.
  //
  // A tunneled error might have a prefix "remote.". This indicates it was tunneled from an actor or
  // from one worker to another. If this prefix is present, we set `isFromRemote` to true, remove
  // the "remote." prefix, and continue processing the rest of the error.
  //
  // Additionally, a prefix of `jsg-internal.` instead of `jsg.` means "throw a specific
  // JavaScript error type, but still hide the message text from the app".

  internalMessage = stripRemoteExceptionPrefix(internalMessage);

  struct Properties {
    bool isFromRemote = false;
    bool isDurableObjectReset = false;
    bool isDoNotLogException = false;
  };
  Properties properties;

  properties.isDoNotLogException = isDoNotLogException(internalMessage);

  // Remove `remote.` and any Sentry prefixes in either order. We can see multiple `remote.`
  // prefixes when an error crosses worker boundaries more than once.
  while (true) {
    auto strippedSentryPrefix = stripSentryExceptionPrefix(internalMessage);
    if (strippedSentryPrefix != internalMessage) {
      internalMessage = strippedSentryPrefix;
      continue;
    }

    if (internalMessage.startsWith(ERROR_REMOTE_PREFIX)) {
      properties.isFromRemote = true;
      internalMessage = internalMessage.slice(ERROR_REMOTE_PREFIX.size());
      continue;
    }

    break;
  }

  auto findDelim = [](kj::StringPtr msg) -> size_t {
    // Either return 0 if no matches or the index past the first delim if there are.
    KJ_IF_SOME(i, msg.find(ERROR_PREFIX_DELIM)) {
      return i + ERROR_PREFIX_DELIM.size();
    }
    return 0;
  };

  auto tryExtractError = [](kj::StringPtr msg,
                             Properties properties) -> kj::Maybe<TunneledErrorType> {
    if (msg.startsWith(ERROR_TUNNELED_PREFIX_JSG)) {
      return TunneledErrorType{
        .message = msg.slice(ERROR_TUNNELED_PREFIX_JSG.size()),
        .isJsgError = true,
        .isInternal = false,
        .isFromRemote = properties.isFromRemote,
        .isDurableObjectReset = properties.isDurableObjectReset,
        .isDoNotLogException = properties.isDoNotLogException,
      };
    }
    if (msg.startsWith(ERROR_INTERNAL_SOURCE_PREFIX_JSG)) {
      return TunneledErrorType{
        .message = msg.slice(ERROR_INTERNAL_SOURCE_PREFIX_JSG.size()),
        .isJsgError = true,
        .isInternal = true,
        .isFromRemote = properties.isFromRemote,
        .isDurableObjectReset = properties.isDurableObjectReset,
        .isDoNotLogException = properties.isDoNotLogException,
      };
    }

    return kj::none;
  };

  auto makeDefaultError = [](kj::StringPtr msg, Properties properties) {
    return TunneledErrorType{
      .message = msg,
      .isJsgError = false,
      .isInternal = true,
      .isFromRemote = properties.isFromRemote,
      .isDurableObjectReset = properties.isDurableObjectReset,
      .isDoNotLogException = isDoNotLogException(msg),
    };
  };

  if (internalMessage.startsWith("expected ")) {
    // This was a test assertion, peel away delimiters until either we find an error or there are
    // none left.
    auto idx = findDelim(internalMessage);
    while (idx) {
      internalMessage = internalMessage.slice(idx);
      KJ_IF_SOME(e, tryExtractError(internalMessage, properties)) {
        return kj::mv(e);
      }
      idx = findDelim(internalMessage);
    }

    // We failed to extract an expected error, make a default one.
    return makeDefaultError(internalMessage, properties);
  }

  while (internalMessage.startsWith("broken.")) {
    properties.isDurableObjectReset = true;

    // Trim away all broken prefixes, they are not allowed to have internal delimiters.
    internalMessage = internalMessage.slice(findDelim(internalMessage));
  }

  // There are no prefixes left, just try to extract the error.
  KJ_IF_SOME(e, tryExtractError(internalMessage, properties)) {
    return kj::mv(e);
  } else {
    return makeDefaultError(internalMessage, properties);
  }
}

bool isTunneledException(kj::StringPtr internalMessage) {
  return !tunneledErrorType(internalMessage).isInternal;
}

bool isDoNotLogException(kj::StringPtr internalMessage) {
  return internalMessage.contains("worker_do_not_log"_kjc);
}

kj::String annotateBroken(kj::StringPtr internalMessage, kj::StringPtr brokennessReason) {
  // TODO(soon) Once we support multiple brokenness reasons, we can make this much simpler.

  KJ_LOG(INFO, "Annotating with brokenness", internalMessage, brokennessReason);
  auto tunneledInfo = tunneledErrorType(internalMessage);

  kj::StringPtr remotePrefix;
  if (tunneledInfo.isFromRemote) {
    remotePrefix = ERROR_REMOTE_PREFIX;
  }

  kj::StringPtr prefixType = ERROR_TUNNELED_PREFIX_JSG;
  kj::StringPtr internalErrorType;
  if (tunneledInfo.isInternal) {
    prefixType = ERROR_INTERNAL_SOURCE_PREFIX_JSG;
    if (!tunneledInfo.isJsgError) {
      // This is not a JSG error, so we need to give it a type.
      internalErrorType = "Error: "_kj;
    }
  }

  return kj::str(remotePrefix, brokennessReason, ERROR_PREFIX_DELIM, prefixType, internalErrorType,
      tunneledInfo.message);
}

bool isExceptionFromInputGateBroken(kj::StringPtr description) {
  // annotateBroken() produces "broken.inputGateBroken; {message}", optionally prefixed with
  // "remote." when crossing RPC boundaries. Strip the remote prefix first, then check the tag.
  return stripRemoteExceptionPrefix(description).startsWith("broken.inputGateBroken; "_kj);
}

}  // namespace workerd::jsg
