# Copyright (c) 2025 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xc086f616deb649e5;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::server");
$Cxx.allowCancellation;

using Frankenvalue = import "/workerd/io/frankenvalue.capnp".Frankenvalue;

struct ChannelToken {
  # Internal structure of a channel token in workerd, as returned by
  # {SubrequestChannel,ActorClassChannel}::getToken().
  #
  # For RPC tokens, this structure is encoded as a "packed" capnp message and then AES-GCM
  # encrypted using a secret service key and random IV to form the final token. The full token
  # contains, in order:
  # * 4-byte magic number: 0x821dad26 (little-endian encoded)
  # * 12-byte IV
  # * 16-byte key ID (prefix of SHA-256 hash of secret key, not encrypted)
  # * ciphertext
  # * 16-byte MAC (covering ciphertext and the first 32 bytes as AAD)
  #
  # The encryption (particularly the MAC) is important in order to ensure that someone speaking to
  # workerd over RPC cannot trivially invoke an arbitrary service with arbitrary props by simply
  # presenting a channel token.
  #
  # As of this writing, for RPC tokens, the secret key is generated randomly at process startup.
  # This means that RPC tokens are only usable within the same workerd process that created them,
  # which also has the side effect of meaning there is no backwards-compatibilty concern. The
  # format is likely to change in the future once we figure out more how it should actually be used.
  #
  # The 16-byte key ID is included to enable routing. Hypothetically, if workerd processes were to
  # register their key IDs in some lookup service, then based on the key ID you could find an
  # appropriate workerd instance to connect to to use this token. As of this writing, this is still
  # speculative.
  #
  # For storage tokens -- which are only permitted today with the experimental
  # allow_irrevocable_stub_storage compat flag -- the format is:
  # * 4-byte magic number: 0x9082806d (little-endian encoded)
  # * plaintext ("packed" ChannelToken)
  #
  # There is no encryption in this case. This format is experimental and will be replaced in the
  # future with a different kind of token that refers into some sort of token grant table.

  const rpcTokenMagic :UInt32 = 0x821dad26;
  const storageTokenMagic :UInt32 = 0x9082806d;

  type @0 :Type;
  # What type of channel does this point to? This is encoded as a safety measure. In normal
  # operation the envelope containing the token always knows what type it is meant to be, but we
  # want to prevent any possible shenanigans from someone taking a channel token of one type and
  # trying to stuff it in an envelope for a different type.

  enum Type {
    subrequest @0;  # token for IoChannelFactory::SubrequestChannel
    actorClass @1;  # token for IoChannelFactory::ActorClassChannel
  }

  name @1 :Text;
  # Name of the service in the workerd config's services list.

  entrypoint @2 :Text;
  # Name of the entrypoint the channel points at. For subrequest channels this must be a
  # WorkerEntrypoint derivative (or plain object implementing `ExportedHandlers`). For actor class
  # channels this must be a `DurableObject` implementation.

  props @3 :Frankenvalue;

  struct FrankenvalueCapTable {
    # CapTable representation for `ChannelToken.props`.

    caps @0 :List(Cap);

    struct Cap {
      union {
        unknown @0 :Void;
        # Dummy default value, never appears in practice.

        subrequestChannel @1 :Data;
        actorClassChannel @2 :Data;
        # Nested capabilities are represented using fully encoded channel tokens themselves (rather
        # than ChannelToken capnp structs) for a couple reasons:
        # 1. This simplifies the abstractions needed when encoding a channel token -- just call
        #    getToken() on any nested channels.
        # 2. Channel tokens may be tied to the particular workerd instance that they came from, and
        #    cannot be decoded on other instances, but I suspect we will eventually want to support
        #    props containing capabilities pointing to other workerd instances.
      }
    }
  }
}
