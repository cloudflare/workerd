# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0x8b3d4aaa36221ec8;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd");
$Cxx.allowCancellation;

struct CompatibilityFlags @0x8f8c1b68151b6cef {
  # Flags that change the basic behavior of the runtime API, especially for
  # backwards-compatibility with old bugs.
  #
  # Note: At one time, this was called "FeatureFlags", and many places in the codebase still call
  #   it that. We could do a mass-rename but there's some tricky spots involving JSON
  #   communications with other systems... I'm leaving it for now.

  annotation compatEnableFlag @0xb6dabbc87cd1b03e (field) :Text;
  annotation compatDisableFlag @0xd145cf1adc42577c (field) :Text;
  # Compatibility flag names which enable or disable this feature, overriding what the worker's
  # compatibility date would otherwise set.
  #
  # An enable-flag is used to enable the feature before it becomes the default, probably for
  # testing purposes. All features probably should have an enable-flag defined first, then get
  # a date assigned once testing is done.
  #
  # A disable-flag is used when a worker needs to keep long-term backwards compatibility with one
  # bug but doesn't want to hold back everything else. This is hopefully rare! Most features
  # should not have a disable-flag defined unless we get requests from customers.

  annotation compatEnableDate @0x91a5d5d7244cf6d0 (field) :Text;
  # The compatibility date (date string, like "2021-05-17") after which this flag should always
  # be enabled.

  annotation compatEnableAllDates @0x9a1d37c8030d9418 (field) :Void;
  # All compatibility dates should start using the flag as enabled.
  # NOTE: This is almost NEVER what you actually want because you're most likely breaking back
  # compat. Note that workers uploaded with the flag will fail validation, so this will break
  # uploads for anyone still using the flag.
  #
  # However, this is useful in some cases where the back compat you're breaking is for
  # pre-release users who you've communicated this in advance to. Turning this on for a field
  # will also cause test failures in compatibility-date-test.c++ and validation-test.ekam-rule
  # because the default set of flags that are enabled changes from being the empty set to including
  # this flag (at some point presumably it also means you're removing the compat flags at some point
  # and you'll have to repeat this exercise).

  annotation neededByFl @0xbd23aff9deefc308 (field) :Void;
  # A tag to tell us which fields we'll need to propagate to FL on subrequests and responses.
  #
  # ("FL" refers to Cloudflare's HTTP proxy stack which is used for all outbound requests. Except
  # for `brotliContentEncoding`, flags with this annotation have no effect when `workerd` is used
  # outside of Cloudflare.)

  annotation experimental @0xe3e5a63e76284d88 (field):Void;
  # Flags with this annotation can only be used when workerd is run with the --experimental flag.
  # These flags may be subject to change or even removal in the future with no warning -- they are
  # not covered by Workers' usual backwards-compatibility promise. Experimental flags cannot be
  # used in Workers deployed on Cloudflare except by test accounts belonging to Cloudflare team
  # members.

  formDataParserSupportsFiles @0 :Bool
      $compatEnableFlag("formdata_parser_supports_files")
      $compatEnableDate("2021-11-03")
      $compatDisableFlag("formdata_parser_converts_files_to_strings");
  # Our original implementations of FormData made the mistake of turning files into strings.
  # We hadn't implemented `File` yet, and we forgot.

  fetchRefusesUnknownProtocols @1 :Bool
      $compatEnableFlag("fetch_refuses_unknown_protocols")
      $compatEnableDate("2021-11-10")
      $compatDisableFlag("fetch_treats_unknown_protocols_as_http");
  # Our original implementation of fetch() incorrectly accepted URLs with any scheme (protocol).
  # It happily sent any scheme to FL in the `X-Forwarded-Proto` header. FL would ignore any
  # scheme it didn't recgonize, which effectively meant it treated all schemes other than `https`
  # as `http`.

  esiIncludeIsVoidTag @2 :Bool
      $compatEnableFlag("html_rewriter_treats_esi_include_as_void_tag");
  # Our original implementation of `esi:include` treated it as needing an end tag.
  # We're worried that fixing this could break existing workers.

  obsolete3 @3 :Bool;

  durableObjectFetchRequiresSchemeAuthority @4 :Bool
      $compatEnableFlag("durable_object_fetch_requires_full_url")
      $compatEnableDate("2021-11-10")
      $compatDisableFlag("durable_object_fetch_allows_relative_url");
  # Our original implementation allowed URLs without schema and/or authority components and
  # interpreted them relative to "https://fake-host/".

  streamsByobReaderDetachesBuffer @5 :Bool
      $compatEnableFlag("streams_byob_reader_detaches_buffer")
      $compatEnableDate("2021-11-10")
      $compatDisableFlag("streams_byob_reader_does_not_detach_buffer");
  # The streams specification dictates that ArrayBufferViews that are passed in to the
  # read() operation of a ReadableStreamBYOBReader are detached, making it impossible
  # for user code to modify or observe the data as it is being read. Our original
  # implementation did not do that.

  streamsJavaScriptControllers @6 :Bool
      $compatEnableFlag("streams_enable_constructors")
      $compatEnableDate("2022-11-30")
      $compatDisableFlag("streams_disable_constructors");
  # Controls the availability of the work in progress new ReadableStream() and
  # new WritableStream() constructors backed by JavaScript underlying sources
  # and sinks.

  jsgPropertyOnPrototypeTemplate @7 :Bool
      $compatEnableFlag("workers_api_getters_setters_on_prototype")
      $compatEnableDate("2022-01-31")
      $compatDisableFlag("workers_api_getters_setters_on_instance");
  # Originally, JSG_PROPERTY registered getter/setters on an objects *instance*
  # template as opposed to it's prototype template. This broke subclassing at
  # the JavaScript layer, preventing a subclass from correctly overriding the
  # superclasses getters/setters. This flag controls the breaking change made
  # to set those getters/setters on the prototype template instead.

  minimalSubrequests @8 :Bool
      $compatEnableFlag("minimal_subrequests")
      $compatEnableDate("2022-04-05")
      $compatDisableFlag("no_minimal_subrequests")
      $neededByFl;
  # Turns off a bunch of redundant features for outgoing subrequests from Cloudflare. Historically,
  # a number of standard Cloudflare CDN features unrelated to Workers would sometimes run on
  # Workers subrequests despite not making sense there. For example, Cloudflare might automatically
  # apply gzip compression when the origin server responds without it but the client supports it.
  # This doesn't make sense to do when the response is going to be consumed by Workers, since the
  # Workers Runtime is typically running on the same machine. When Workers itself returns a
  # response to the client, Cloudflare's stack will have another chance to apply gzip, and it
  # makes much more sense to do there.

  noCotsOnExternalFetch @9 :Bool
      $compatEnableFlag("no_cots_on_external_fetch")
      $compatEnableDate("2022-03-08")
      $compatDisableFlag("cots_on_external_fetch")
      $neededByFl;
  # Tells FL not to use the Custom Origin Trust Store for grey-cloud / external subrequests. Fixes
  # EW-5299.

  specCompliantUrl @10 :Bool
      $compatEnableFlag("url_standard")
      $compatEnableDate("2022-10-31")
      $compatDisableFlag("url_original");
  # The original URL implementation based on kj::Url is not compliant with the
  # WHATWG URL Standard, leading to a number of issues reported by users. Unfortunately,
  # making it spec compliant is a breaking change. This flag controls the availability
  # of the new spec-compliant URL implementation.

  globalNavigator @11 :Bool
      $compatEnableFlag("global_navigator")
      $compatEnableDate("2022-03-21")
      $compatDisableFlag("no_global_navigator");

  captureThrowsAsRejections @12 :Bool
      $compatEnableFlag("capture_async_api_throws")
      $compatEnableDate("2022-10-31")
      $compatDisableFlag("do_not_capture_async_api_throws");
  # Many worker APIs that return JavaScript promises currently throw synchronous errors
  # when exceptions occur. Per the Web Platform API specs, async functions should never
  # throw synchronously. This flag changes the behavior so that async functions return
  # rejections instead of throwing.

  r2PublicBetaApi @13 :Bool
      $compatEnableFlag("r2_public_beta_bindings")
      $compatDisableFlag("r2_internal_beta_bindings")
      $compatEnableAllDates;
  # R2 public beta bindings are the default.
  # R2 internal beta bindings is back-compat.

  obsolete14 @14 :Bool
      $compatEnableFlag("durable_object_alarms");
  # Used to gate access to durable object alarms to authorized workers, no longer used (alarms
  # are now enabled for everyone).

  noSubstituteNull @15 :Bool
      $compatEnableFlag("dont_substitute_null_on_type_error")
      $compatEnableDate("2022-06-01")
      $compatDisableFlag("substitute_null_on_type_error");
  # There is a bug in the original implementation of the kj::Maybe<T> type wrapper
  # that had it inappropriately interpret a nullptr result as acceptable as opposed
  # to it being a type error. Enabling this flag switches on the correct behavior.
  # Disabling the flag switches back to the original broken behavior.

  transformStreamJavaScriptControllers @16 :Bool
      $compatEnableFlag("transformstream_enable_standard_constructor")
      $compatEnableDate("2022-11-30")
      $compatDisableFlag("transformstream_disable_standard_constructor");
  # Controls whether the TransformStream constructor conforms to the stream standard or not.
  # Must be used in combination with the streamsJavaScriptControllers flag.

  r2ListHonorIncludeFields @17 :Bool
      $compatEnableFlag("r2_list_honor_include")
      $compatEnableDate("2022-08-04");
  # Controls if R2 bucket.list honors the `include` field as intended. It previously didn't
  # and by default would result in http & custom metadata for an object always being returned
  # in the list.

  exportCommonJsDefaultNamespace @18 :Bool
      $compatEnableFlag("export_commonjs_default")
      $compatEnableDate("2022-10-31")
      $compatDisableFlag("export_commonjs_namespace");
  # Unfortunately, when the CommonJsModule type was implemented, it mistakenly exported the
  # module namespace (an object like `{default: module.exports}`) rather than exporting only
  # the module.exports. When this flag is enabled, the export is fixed.

  obsolete19 @19 :Bool
      $compatEnableFlag("durable_object_rename")
      $experimental;
  # Obsolete flag. Has no effect.

  webSocketCompression @20 :Bool
      $compatEnableFlag("web_socket_compression")
      $compatEnableDate("2023-08-15")
      $compatDisableFlag("no_web_socket_compression");
  # Enables WebSocket compression. Without this flag, all attempts to negotiate compression will
  # be refused for scripts prior to the compat date, so WebSockets will never use compression.
  # With this flag, the system will automatically negotiate the use of the permessage-deflate
  # extension where appropriate. The Worker can also request specific compression settings by
  # specifying a valid Sec-WebSocket-Extensions header, or setting the header to the empty string
  # to explicitly request that no compression be used.

  nodeJsCompat @21 :Bool
      $compatEnableFlag("nodejs_compat")
      $compatDisableFlag("no_nodejs_compat");
  # Enables nodejs compat imports in the application.

  obsolete22 @22 :Bool
      $compatEnableFlag("tcp_sockets_support");
  # Used to enables TCP sockets in workerd.

  specCompliantResponseRedirect @23 :Bool
      $compatEnableDate("2023-03-14")
      $compatEnableFlag("response_redirect_url_standard")
      $compatDisableFlag("response_redirect_url_original");
  # The original URL implementation based on kj::Url is not compliant with the
  # WHATWG URL Standard, leading to a number of issues reported by users. Unfortunately,
  # the specCompliantUrl flag did not contemplate the redirect usage. This flag is
  # specifically about the usage in a redirect().

  workerdExperimental @24 :Bool
      $compatEnableFlag("experimental")
      $experimental;
  # Experimental, do not use.
  # This is a catch-all compatibility flag for experimental development within workerd
  # that is not covered by another more-specific compatibility flag. It is the intention
  # of this flag to always have the $experimental attribute.
  # This is intended to guard new features that do not introduce any backwards-compatibility
  # concerns (e.g. they only add a new API), and therefore do not need a compat flag of their own
  # in the long term, but where we still want to guard access to the feature while it is in
  # development. Don't use this for backwards-incompatible changes; give them their own flag.
  # WARNING: Any feature blocked by this flag is subject to change at any time, including
  # removal. Do not ignore this warning.

  durableObjectGetExisting @25 :Bool
      $compatEnableFlag("durable_object_get_existing")
      $experimental;
  # Experimental, allows getting a durable object stub that ensures the object already exists.
  # This is currently a work in progress mechanism that is not yet available for use in workerd.

  httpHeadersGetSetCookie @26 :Bool
      $compatEnableFlag("http_headers_getsetcookie")
      $compatDisableFlag("no_http_headers_getsetcookie")
      $compatEnableDate("2023-03-01");
  # Enables the new headers.getSetCookie() API and the corresponding changes in behavior for
  # the Header objects keys() and entries() iterators.

  dispatchExceptionTunneling @27 :Bool
      $compatEnableDate("2023-03-01")
      $compatEnableFlag("dynamic_dispatch_tunnel_exceptions")
      $compatDisableFlag("dynamic_dispatch_treat_exceptions_as_500");
  # Enables the tunneling of exceptions from a dynamic dispatch callee back into the caller.
  # Previously any uncaught exception in the callee would be returned to the caller as an empty
  # HTTP 500 response.

  serviceBindingExtraHandlers @28 :Bool
      $compatEnableFlag("service_binding_extra_handlers")
      $experimental;
  # Allows service bindings to call additional event handler methods on the target Worker.
  # Initially only includes support for calling the queue() handler.
  # WARNING: this flag exposes the V8 deserialiser to users via `Fetcher#queue()` `serializedBody`.
  # Historically, this has required a trusted environment to be safe. If we decide to make this
  # flag non-experimental, we must ensure we take appropriate precuations.

  noCfBotManagementDefault @29 :Bool
      $compatEnableFlag("no_cf_botmanagement_default")
      $compatDisableFlag("cf_botmanagement_default")
      $compatEnableDate("2023-08-01");
  # This one operates a bit backwards. With the flag *enabled* no default cfBotManagement
  # data will be included. The the flag *disable*, default cfBotManagement data will be
  # included in the request.cf if the field is not present.

  urlSearchParamsDeleteHasValueArg @30 :Bool
      $compatEnableFlag("urlsearchparams_delete_has_value_arg")
      $compatDisableFlag("no_urlsearchparams_delete_has_value_arg")
      $compatEnableDate("2023-07-01");
  # When enabled, the delete() and has() methods of the standard URLSearchParams object
  # (see url-standard.h) will have the recently added second value argument enabled.

  strictCompression @31 :Bool
      $compatEnableFlag("strict_compression_checks")
      $compatDisableFlag("no_strict_compression_checks")
      $compatEnableDate("2023-08-01");
  # Perform additional error checking in the Web Compression API and throw an error if a
  # DecompressionStream has trailing data or gets closed before the full compressed data has been
  # provided.

  brotliContentEncoding @32 :Bool
      $compatEnableFlag("brotli_content_encoding")
      $compatEnableDate("2024-04-29")
      $compatDisableFlag("no_brotli_content_encoding")
      $neededByFl;
  # Enables compression/decompression support for the brotli compression algorithm.
  # With the flag enabled workerd will support the "br" content encoding in the Request and
  # Response APIs and compress or decompress data accordingly as with gzip.

  strictCrypto @33 :Bool
      $compatEnableFlag("strict_crypto_checks")
      $compatDisableFlag("no_strict_crypto_checks")
      $compatEnableDate("2023-08-01");
  # Perform additional error checking in the Web Crypto API to conform with the specification as
  # well as reject key parameters that may be unsafe based on key length or public exponent.

  rttiApi @34 :Bool
      $compatEnableFlag("rtti_api")
      $experimental;
  # Enables the `workerd:rtti` module for querying runtime-type-information from JavaScript.

  webgpu @35 :Bool
      $compatEnableFlag("webgpu")
      $experimental;

  cryptoPreservePublicExponent @36 :Bool
      $compatEnableFlag("crypto_preserve_public_exponent")
      $compatDisableFlag("no_crypto_preserve_public_exponent")
      $compatEnableDate("2023-12-01");
  # In the WebCrypto API, the `publicExponent` field of the algorithm of RSA keys would previously
  # be an ArrayBuffer. Using this flag, publicExponent is a Uint8Array as mandated by the
  # specification.

  vectorizeQueryMetadataOptional @37 :Bool
      $compatEnableFlag("vectorize_query_metadata_optional")
      $compatEnableDate("2023-11-08")
      $compatDisableFlag("vectorize_query_original");
  # Vectorize query option change to allow returning of metadata to be optional. Accompanying this:
  # a return format change to move away from a nested object with the VectorizeVector.

  unsafeModule @38 :Bool
      $compatEnableFlag("unsafe_module")
      $experimental;
  # Enables the `workerd:unsafe` module for performing dangerous operations from JavaScript.
  # Intended for local development and testing use cases. Currently just supports aborting all
  # Durable Objects running in a `workerd` process.

  jsRpc @39 :Bool
      $compatEnableFlag("js_rpc")
      $experimental;
  # Enables JS RPC on the server side for Durable Object classes that do not explicitly extend
  # `DurableObjects`.
  #
  # This flag is obsolete but supported temporarily to avoid breaking people who used it. All code
  # should switch to using `extends DurableObject` as the way to enable RPC.
  #
  # As of this writing, it is still necessary to enable the general `experimental` flag to use RPC
  # on both the client and server sides.

  noImportScripts @40 :Bool
      $compatEnableFlag("no_global_importscripts")
      $compatDisableFlag("global_importscripts")
      $compatEnableDate("2024-03-04");
  # Removes the non-implemented importScripts() function from the global scope.

  nodeJsAls @41 :Bool
      $compatEnableFlag("nodejs_als")
      $compatDisableFlag("no_nodejs_als");
  # Enables the availability of the Node.js AsyncLocalStorage API independently of the full
  # node.js compatibility option.

  queuesJsonMessages @42 :Bool
      $compatEnableFlag("queues_json_messages")
      $compatDisableFlag("no_queues_json_messages")
      $compatEnableDate("2024-03-18");
  # Queues bindings serialize messages to JSON format by default (the previous default was v8 format)

  pythonWorkers @43 :Bool
      $compatEnableFlag("python_workers");
  # Enables Python Workers. Access to this flag is not restricted, instead bundles containing
  # Python modules are restricted in EWC.
  #
  # WARNING: Python Workers are still an experimental feature and thus subject to change.

  fetcherNoGetPutDelete @44 :Bool
      $compatEnableFlag("fetcher_no_get_put_delete")
      $compatDisableFlag("fetcher_has_get_put_delete")
      $compatEnableDate("2024-03-26");
  # Historically, the `Fetcher` type -- which is the type of Service Bindings, and also the parent
  # type of Durable Object stubs -- had special methods `get()`, `put()`, and `delete()`, which
  # were shortcuts for calling `fetch()` with the corresponding HTTP method. These methods were
  # never documented.
  #
  # To make room for people to define their own RPC methods with these names, this compat flag
  # makes them no longer defined.

  unwrapCustomThenables @45 :Bool
      $compatEnableFlag("unwrap_custom_thenables")
      $compatDisableFlag("no_unwrap_custom_thenables")
      $compatEnableDate("2024-04-01");

  fetcherRpc @46 :Bool
      $compatEnableFlag("rpc")
      $compatDisableFlag("no_rpc")
      $compatEnableDate("2024-04-03");
  # Whether the type `Fetcher` type -- which is the type of Service Bindings, and also the parent
  # type of Durable Object stubs -- support RPC. If so, this type will have a wildcard method, so
  # it will appear that all possible property names are present on any fetcher instance. This could
  # break code that tries to infer types based on the presence or absence of methods.

  internalStreamByobReturn @47 :Bool
      $compatEnableFlag("internal_stream_byob_return_view")
      $compatDisableFlag("internal_stream_byob_return_undefined")
      $compatEnableDate("2024-05-13");
  # Sadly, the original implementation of ReadableStream (now called "internal" streams), did not
  # properly implement the result of ReadableStreamBYOBReader's read method. When done = true,
  # per the spec, the result `value` must be an empty ArrayBufferView whose underlying ArrayBuffer
  # is the same as the one passed to the read method. Our original implementation returned
  # undefined instead. This flag changes the behavior to match the spec and to match the behavior
  # implemented by the JS-backed ReadableStream implementation.

  blobStandardMimeType @48 :Bool
      $compatEnableFlag("blob_standard_mime_type")
      $compatDisableFlag("blob_legacy_mime_type")
      $compatEnableDate("2024-06-03");
  # The original implementation of the Blob mime type normalization when extracting a blob
  # from the Request or Response body is not compliant with the standard. Unfortunately,
  # making it compliant is a breaking change. This flag controls the availability of the
  # new spec-compliant Blob mime type normalization.

  fetchStandardUrl @49 :Bool
    $compatEnableFlag("fetch_standard_url")
    $compatDisableFlag("fetch_legacy_url")
    $compatEnableDate("2024-06-03");
  # Ensures that WHATWG standard URL parsing is used in the fetch API implementation.
}
