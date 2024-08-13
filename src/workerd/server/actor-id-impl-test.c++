// Copyright (c) 2024-2029 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <openssl/hmac.h>
#include <workerd/jsg/exception.h>
#include <workerd/server/actor-id-impl.h>

#include <kj/debug.h>
#include <kj/encoding.h>
#include <kj/test.h>

constexpr kj::byte zero32[SHA256_DIGEST_LENGTH] = {
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
KJ_TEST("ActorIdImpl equals test") {
  using ActorIdImpl = workerd::server::ActorIdFactoryImpl::ActorIdImpl;
  struct ActorEqualsTest {
    ActorIdImpl actorLeft = {zero32, kj::none};
    ActorIdImpl actorRight = {zero32, kj::none};
    bool expectedResult;
    ActorEqualsTest(kj::byte leftFill,
        const char* leftString,
        kj::byte rightFill,
        const char* rightString,
        bool expectedResult)
        : expectedResult(expectedResult) {
      kj::byte idParamCopier[SHA256_DIGEST_LENGTH];
      memset(idParamCopier, leftFill, SHA256_DIGEST_LENGTH);
      if (leftString == nullptr) {
        actorLeft = ActorIdImpl(idParamCopier, kj::none);
      } else {
        actorLeft = ActorIdImpl(idParamCopier, kj::heapString(leftString));
      }
      memset(idParamCopier, rightFill, SHA256_DIGEST_LENGTH);
      if (rightString == nullptr) {
        actorRight = ActorIdImpl(idParamCopier, kj::none);
      } else {
        actorRight = ActorIdImpl(idParamCopier, kj::heapString(rightString));
      }
    }
  };
  using Test = ActorEqualsTest;
  Test testCases[] = {
    {0, nullptr, 0, nullptr, true},
    {0, nullptr, 1, nullptr, false},
    {0, "hello", 0, "goodbye", true},
    {0, "hello", 1, "goodbye", false},
    {0, "hello", 0, nullptr, true},
    {0, "hello", 1, nullptr, false},
  };
  for (const auto& testCase: testCases) {
    KJ_EXPECT(testCase.actorLeft.equals(testCase.actorRight) == testCase.expectedResult);
  }
}

constexpr size_t BASE_LENGTH = SHA256_DIGEST_LENGTH / 2;
kj::String computeProperTestMac(const char* strId, const char* strKey) {
  auto id = kj::decodeHex(kj::heapString(strId));
  KJ_ASSERT(!id.hadErrors);
  KJ_ASSERT(id.size() == SHA256_DIGEST_LENGTH);
  kj::byte key[SHA256_DIGEST_LENGTH];
  auto stringPtrKey = kj::StringPtr(strKey);
  SHA256(stringPtrKey.asBytes().begin(), stringPtrKey.size(), key);
  kj::byte hmacOut[SHA256_DIGEST_LENGTH];
  unsigned int len = SHA256_DIGEST_LENGTH;
  HMAC(EVP_sha256(), key, sizeof(key), id.begin(), BASE_LENGTH, hmacOut, &len);
  KJ_ASSERT(len == SHA256_DIGEST_LENGTH);
  auto ret = kj::heapArray<kj::byte>(SHA256_DIGEST_LENGTH);
  memcpy(ret.begin(), id.begin(), BASE_LENGTH);
  memcpy(ret.begin() + BASE_LENGTH, hmacOut, SHA256_DIGEST_LENGTH - BASE_LENGTH);
  return kj::encodeHex(ret);
}

constexpr const char deadbeef64[] =
    "deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef";
KJ_TEST("ActorIdImplFactory idFromString test") {
  using ActorIdFactoryImpl = workerd::server::ActorIdFactoryImpl;
  struct ActorFactoryFromStringTest {
    ActorIdFactoryImpl actor;
    kj::String string;
    bool isFatal;
    ActorFactoryFromStringTest(const char* actorString, const char* string, bool isFatal)
        : actor(actorString),
          string(kj::heapString(string)),
          isFatal(isFatal) {}
    ActorFactoryFromStringTest(const char* actorString, kj::String string, bool isFatal)
        : actor(actorString),
          string(kj::mv(string)),
          isFatal(isFatal) {}
  };
  using Test = ActorFactoryFromStringTest;
  Test testCases[] = {
    {"hello", "goodbye", true},   // a random string of the wrong length
    {"hello", deadbeef64, true},  //Gets past the first assert
    {deadbeef64, computeProperTestMac(deadbeef64, deadbeef64),
      false},  //Gets past the second assert
  };
  for (auto& testCase: testCases) {
    if (testCase.isFatal) {
      KJ_EXPECT_THROW(FAILED, testCase.actor.idFromString(kj::heapString(testCase.string)));
    } else {
      auto result = testCase.actor.idFromString(kj::heapString(testCase.string));
      KJ_EXPECT(result->getName() == kj::none);
    }
  }
}
