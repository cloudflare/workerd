# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0x8b3d4aaa36221ec8;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd");
$Cxx.allowCancellation;

struct ImpliedByAfterDate @0x8f8c1b68151b6cff {
  # Annotates a compatibility flag to indicate that it is implied by the enablement
  # of the named flag(s) after the specified date.
  union {
    name @0 :Text;
    names @2 :List(Text);
  }
  date @1 :Text;
}

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
  # should have a disable-flag defined.

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

  annotation impliedByAfterDate @0xe3e5a63e76284d89 (field) :ImpliedByAfterDate;

  annotation pythonSnapshotRelease @0xef74c0cc5d18cc0c (field) :Void;
  # This annotation marks a compat flag as introducing a potentially breaking change to Python
  # memory snapshots. See the doc comment for the `PythonSnapshotRelease` struct above for more
  # details.

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
  # template as opposed to its prototype template. This broke subclassing at
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

  obsolete35 @35 :Bool
      $compatEnableFlag("webgpu")
      $experimental;
  # The experimental webgpu API was removed.

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
      $compatEnableFlag("python_workers")
      $pythonSnapshotRelease
      $impliedByAfterDate(name = "pythonWorkersDevPyodide", date = "2000-01-01");
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

  nodeJsCompatV2 @50 :Bool
      $compatEnableFlag("nodejs_compat_v2")
      $compatDisableFlag("no_nodejs_compat_v2")
      $impliedByAfterDate(name = "nodeJsCompat", date = "2024-09-23");
  # Implies nodeJSCompat with the following additional modifications:
  # * Node.js Compat built-ins may be imported/required with or without the node: prefix
  # * Node.js Compat the globals Buffer and process are available everywhere

  globalFetchStrictlyPublic @51 :Bool
      $compatEnableFlag("global_fetch_strictly_public")
      $compatDisableFlag("global_fetch_private_origin");
  # Controls what happens when a Worker hosted on Cloudflare uses the global `fetch()` function to
  # request a hostname that is within the Worker's own Cloudflare zone (domain).
  #
  # Historically, such requests would be routed to the zone's origin server, ignoring any Workers
  # mapped to the URL and also bypassing Cloudflare security settings. This behavior made sense
  # when Workers was first introduced as a way to rewrite requests before passing them along to
  # the origin, and bindings didn't exist: the only way to forward the request to origin was to
  # use global fetch(), and if it didn't bypass Workers, you'd end up looping back to the same
  # Worker.
  #
  # However, this behavior has a problem: it opens the door for SSRF attacks. Imagine a Worker is
  # designed to fetch a resource from a user-provided URL. An attacker could provide a URL that
  # points back to the Worker's own zone, and possibly cause the Worker to fetch a resource from
  # origin that isn't meant to be reachable by the public.
  #
  # Traditionally, this kind of attack is considered a bug in the application: an application that
  # fetches untrusted URLs must verify that the URL doesn't refer to a private resource that only
  # the application itself is meant to access. However, applications can easily get this wrong.
  # Meanwhile, by using bindings, we can make this class of problem go away.
  #
  # When global_fetch_strictly_public is enabled, the global `fetch()` function (when invoked on
  # Cloudflare Workers) will strictly route requests as if they were made on the public internet.
  # Thus, requests to a Worker's own zone will loop back to the "front door" of Cloudflare and
  # will be treated like a request from the internet, possibly even looping back to the same Worker
  # again. If an application wishes to send requests to its origin, it must configure an "origin
  # binding". An origin binding behaves like a service binding (it has a `fetch()` method) but
  # sends requests to the zone's origin servers, bypassing Cloudflare. E.g. the Worker would write
  # `env.ORIGIN.fetch(req)` to send a request to its origin.
  #
  # Note: This flag only impacts behavior on Cloudflare. It has no effect when using workerd.
  # Under workerd, the config file can control where global `fetch()` goes by configuring the
  # worker's `globalOutbound` implicit binding. By default, under workerd, global `fetch()` has
  # always been configured to accept publicly-routable internet hosts only; hostnames which map
  # to private IP addresses (as defined in e.g. RFC 1918) will be rejected. Thus, workerd has
  # always been SSRF-safe by default.

  newModuleRegistry @52 :Bool
      $compatEnableFlag("new_module_registry")
      $compatDisableFlag("legacy_module_registry")
      $experimental;
  # Enables of the new module registry implementation.

  cacheOptionEnabled @53 :Bool
    $compatEnableFlag("cache_option_enabled")
    $compatDisableFlag("cache_option_disabled")
    $compatEnableDate("2024-11-11");
  # Enables the use of no-store headers from requests

  kvDirectBinding @54 :Bool
      $compatEnableFlag("kv_direct_binding")
      $experimental;
  # Enables bypassing FL by translating pipeline tunnel configuration to subpipeline.
  # This flag is used only by the internal repo and not directly by workerd.

  allowCustomPorts @55 :Bool
      $compatEnableFlag("allow_custom_ports")
      $compatDisableFlag("ignore_custom_ports")
      $compatEnableDate("2024-09-02")
      $neededByFl;
  # Enables fetching hosts with a custom port from workers.
  # For orange clouded sites only standard ports are allowed (https://developers.cloudflare.com/fundamentals/reference/network-ports/#network-ports-compatible-with-cloudflares-proxy).
  # For grey clouded sites all ports are allowed.

  increaseWebsocketMessageSize @56 :Bool
      $compatEnableFlag("increase_websocket_message_size")
      $experimental;
  # For local development purposes only, increase the message size limit to 128MB.
  # This is not expected ever to be made available in production, as large messages are inefficient.

  internalWritableStreamAbortClearsQueue @57 :Bool
      $compatEnableFlag("internal_writable_stream_abort_clears_queue")
      $compatDisableFlag("internal_writable_stream_abort_does_not_clear_queue")
      $compatEnableDate("2024-09-02");
  # When using the original WritableStream implementation ("internal" streams), the
  # abort() operation would be handled lazily, meaning that the queue of pending writes
  # would not be cleared until the next time the queue was processed. This behavior leads
  # to a situtation where the stream can hang if the consumer stops consuming. When set,
  # this flag changes the behavior to clear the queue immediately upon abort.

  pythonWorkersDevPyodide @58 :Bool
    $compatEnableFlag("python_workers_development")
    $pythonSnapshotRelease
    $experimental;
  # Enables Python Workers and uses the bundle from the Pyodide source directory directly. For testing only.
  #
  # Note that the baseline snapshot hash here refers to the one used in
  # `baseline-from-gcs.ew-test-bin.c++`. We don't intend to ever load it in production.

  nodeJsZlib @59 :Bool
      $compatEnableFlag("nodejs_zlib")
      $compatDisableFlag("no_nodejs_zlib")
      $impliedByAfterDate(names = ["nodeJsCompat", "nodeJsCompatV2"], date = "2024-09-23");
  # Enables node:zlib implementation while it is in-development.
  # Once the node:zlib implementation is complete, this will be automatically enabled when
  # nodejs_compat or nodejs_compat_v2 are enabled.

  replicaRouting @60 :Bool
      $compatEnableFlag("replica_routing")
      $experimental;
  # Enables routing to a replica on the client-side.
  # Doesn't mean requests *will* be routed to a replica, only that they can be.

  obsolete61 @61 :Bool
      $compatEnableFlag("enable_d1_with_sessions_api")
      $experimental;
  # Was used to enable the withSession(bookmarkOrConstraint) method that allows users
  # to use read-replication Sessions API for D1. This is now enabled for everyone.

  handleCrossRequestPromiseResolution @62 :Bool
      $compatEnableFlag("handle_cross_request_promise_resolution")
      $compatDisableFlag("no_handle_cross_request_promise_resolution")
      $compatEnableDate("2024-10-14");
  # Historically, it has been possible to resolve a promise from an incorrect request
  # IoContext. This leads to issues with promise continuations being scheduled to run
  # in the wrong IoContext leading to errors and difficult to diagnose bugs. With this
  # compatibility flag we arrange to have such promise continuations scheduled to run
  # in the correct IoContext if it is still alive, or dropped on the floor with a warning
  # if the correct IoContext is not still alive.
  obsolete63 @63 :Bool
      $experimental;

  setToStringTag @64 :Bool
      $compatEnableFlag("set_tostring_tag")
      $compatDisableFlag("do_not_set_tostring_tag")
      $compatEnableDate("2024-09-26");
  # A change was made that set the Symbol.toStringTag on all jsg::Objects in order to
  # fix several spec compliance bugs. Unfortunately it turns out that was more breaking
  # than expected. This flag restores the original behavior for compat dates before
  # 2024-09-26

  upperCaseAllHttpMethods @65 :Bool
      $compatEnableFlag("upper_case_all_http_methods")
      $compatDisableFlag("no_upper_case_all_http_methods")
      $compatEnableDate("2024-10-14");
  # HTTP methods are expected to be upper-cased. Per the fetch spec, if the methods
  # is specified as `get`, `post`, `put`, `delete`, `head`, or `options`, implementations
  # are expected to uppercase the method. All other method names would generally be
  # expected to throw as unrecognized (e.g. `patch` would be an error while `PATCH` is
  # accepted). This is a bit restrictive, even if it is in the spec. This flag modifies
  # the behavior to uppercase all methods prior to parsing to that the method is always
  # recognized if it is a known method.

  obsolete66 @66 :Bool
      $compatEnableFlag("python_external_packages");

  noTopLevelAwaitInRequire @67 :Bool
      $compatEnableFlag("disable_top_level_await_in_require")
      $compatDisableFlag("enable_top_level_await_in_require")
      $compatEnableDate("2024-12-02");
  # When enabled, use of top-level await syntax in require() calls will be disallowed.
  # The ecosystem and runtimes are moving to a state where top level await in modules
  # is being strongly discouraged.

  fixupTransformStreamBackpressure @68 :Bool
      $compatEnableFlag("fixup-transform-stream-backpressure")
      $compatDisableFlag("original-transform-stream-backpressure")
      $compatEnableDate("2024-12-16");
  # A bug in the original implementation of TransformStream failed to apply backpressure
  # correctly. The fix, however, can break existing implementations that don't account
  # for the bug so we need to put the fix behind a compat flag.

  obsolete69 @69 :Bool
      $compatEnableFlag("tail_worker_user_spans")
      $experimental;

  cacheNoCache @70 :Bool
      $compatEnableFlag("cache_no_cache_enabled")
      $compatDisableFlag("cache_no_cache_disabled")
      $impliedByAfterDate(name = "cacheOptionEnabled", date = "2025-08-07");
  # Enables the use of cache: no-cache in the fetch api.

  pythonWorkers20250116 @71 :Bool
      $compatEnableFlag("python_workers_20250116")
      $compatDisableFlag("no_python_workers_20250116")
      $impliedByAfterDate(name = "pythonWorkers", date = "2025-09-29")
      $pythonSnapshotRelease;

  requestCfOverridesCacheRules @72 :Bool
      $compatEnableFlag("request_cf_overrides_cache_rules")
      $compatDisableFlag("no_request_cf_overrides_cache_rules")
      $compatEnableDate("2025-04-02")
      $neededByFl;
  # Enables cache settings specified request in fetch api cf object to override cache rules. (only for user owned or grey-clouded sites)

  memoryCacheDelete @73 :Bool
      $compatEnableFlag("memory_cache_delete")
      $experimental;
  # Enables delete operations on memory cache if enabled.

  obsolete74 @74: Bool
      $compatEnableFlag("unique_ctx_per_invocation")
      $compatEnableDate("2025-03-10");
  # Creates a unique ExportedHandler for each call to `export default` thus allowing a unique ctx
  # per invocation.
  #
  # OBSOLETE: We decided to apply this change even to old workers as we've found some old workers
  #   that assumed this behavior all along, and so this change actually fixes bugs for them.
  #   It seems unlikely to break anyone because there's no way a Worker could depend on any two
  #   requests hitting the same isolate, so how could they depend on any two requests having the
  #   same `ctx` object? At worst they might store some sort of cache on it which becomes
  #   ineffective, but their Worker would still work, and anyway this would be a very weird thing
  #   for someone to do.

  reuseCtxAcrossNonclassEvents @92: Bool
      $compatEnableFlag("nonclass_entrypoint_reuses_ctx_across_invocations");
  # Just in case someone somewhere somehow actually relied on every event receiving the same `ctx`
  # object, this restores the original behavior. We do not recommend this.

  queueConsumerNoWaitForWaitUntil @75 :Bool
      $compatEnableFlag("queue_consumer_no_wait_for_wait_until")
      $compatDisableFlag("queue_consumer_wait_for_wait_until");
  # If enabled, does not require all waitUntil'ed promises to resolve successfully before reporting
  # succeeded/failed messages/batches back from a queue consumer to the Queues service. This
  # prevents a slow waitUntil'ed promise from slowing down consumption of messages from a queue,
  # which has been a recurring problem for the prior behavior (which did wait for all waitUntil'ed
  # tasks to complete.
  # This intentionally doesn't have a compatEnableDate yet until so we can let some users opt-in to
  # try it before enabling it for all new scripts, but will eventually need one.

  populateProcessEnv @76 :Bool
      $compatEnableFlag("nodejs_compat_populate_process_env")
      $compatDisableFlag("nodejs_compat_do_not_populate_process_env")
      $impliedByAfterDate(name = "nodeJsCompat", date = "2025-04-01");
  # Automatically populate process.env from text bindings only
  # when nodejs_compat is being used.

  cacheApiRequestCfOverridesCacheRules @77 :Bool
      $compatEnableFlag("cache_api_request_cf_overrides_cache_rules")
      $compatDisableFlag("no_cache_api_request_cf_overrides_cache_rules")
      $compatEnableDate("2025-05-19")
      $neededByFl;
  # Enables cache settings specified request in cache api cf object to override cache rules. (only for user owned or grey-clouded sites)

  disableImportableEnv @78 :Bool
      $compatEnableFlag("disallow_importable_env")
      $compatDisableFlag("allow_importable_env");
  # When allowed, `import { env, exports } from 'cloudflare:workers'` will provide access
  # to the per-request environment/bindings. This flag also disables importable exports
  # (the exports proxy) since both features are conceptually related.

  assetsSecFetchModeNavigateHeaderPrefersAssetServing @79 :Bool
      $compatEnableFlag("assets_navigation_prefers_asset_serving")
      $compatDisableFlag("assets_navigation_has_no_effect")
      $compatEnableDate("2025-04-01");
  # Enables routing to asset-worker over a user worker when an appropriate
  # `assets.not_found_handling` configuration option is set and `Sec-Fetch-Mode: navigate` header
  # is present. This flag is used only by @cloudflare/workers-shared (within workers-sdk) and not
  #  directly by workerd.

  cacheApiCompatFlags @80 :Bool
      $compatEnableFlag("cache_api_compat_flags")
      $compatDisableFlag("no_cache_api_compat_flags")
      $compatEnableDate("2025-04-19");
  # when enabled, exports compability flags for FL to Cache API requests.

  obsolete81 @81 :Bool
      $compatEnableFlag("python_workers_durable_objects")
      $experimental;
  # when enabled, enables Durable Object support for Python Workers.

  obsolete82 @82 :Bool
      $compatEnableFlag("streaming_tail_worker")
      $experimental;
  # Obsolete flag. Has no effect.

  specCompliantUrlpattern @83 :Bool
    $compatEnableFlag("urlpattern_standard")
    $compatEnableDate("2025-05-01")
    $compatDisableFlag("urlpattern_original");
  # The original URLPattern implementation is not compliant with the
  # WHATWG URLPattern Standard, leading to a number of issues reported by users. Unfortunately,
  # making it spec compliant is a breaking change. This flag controls the availability
  # of the new spec-compliant URLPattern implementation.

  jsWeakRef @84 :Bool
      $compatEnableFlag("enable_weak_ref")
      $compatEnableDate("2025-05-05")
      $compatDisableFlag("disable_weak_ref");
  # Enables WeakRefs and FinalizationRegistry API.
  # WebAssembly based projects often rely on this API for wasm memory cleanup

  requestSignalPassthrough @85 :Bool
      $compatEnableFlag("request_signal_passthrough")
      $compatDisableFlag("no_request_signal_passthrough");
  # When enabled, the AbortSignal of the incoming request is not passed through to subrequests.
  # As a result, outgoing subrequests will not be cancelled when the incoming request is.

  enableNavigatorLanguage @86 :Bool
      $compatEnableFlag("enable_navigator_language")
      $compatEnableDate("2025-05-19")
      $compatDisableFlag("disable_navigator_language");
  # Enables Navigator.language API.

  webFileSystem @87 :Bool
      $compatEnableFlag("enable_web_file_system")
      $experimental;
  # Enables the experimental Web File System API.
  # WARNING: This API is still in development and may change or be removed in the future.

  abortSignalRpc @88 :Bool
      $compatEnableFlag("enable_abortsignal_rpc")
      $experimental;
  # Enables experimental support for passing AbortSignal over RPC.

  allowEvalDuringStartup @89 :Bool
      $compatEnableFlag("allow_eval_during_startup")
      $compatEnableDate("2025-06-01")
      $compatDisableFlag("disallow_eval_during_startup");
  # Enables eval() and new Function() during startup.

  enableRequestSignal @90 :Bool
    $compatEnableFlag("enable_request_signal")
    $compatDisableFlag("disable_request_signal");
  # Enables Request.signal for incoming requests.
  # This feature is still experimental and the compat flag has no default enable date.

  connectPassThrough @91 :Bool
    $compatEnableFlag("connect_pass_through")
    $experimental;
  # Causes the Worker to handle incoming connect events by simply passing them through to the
  # Worker's globalOutbound (typically, the internet).
  #
  # As of this writing, Workers cannot yet receive raw socket connections, because no API has been
  # defined for doing so. But a Worker can be configured to be the `globalOutbound` for another
  # Worker, causing the first Worker to intercept all outbound network requests that the second
  # Worker makes by calling `fetch()` or `connect()`. Since there's no way for the first Worker
  # to actually handle the `connect()` requests, this implies the second Worker cannot make any
  # raw TCP connections in this configuration. This is intended: often, the first Worker
  # implements some sort of security rules governing what kinds of requests the second Worker can
  # send to the internet, and if `connect()` requests were allowed to go directly to the internet,
  # that could be used to bypass said security checks.
  #
  # However, sometimes outbound workers are used for reasons other than security, and in fact the
  # outbound Worker does not really care to block `connect()` requests. Until such a time as we
  # create an actual API for proxying connections, such outbound workers can set this compat flag
  # to opt into allowing connect requests to pass through.

  # NOTE: `reuseCtxAcrossNonclassEvents @92` was declared earlier in the file. Next ordinal is
  #   @93.

  bindAsyncLocalStorageSnapshot @93 :Bool
      $compatEnableFlag("bind_asynclocalstorage_snapshot_to_request")
      $compatDisableFlag("do_not_bind_asynclocalstorage_snapshot_to-request")
      $compatEnableDate("2025-06-16");
  # The AsyncLocalStorage frame can capture values that are bound to the
  # current IoContext. This is not always in the users control since we use
  # the ALS storage frame to propagate internal trace spans as well as
  # user-provided values. This flag, when set, binds the snapshot / bound
  # functions to the current IoContext and will throw an error if the bound
  # functions are called outside of the IoContext in which they were created.

  throwOnUnrecognizedImportAssertion @94 :Bool
      $compatEnableFlag("throw_on_unrecognized_import_assertion")
      $compatDisableFlag("ignore_unrecognized_import_assertion")
      $compatEnableDate("2025-06-16");
  # In the original module registry implementation, import attributes that are not recognized
  # would be ignored. This is not compliant with the spec which strongly recommends that runtimes
  # throw an error when unknown import attributes are encountered. In the new module registry
  # implementation the recommended behavior is what is implemented. With this compat flag
  # enabled, the original module registry implementation will follow the recommended behavior.

  pythonWorkflows @95 :Bool
    $compatEnableFlag("python_workflows")
    $compatDisableFlag("disable_python_workflows")
    $impliedByAfterDate(name = "pythonWorkers", date = "2025-09-20");
  # Enables support for Python workflows.
  # This is still in development and may change in the future.

  unsupportedProcessActualPlatform @96 :Bool
      $compatEnableFlag("unsupported_process_actual_platform")
      $experimental;
  # By default, Workerd will always expose "linux" as the process.platform.
  # This flag enables support for process.platform to expose the actual system platform.
  # This is unsupported, as this feature will never ever be supported as non-experimental and is a
  # temporary WPT test path only.

  enableNodeJsProcessV2 @97 :Bool
      $compatEnableFlag("enable_nodejs_process_v2")
      $compatDisableFlag("disable_nodejs_process_v2")
      $impliedByAfterDate(name = "nodeJsCompat", date="2025-09-15");
  # Switches from the partial process implementation with only "nextTick", "env", "exit",
  # "getBuiltinModule", "platform" and "features" property implementations, to the full-featured
  # Node.js-compatibile process implementation with all process properties either stubbed or
  # implemented. It is required to use this flag with nodejs_compat (or nodejs_compat_v2).

  setEventTargetThis @98 :Bool
      $compatEnableFlag("set_event_target_this")
      $compatDisableFlag("no_set_event_target_this")
      $compatEnableDate("2025-08-01");
  # The original implementation of EventTarget was not correctly setting the `this` value
  # for event handlers. This flag enables the correct behavior, which is compliant with the spec.

  enableForwardableEmailFullHeaders @99 :Bool
      $compatEnableFlag("set_forwardable_email_full_headers")
      $compatDisableFlag("set_forwardable_email_single_headers")
      $compatEnableDate("2025-08-01");
  # The original version of the headers sent to edgeworker were truncated to a single
  # value for specific header names, such as To and Cc. With this compat flag we will send
  # the full header values to the worker script.

  enableNodejsHttpModules @100 :Bool
      $compatEnableFlag("enable_nodejs_http_modules")
      $compatDisableFlag("disable_nodejs_http_modules")
      $impliedByAfterDate(name = "nodeJsCompat", date = "2025-08-15");
  # Enables Node.js http related modules such as node:http and node:https

  pedanticWpt @101 :Bool
      $compatEnableFlag("pedantic_wpt")
      $compatDisableFlag("non_pedantic_wpt");
  # Enables a "pedantic mode" for WPT compliance. Multiple changes are grouped under
  # this flag that are known to be required to pass more web platform tests but which
  # otherwise are likely not to be strictly necessary for most users.

  exposeGlobalMessageChannel @102 :Bool
      $compatEnableFlag("expose_global_message_channel")
      $compatDisableFlag("no_expose_global_message_channel")
      $compatEnableDate("2025-08-15");
  # Enables exposure of the MessagePort and MessageChannel classes on the global scope.

  enableNodejsHttpServerModules @103 :Bool
      $compatEnableFlag("enable_nodejs_http_server_modules")
      $compatDisableFlag("disable_nodejs_http_server_modules")
      $impliedByAfterDate(name = "enableNodejsHttpModules", date = "2025-09-01");
  # Enables Node.js http server related modules such as node:_http_server
  # It is required to use this flag with `enable_nodejs_http_modules` since
  # it enables the usage of http related node.js modules, and this flag enables
  # the methods exposed by the node.js http modules.
  # Regarding the recommendation for using import { env, waitUntil } from 'cloudflare:workers';
  # `disallow_importable_env` compat flag should not be set if you are using this
  # and need access to the env since that will prevent access.

  pythonNoGlobalHandlers @104 :Bool
      $compatEnableFlag("python_no_global_handlers")
      $compatDisableFlag("disable_python_no_global_handlers")
      $compatEnableDate("2025-08-14");
  # Disables the global handlers for Python workers and enforces their use via default entrypoint
  # classes.

  enableNodeJsFsModule @105 :Bool
    $compatEnableFlag("enable_nodejs_fs_module")
    $compatDisableFlag("disable_nodejs_fs_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-09-15");
  # Enables the Node.js fs module. It is required to use this flag with
  # nodejs_compat (or nodejs_compat_v2).

  enableNodeJsOsModule @106 :Bool
    $compatEnableFlag("enable_nodejs_os_module")
    $compatDisableFlag("disable_nodejs_os_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-09-15");
  # Enables the Node.js os module. It is required to use this flag with
  # nodejs_compat (or nodejs_compat_v2).

  pythonWorkersForceNewVendorPath @107 :Bool
      $compatEnableFlag("python_workers_force_new_vendor_path")
      $compatEnableDate("2025-08-11");
  # Disables adding `/session/metadata/vendor` to the Python Worker's sys.path. So Workers using
  # this flag will have to place their vendored modules in a `python_modules` directory.

  removeNodejsCompatEOL @108 :Bool
    $compatEnableFlag("remove_nodejs_compat_eol")
    $compatDisableFlag("add_nodejs_compat_eol")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-09-01");
  # Removes the Node.js compatibility layer for EOL versions of Node.js.
  # When the flag is enabled, APIs that have reached End-of-Life in Node.js
  # will be removed for workers. When disabled, the APIs are present (but
  # might still be non-functional stubs)
  # This flag is intended to be a roll-up flag. That is, as additional APIs
  # reach EOL, new compat flags will be added for those that will have
  # `impliedByAfterDate(name = "removeNodeJsCompatEOL", ...` annotations.

  enableWorkflowScriptValidation @109 :Bool
      $compatEnableFlag("enable_validate_workflow_entrypoint")
      $compatDisableFlag("disable_validate_workflow_entrypoint")
      $compatEnableDate("2025-09-20");
  # This flag enables additional checks in the control plane to validate that workflows are
  # defined and used correctly

  pythonDedicatedSnapshot @110 :Bool
      $compatEnableFlag("python_dedicated_snapshot")
      $compatDisableFlag("disable_python_dedicated_snapshot")
      $impliedByAfterDate(name = "pythonWorkers20250116", date = "2025-10-16");
  # Enables the generation of dedicated snapshots on Python Worker upload. The snapshot will be
  # stored inside the resulting WorkerBundle of the Worker. The snapshot will be taken after the
  # top-level execution of the Worker.

  typescriptStripTypes @111 :Bool
    $compatEnableFlag("typescript_strip_types")
    $experimental;
  # Strips all Typescript types from loaded files.
  # If loaded files contain unsupported typescript construct beyond type annotations (e.g. enums),
  # or is not a syntactically valid Typescript, the worker will fail to load.

  enableNodeJsHttp2Module @112 :Bool
    $compatEnableFlag("enable_nodejs_http2_module")
    $compatDisableFlag("disable_nodejs_http2_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-09-01");
  # Enables the Node.js http2 module stubs.

  experimentalAllowEvalAlways @113 :Bool
      $compatEnableFlag("allow_insecure_inefficient_logged_eval")
      $experimental;
  # Enables eval() and new Function() always, even during request handling.
  # ***This flag should *never* be enabled by default.***
  # The name of the enable flag is intentionally long and scary-sounding to
  # discourage casual use.
  #  * "insecure" because code-gen during request handling can lead to security issues.
  #  * "inefficient" because repeated code-gen during request handling can be slow.
  #  * "logged" because each use would likely be logged in production for security
  #    auditing so users should avoid including PII and other sensitive data in dynamically
  #    generated and evaluated code.
  # This flag is experimental and may be removed in the future. It is added for
  # testing purposes.

  stripAuthorizationOnCrossOriginRedirect @114 :Bool
      $compatEnableFlag("strip_authorization_on_cross_origin_redirect")
      $compatDisableFlag("retain_authorization_on_cross_origin_redirect")
      $compatEnableDate("2025-09-01");
  # This flag specifies that when automatic redirects are enabled, and a redirect points to a URL
  # at a different origin, if the original request contained an Authorization header, that header
  # is removed before following the redirect. This behavior is required by the current version of
  # the Fetch API specification.
  #
  # This requirement was added to the Fetch spec in 2022, well after Cloudflare Workers
  # originally implemented it. Hence, Workers did not originally implement this requirement. This
  # requirement is backwards-incompatible, and so the new behavior is guarded by a compatibility
  # flag.
  #
  # Note that the old behavior was not inherently insecure, and indeed could be desirable in many
  # circumstances. For example, if an API that requires authorization wishes to change its
  # hostname, it might wish to redirect to the new hostname while having the client send along
  # their credentials. Under the new fetch behavior, such a redirect will break clients, and this
  # has legitimately broken real use cases. However, it's true that the old behavior could be a
  # "gotcha" leading to security problems when combined with other mistakes. Hence, the spec was
  # changed, and Workers must follow the spec.

  enhancedErrorSerialization @115 :Bool
      $compatEnableFlag("enhanced_error_serialization")
      $compatDisableFlag("legacy_error_serialization")
      $experimental;
  # Enables enhanced error serialization for errors serialized using structuredClone /
  # v8 serialization. More error types are supported, and own properties are included.
  # Note that when enabled, deserialization of the errors will not preserve the original
  # stack by default.

  emailSendingQueuing @116 :Bool
      $compatEnableFlag("enable_email_sending_queuing")
      $compatDisableFlag("disable_email_sending_queuing");
  # Enables Queuing on the `.send(message: EmailMessage)` function on send_email binding if there's
  # a temporary error on email delivery.
  # Note that by enabling this, user-provided Message-IDs are stripped and
  # Email Workers will generate and use its own.

  removeNodejsCompatEOLv22 @117 :Bool
      $compatEnableFlag("remove_nodejs_compat_eol_v22")
      $compatDisableFlag("add_nodejs_compat_eol_v22")
      $impliedByAfterDate(name = "removeNodejsCompatEOL", date = "2027-04-30");
  # Removes APIs that reached end-of-life in Node.js 22.x. When using the
  # removeNodejsCompatEOL flag, this will default enable on/after 2027-04-30.

  removeNodejsCompatEOLv23 @118 :Bool
      $compatEnableFlag("remove_nodejs_compat_eol_v23")
      $compatDisableFlag("add_nodejs_compat_eol_v23")
      $impliedByAfterDate(name = "removeNodejsCompatEOLv24", date = "2025-09-01");
  # Removes APIs that reached end-of-life in Node.js 23.x. This will default
  # enable when the removeNodejsCompatEOLv24 flag is enabled after 2025-09-01.
  # Went EOL on 2025-06-01

  removeNodejsCompatEOLv24 @119 :Bool
      $compatEnableFlag("remove_nodejs_compat_eol_v24")
      $compatDisableFlag("add_nodejs_compat_eol_v24")
      $impliedByAfterDate(name = "removeNodejsCompatEOL", date = "2028-04-30");
  # Removes APIs that reached end-of-life in Node.js 24.x. When using the
	# removeNodejsCompatEOL flag, this will default enable on/after 2028-04-30.

  enableNodeJsConsoleModule @120 :Bool
    $compatEnableFlag("enable_nodejs_console_module")
    $compatDisableFlag("disable_nodejs_console_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-09-21");
  # Enables the Node.js console module. It is required to use this flag with
  # nodejs_compat (or nodejs_compat_v2).

  enableNodeJsVmModule @121 :Bool
    $compatEnableFlag("enable_nodejs_vm_module")
    $compatDisableFlag("disable_nodejs_vm_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-10-01");
  # Enables the Node.js non-functional stub vm module. It is required to use this flag with
  # nodejs_compat (or nodejs_compat_v2).

  enableNodeJsPerfHooksModule @122 :Bool
     $compatEnableFlag("enable_nodejs_perf_hooks_module")
     $compatDisableFlag("disable_nodejs_perf_hooks_module")
     $experimental;
     # $impliedByAfterDate(name = "nodeJsCompat", date = "2025-10-01");
   # Enables the Node.js perf_hooks module. It is required to use this flag with
   # nodejs_compat (or nodejs_compat_v2).

  enableGlobalPerformanceClasses @123 :Bool
     $compatEnableFlag("enable_global_performance_classes")
     $compatDisableFlag("disable_global_performance_classes")
     $experimental;
   # Enables PerformanceEntry, PerformanceMark, PerformanceMeasure, PerformanceResourceTiming,
   # PerformanceObserver and PerformanceObserverEntryList global classes.

  enableNodeJsDomainModule @124 :Bool
    $compatEnableFlag("enable_nodejs_domain_module")
    $compatDisableFlag("disable_nodejs_domain_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-12-04");
  # Enables the Node.js non-functional stub domain module. It is required to use this flag with
  # nodejs_compat (or nodejs_compat_v2).

  enableNodeJsV8Module @125 :Bool
    $compatEnableFlag("enable_nodejs_v8_module")
    $compatDisableFlag("disable_nodejs_v8_module")
    $experimental;
    # $impliedByAfterDate(name = "nodeJsCompat", date = "2025-10-15");
  # Enables the Node.js non-functional stub v8 module. It is required to use this flag with
  # nodejs_compat (or nodejs_compat_v2).

  enableNodeJsTtyModule @126 :Bool
    $compatEnableFlag("enable_nodejs_tty_module")
    $compatDisableFlag("disable_nodejs_tty_module")
    $experimental;
    # $impliedByAfterDate(name = "nodeJsCompat", date = "2025-10-15");
  # Enables the Node.js non-functional stub tty module. It is required to use this flag with
  # nodejs_compat (or nodejs_compat_v2).

  enableNodeJsPunycodeModule @127 :Bool
    $compatEnableFlag("enable_nodejs_punycode_module")
    $compatDisableFlag("disable_nodejs_punycode_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-12-04");
  # Enables the Node.js deprecated punycode module. It is required to use this flag with
  # nodejs_compat (or nodejs_compat_v2).

  enableNodeJsClusterModule @128 :Bool
    $compatEnableFlag("enable_nodejs_cluster_module")
    $compatDisableFlag("disable_nodejs_cluster_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-12-04");
  # Enables the Node.js non-functional stub cluster module. It is required to use this flag with
  # nodejs_compat (or nodejs_compat_v2).

  enableNodeJsChildProcessModule @129 :Bool
    $compatEnableFlag("enable_nodejs_child_process_module")
    $compatDisableFlag("disable_nodejs_child_process_module")
    $experimental;
  # Enables the Node.js non-functional stub child_process module. It is required to use this
  # flag with nodejs_compat (or nodejs_compat_v2).

  enableNodeJsWorkerThreadsModule @130 :Bool
    $compatEnableFlag("enable_nodejs_worker_threads_module")
    $compatDisableFlag("disable_nodejs_worker_threads_module")
    $experimental;
  # Enables the Node.js non-functional stub worker_threads module. It is required to use this
  # flag with nodejs_compat (or nodejs_compat_v2).

  enableNodeJsStreamWrapModule @131 :Bool
    $compatEnableFlag("enable_nodejs_stream_wrap_module")
    $compatDisableFlag("disable_nodejs_stream_wrap_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2026-01-29");
  # Enables the Node.js non-functional stub _stream_wrap module. It is required to use this
  # flag with nodejs_compat (or nodejs_compat_v2).

  enableNodeJsWasiModule @132 :Bool
    $compatEnableFlag("enable_nodejs_wasi_module")
    $compatDisableFlag("disable_nodejs_wasi_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-12-04");
  # Enables the Node.js non-functional stub wasi module. It is required to use this
  # flag with nodejs_compat (or nodejs_compat_v2).

  enableNodeJsDgramModule @133 :Bool
    $compatEnableFlag("enable_nodejs_dgram_module")
    $compatDisableFlag("disable_nodejs_dgram_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2026-01-29");
  # Enables the Node.js non-functional stub dgram module. It is required to use this
  # flag with nodejs_compat (or nodejs_compat_v2).

  enableNodeJsInspectorModule @134 :Bool
    $compatEnableFlag("enable_nodejs_inspector_module")
    $compatDisableFlag("disable_nodejs_inspector_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2026-01-29");
  # Enables the Node.js non-functional stub inspector module. It is required to use this
  # flag with nodejs_compat (or nodejs_compat_v2).

  enableNodeJsTraceEventsModule @135 :Bool
    $compatEnableFlag("enable_nodejs_trace_events_module")
    $compatDisableFlag("disable_nodejs_trace_events_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2025-12-04");
  # Enables the Node.js non-functional stub trace_events module. It is required to use this
  # flag with nodejs_compat (or nodejs_compat_v2).

  enableNodeJsReadlineModule @136 :Bool
    $compatEnableFlag("enable_nodejs_readline_module")
    $compatDisableFlag("disable_nodejs_readline_module")
    $experimental;
  # Enables the Node.js non-functional stub readline module. It is required to use this
  # flag with nodejs_compat (or nodejs_compat_v2).

  enableNodeJsReplModule @137 :Bool
    $compatEnableFlag("enable_nodejs_repl_module")
    $compatDisableFlag("disable_nodejs_repl_module")
    $experimental;
  # Enables the Node.js non-functional stub repl module. It is required to use this
  # flag with nodejs_compat (or nodejs_compat_v2).

  enableNodeJsSqliteModule @138 :Bool
    $compatEnableFlag("enable_nodejs_sqlite_module")
    $compatDisableFlag("disable_nodejs_sqlite_module")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2026-01-29");
  # Enables the Node.js non-functional stub sqlite module. It is required to use this
  # flag with nodejs_compat (or nodejs_compat_v2).

  enableCtxExports @139 :Bool
    $compatEnableFlag("enable_ctx_exports")
    $compatDisableFlag("disable_ctx_exports")
    $compatEnableDate("2025-11-17");
  # Enable the ctx.exports API.

  pythonExternalSDK @140 :Bool
    $compatEnableFlag("enable_python_external_sdk")
    $compatDisableFlag("disable_python_external_sdk")
    $experimental;
  # Don't include the Python sdk from the runtime, use a vendored copy.

  fastJsgStruct @141 :Bool
    $compatEnableFlag("enable_fast_jsg_struct")
    $compatDisableFlag("disable_fast_jsg_struct")
    $compatEnableDate("2025-12-03");
  # Enables the fast jsg::Struct optimization. With this enabled, JSG_STRUCTS
  # will use a more efficient creation pattern that reduces construction time.
  # However, optional fields will be explicitly set to undefined rather than
  # being omitted, which is an observable behavior change.

  cacheReload @142 :Bool
      $compatEnableFlag("cache_reload_enabled")
      $compatDisableFlag("cache_reload_disabled")
      $experimental;
  # Enables the use of cache: reload in the fetch api.

  streamsNodejsV24Compat @143 :Bool
    $compatEnableFlag("enable_streams_nodejs_v24_compat")
    $compatDisableFlag("disable_streams_nodejs_v24_compat");
  # Enables breaking changes to Node.js streams done with the release of Node.js v24.

  pythonCheckRngState @144 : Bool
    $compatEnableFlag("python_check_rng_state")
    $compatDisableFlag("disable_python_check_rng_state")
    $impliedByAfterDate(name = "pythonWorkers", date = "2025-12-16");

  shouldSetImmutablePrototype @145 :Bool
    $compatEnableFlag("immutable_api_prototypes")
    $compatDisableFlag("mutable_api_prototypes");
  # When set, tells JSG to make the prototype of all jsg::Objects immutable.
  # TODO(soon): Add the default on date once the flag is verified to be
  # generally safe.

  fetchIterableTypeSupport @146 :Bool
    $compatEnableFlag("fetch_iterable_type_support")
    $compatDisableFlag("no_fetch_iterable_type_support")
    $compatEnableDate("2026-02-19");
  # Enables passing sync and async iterables as the body of fetch Request or Response.
  # Previously, sync iterables like Arrays would be accepted but stringified, and async
  # iterables would be treated as regular objects and not iterated over at all. With this
  # flag enabled, sync and async iterables will be properly iterated over and their values
  # used as the body of the request or response.
  # The actual compat flag enables the specific AsyncGeneratorIgnoringStrings type wrapper
  # that allows this behavior and allows sync Generator and AsyncGenerator objects to be
  # included in kj::OneOf declarations safely with strings and other types. When enabled,
  # strings are ignored but Arrays will be treated as iterables and not stringified as before.
  # Also see fetchIterableTypeSupportOverrideAdjustment.

  envModuleNullableSupport @147 :Bool
    $compatEnableFlag("env_module_nullable_support")
    $compatDisableFlag("no_env_module_nullable_support");
  # Enables support for null and undefined values in EnvModule's getCurrentEnv() and
  # getCurrentExports() methods. When enabled, if the async context contains null or
  # undefined values, they will be returned instead of falling through to the worker
  # env/exports. This allows more explicit control over env and exports values in async
  # contexts.

  preciseTimers @148 :Bool
    $compatEnableFlag("precise_timers")
    $compatDisableFlag("no_precise_timers")
    $experimental;
  # Enables precise timers with 3ms granularity. This provides more accurate timing for performance
  # measurements and time-sensitive operations.

  fetchIterableTypeSupportOverrideAdjustment @149 :Bool
    $compatEnableFlag("fetch_iterable_type_support_override_adjustment")
    $compatDisableFlag("no_fetch_iterable_type_support_override_adjustment")
    $impliedByAfterDate(name = "fetchIterableTypeSupport", date = "2026-01-15");
  # Further adapts the fetch iterable type support to adjust for toString/toPrimitive
  # overrides on sync iterable objects. Specifically, if an object passed as the body
  # of a fetch Request or Response is sync iterable but has a custom toString or
  # toPrimitive method, we will skip treating it as a sync iterable and instead allow
  # it to fall through to being handled as a stringified object.

  stripBomInReadAllText @150 :Bool
    $compatEnableFlag("strip_bom_in_read_all_text")
    $compatDisableFlag("do_not_strip_bom_in_read_all_text")
    $compatEnableDate("2026-01-13")
    $impliedByAfterDate(name = "pedanticWpt", date = "2026-01-13");
  # Instructs the readAllText method in streams to strip the leading UTF8 BOM if present.

  allowIrrevocableStubStorage @151 :Bool
    $compatEnableFlag("allow_irrevocable_stub_storage")
    $experimental;
  # Permits various stub types (e.g. ServiceStub aka Fetcher, DurableObjectClass) to be stored in
  # long-term Durable Object storage without any mechanism for the stub target to audit or revoke
  # incoming connections.
  #
  # This feature exists for experimental use only, and will be removed once we have a properly
  # auditable and revocable storage mechanism.

  rpcParamsDupStubs @152 :Bool
    $compatEnableFlag("rpc_params_dup_stubs")
    $compatDisableFlag("rpc_params_transfer_stubs")
    $compatEnableDate("2026-01-20");
  # Changes the ownership semantics of RPC stubs embedded in the parameters of an RPC call.
  #
  # When the RPC system was first introduced, RPC stubs that were embedded in the params or return
  # value of some other call had their ownership transferred. That is, the original stub was
  # implicitly disposed, with a duplicate stub being delivered to the destination.
  #
  # This turns out to compose poorly with another rule: in the callee, any stubs received in the
  # params of a call are automatically disposed when the call returns. These two rules combine to
  # mean that if you proxy a call -- i.e. the implementation of an RPC just makes another RPC call
  # passing along the same params -- then any stubs in the params get disposed twice. Worse, if
  # the eventual recipient of the stub wants to keep a duplicate past the end of the call, this
  # may not work because the copy of the stub in the proxy layer gets disposed anyway, breaking the
  # connection.
  #
  # For this reason, the pure-JS implementation of Cap'n Web switched to saying that stubs in params
  # do NOT transfer ownership -- they are simply duplicated. This compat flag fixes the Workers
  # Runtime built-in RPC to match Cap'n Web behavior.
  #
  # In particular, this fixes: https://github.com/cloudflare/capnweb/issues/110

  enableNodejsGlobalTimers @153 :Bool
    $compatEnableFlag("enable_nodejs_global_timers")
    $compatDisableFlag("no_nodejs_global_timers")
    $impliedByAfterDate(name = "nodeJsCompat", date = "2026-02-10");
  # When enabled, setTimeout, setInterval, clearTimeout, and clearInterval
  # are available on globalThis as Node.js-compatible versions from node:timers.
  # setTimeout and setInterval return Timeout objects with methods like
  # refresh(), ref(), unref(), and hasRef().
  # This flag requires nodejs_compat or nodejs_compat_v2 to be enabled.

  noAutoAllocateChunkSize @154 :Bool
    $compatEnableFlag("streams_no_default_auto_allocate_chunk_size")
    $compatDisableFlag("streams_auto_allocate_chunk_size_default")
    $experimental;
  # Per the WHATWG Streams spec, byte streams should only have autoAllocateChunkSize
  # set when the user explicitly provides it. When not set, non-BYOB reads on byte
  # streams won't have a byobRequest available in the pull() callback, and the
  # underlying source must use controller.enqueue() to provide data instead.
  #
  # Our original implementation always defaulted autoAllocateChunkSize to 4096,
  # which meant all byte stream reads behaved as BYOB reads. This flag enables
  # the spec-compliant behavior where autoAllocateChunkSize is only set when
  # explicitly provided by the user.
  #
  # Code relying on the old behavior that accesses controller.byobRequest without
  # checking for null will break. To migrate, either:
  # 1. Add a null check: if (controller.byobRequest) { ... }
  # 2. Explicitly set autoAllocateChunkSize when creating the stream

  pythonWorkflowsImplicitDeps @155 :Bool
    $compatEnableFlag("python_workflows_implicit_dependencies")
    $compatDisableFlag("no_python_workflows_implicit_dependencies")
    $impliedByAfterDate(name = "pythonWorkers", date = "2026-02-25");
  # replaces depends param on steps to an implicit approach with step callables passed as params
  # these steps are called internally and act as dependencies

  requireReturnsDefaultExport @156 :Bool
    $compatEnableFlag("require_returns_default_export")
    $compatDisableFlag("require_returns_namespace")
    $compatEnableDate("2026-01-22");
  # When enabled, require() will return the default export of a module if it exists.
  # If the default export does not exist, it falls back to returning a mutable
  # copy of the module namespace object. This matches the behavior that Node.js
  # uses for require(esm) where the default export is returned when available.
  # This flag is useful for frameworks like Next.js that expect to patch module exports.
  #
  # TODO(later): Once this is no longer experimental, this flag should be implied by
  # exportCommonJsDefaultNamespace (or vice versa) for consistency.

  pythonRequestHeadersPreserveCommas @157 :Bool
    $compatEnableFlag("python_request_headers_preserve_commas")
    $compatDisableFlag("disable_python_request_headers_preserve_commas")
    $compatEnableDate("2026-02-17");
  # Preserve commas in Python Request headers rather than treating them as separators,
  # while still exposing multiple Set-Cookie headers as distinct values.

  queueExposeErrorCodes @158 :Bool
    $compatEnableFlag("queue_expose_error_codes")
    $compatDisableFlag("no_queue_expose_error_codes");
  # When enabled, queue operations will include detailed error information (error code and cause)

  textDecoderReplaceSurrogates @159 :Bool
    $compatEnableFlag("text_decoder_replace_surrogates")
    $compatDisableFlag("disable_text_decoder_replace_surrogates")
    $compatEnableDate("2026-02-24")
    $impliedByAfterDate(name = "pedanticWpt", date = "2026-02-24");
  # When enabled, the UTF-16le TextDecoder will replace lone surrogates with U+FFFD
  # (the Unicode replacement character) as required by the spec. Previously, lone
  # surrogates were passed through unchanged, producing non-well-formed strings.

  containersPidNamespace @160 :Bool
    $compatEnableFlag("containers_pid_namespace")
    $compatDisableFlag("no_containers_pid_namespace")
    $experimental;
  # When enabled, containers attached to Durable Objects do NOT share the host PID namespace
  # (they get their own isolated PID namespace). When disabled (the default), containers share
  # the host PID namespace.

  deleteAllDeletesAlarm @161 :Bool
    $compatEnableFlag("delete_all_deletes_alarm")
    $compatDisableFlag("delete_all_preserves_alarm")
    $compatEnableDate("2026-02-24");
  # When enabled, calling storage.deleteAll() on a Durable Object also deletes
  # any scheduled alarm, in addition to deleting all stored key-value data.
  #
  # Previously, deleteAll() preserved the alarm state. This was surprising
  # behavior since the intent of deleteAll() is to clear all state.

  unhandledRejectionAfterMicrotaskCheckpoint @162 :Bool
    $compatEnableFlag("unhandled_rejection_after_microtask_checkpoint")
    $compatDisableFlag("no_unhandled_rejection_after_microtask_checkpoint")
    $compatEnableDate("2026-03-03");
  # When enabled, unhandledrejection processing is deferred until the microtask
  # checkpoint completes, avoiding misfires on multi-tick promise chains.

  textDecoderCjkDecoder @163 :Bool
    $compatEnableFlag("text_decoder_cjk_decoder")
    $compatDisableFlag("disable_text_decoder_cjk_decoder")
    $compatEnableDate("2026-03-03");
  # Enables the dedicated CJK TextDecoder implementation for overrides and
  # Big5 lead-byte handling instead of the legacy ICU-only path.
}
