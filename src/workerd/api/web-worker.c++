#include <kj/async.h>
#include <kj/common.h>

#include "web-worker.h"
#include <capnp/compat/json.h>
#include <workerd/api/basics.h>
#include <workerd/api/web-worker-api.capnp.h>

namespace workerd::api {

jsg::Ref<WebWorker> WebWorker::constructor(kj::String aUrl, jsg::Optional<Options> options) {
  auto& context = IoContext::current();

  auto client = context.getHttpClient(IoContext::SELF_CLIENT_CHANNEL, true, nullptr, "create"_kj);

  auto headers = kj::HttpHeaders(context.getHeaderTable());
  headers.set(kj::HttpHeaderId::CONTENT_TYPE, "application/json");

  capnp::JsonCodec json;
  json.handleByAnnotation<experimental::CreateWorkerRequest>();
  capnp::MallocMessageBuilder requestMessage;

  auto requestBuilder = requestMessage.initRoot<experimental::CreateWorkerRequest>();
  requestBuilder.setUrl(aUrl);
  KJ_IF_MAYBE(opts, options) {
    /* auto optionsBuilder = requestBuilder.initOptions(); */
  }

  auto requestJson = json.encode(requestBuilder);

  auto req = client->request(kj::HttpMethod::POST, "", headers);
  return jsg::alloc<WebWorker>(*kj::heap(req.body->write(requestJson.begin(), requestJson.size())
      .attach(kj::mv(requestJson))
      .then([resp = kj::mv(req.response),&context]() mutable {
        return resp.then([&context](kj::HttpClient::Response&& response) mutable
            -> kj::Promise<uint> {
            if (response.statusCode >= 400) {
            /* auto error = KJ_ASSERT_NONNULL(response.headers->get( */
            /*     context.getHeaderIds().cfR2ErrorHeader)); */
            KJ_DBG("uh oh");
            /* return kj::reject(nullptr); */
            }
            return kj::Promise<uint>(42);
            /* return response.body->readAllText().attach(kj::mv(response.body)).then( */
            /*     [statusCode = response.statusCode, statusText = kj::str(response.statusText)] */
            /*     (kj::String responseBody) mutable -> R2Result { */
            /*   return R2Result{ */
            /*     .httpStatus = statusCode, */
            /*     .metadataPayload = responseBody.releaseArray(), */
            /*   }; */
            /* }); */
            });
        })));
}

kj::Promise<void> WebWorker::postMessage(kj::String message) {
  return kj::Promise<void>(nullptr);
}

}  // namespace workerd::api
