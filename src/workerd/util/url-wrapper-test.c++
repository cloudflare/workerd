#include <kj/test.h>
#include "url-wrapper.h"

KJ_TEST("Legacy Basics") {
  auto url = workerd::UrlWrapper::legacy("http://example.com/a?1#foo");
  KJ_ASSERT(url.toString() == "http://example.com/a?1#foo");
  auto cloned = url.clone();
  KJ_ASSERT(url.toString() == cloned.toString());

  KJ_ASSERT(url.toString(workerd::UrlWrapper::ToStringOptions {
    .includeFragments = false
  }) == "http://example.com/a?1");

  KJ_ASSERT(url.toString(workerd::UrlWrapper::ToStringOptions {
    .context = kj::Url::Context::HTTP_PROXY_REQUEST
  }) == "http://example.com/a?1");

  KJ_ASSERT(url.toString(workerd::UrlWrapper::ToStringOptions {
    .context = kj::Url::Context::HTTP_REQUEST
  }) == "/a?1");

  KJ_ASSERT(workerd::UrlWrapper::kLegacyFake.resolve("/foo/bar").toString() ==
            "https://fake-host/foo/bar");
}

KJ_TEST("Standard Basics") {
  auto url = workerd::UrlWrapper::standard(" http://example.\t\ncom/a?1#foo ");
  KJ_ASSERT(url.toString() == "http://example.com/a?1#foo");
  auto cloned = url.clone();
  KJ_ASSERT(url.toString() == cloned.toString());

  KJ_ASSERT(url.toString(workerd::UrlWrapper::ToStringOptions {
    .includeFragments = false
  }) == "http://example.com/a?1");

  KJ_ASSERT(url.toString(workerd::UrlWrapper::ToStringOptions {
    .context = kj::Url::Context::HTTP_PROXY_REQUEST
  }) == "http://example.com/a?1" );

  KJ_ASSERT(url.toString(workerd::UrlWrapper::ToStringOptions {
    .context = kj::Url::Context::HTTP_REQUEST
  }) == "/a?1" );

  KJ_ASSERT(workerd::UrlWrapper::kStandardFake.resolve("  /foo\t/bar  ").toString() ==
            "https://fake-host/foo/bar");
}

KJ_TEST("URLList") {
  workerd::UrlList list;
  KJ_ASSERT(list.size() == 0);
  list.add(workerd::UrlWrapper::standard("https://example.com/1"));
  list.add(workerd::UrlWrapper::standard("https://example.com/2"));
  KJ_ASSERT(list.size() == 2);
  auto& back = KJ_ASSERT_NONNULL(list.back());
  KJ_ASSERT(back.toString() == "https://example.com/2");
}
