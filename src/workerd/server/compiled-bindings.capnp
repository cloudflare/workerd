# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0x81303bd8e960bdd1;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::server");

using Workerd = import "/workerd/server/workerd.capnp";

# The bindings of one worker after the server has interpreted its config: every value is final and
# every capability has its channel number. The server (Rust) produces this message from
# `Worker.bindings` in workerd.capnp and from the worker's exports; the worker factory (C++)
# compiles it into the `env` object and `ctx.exports` under the isolate lock.
#
# Channel numbers index the worker's IoChannelFactory tables. Subrequest channels start at
# IoContext::SPECIAL_SUBREQUEST_CHANNEL_COUNT (the two below are the global outbound); actor,
# actor-class and worker-loader channels each start at zero.

struct Global {
  name @0 :Text;

  union {
    text @1 :Text;
    data @2 :Data;
    json @3 :Text;
    # JSON text, parsed into a value.

    fetcher @4 :UInt32;
    # A binding that is only a capability (this and the other `UInt32`s) is its channel number.
    loopbackServiceStub @5 :UInt32;
    kvNamespace @6 :UInt32;
    r2Bucket :group {
      channel @7 :UInt32;
      bucket @8 :Text;
    }
    queue @9 :UInt32;
    analyticsEngine :group {
      channel @10 :UInt32;
      dataset @11 :Text;
    }
    hyperdrive :group {
      channel @12 :UInt32;
      database @13 :Text;
      user @14 :Text;
      password @15 :Text;
      scheme @16 :Text;
    }

    cryptoKey :group {
      format @17 :Text;
      # "raw", "pkcs8", "spki" or "jwk".
      keyData :union {
        bytes @18 :Data;
        # For raw, pkcs8 and spki: the key material, DER for the latter two.
        json @19 :Text;
        # For jwk: the key as JSON text.
      }
      algorithm @20 :Text;
      # The algorithm as JSON text: a quoted name, or an object.
      extractable @21 :Bool;
      usages @22 :List(Workerd.Worker.Binding.CryptoKey.Usage);
    }

    ephemeralActorNamespace @23 :UInt32;
    loopbackEphemeralActorNamespace :group {
      actorChannel @24 :UInt32;
      classChannel @25 :UInt32;
    }
    durableActorNamespace :group {
      actorChannel @26 :UInt32;
      uniqueKey @27 :Text;
      retryPolicy @28 :Workerd.Worker.Binding.DurableObjectNamespaceDesignator.RetryPolicy;
      # The binding's `retryPolicy`, already checked against the runtime's limits
      # (`api::UserDefinedRetryPolicy`); the runtime's defaults apply when absent.
    }
    loopbackDurableActorNamespace :group {
      actorChannel @29 :UInt32;
      uniqueKey @30 :Text;
      classChannel @31 :UInt32;
    }
    actorClass @32 :UInt32;
    loopbackActorClass @33 :UInt32;

    wrapped :group {
      moduleName @34 :Text;
      entrypoint @35 :Text;
      innerBindings @36 :List(Global);
    }

    unsafeEval @37 :Void;
    memoryCache :group {
      cacheId @38 :Text;
      # Empty when the cache is not shared.
      maxKeys @39 :UInt32;
      maxValueSize @40 :UInt32;
      maxTotalValueSize @41 :UInt64;
    }
    workerLoader @42 :UInt32;
    workerdDebugPort @43 :Void;
  }
}

struct Globals {
  globals @0 :List(Global);
}
