# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xb200a391b94343f1;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::rpc");
$Cxx.allowCancellation;

interface ActorStorage @0xd7759d7fc87c08e4 {
  struct KeyValue {
    key @0 :Data;
    value @1 :Data;
  }

  struct KeyRename {
    oldKey @0 :Data;
    newKey @1 :Data;
  }

  getStage @0 (stableId :Text) -> (stage :Stage);
  # Get the storage capability for the given stage of the pipeline, identified by its stable ID.

  interface Operations @0xb512f2ce1f544439 {
    get @0 (key :Data) -> (value :Data);
    list @3 (start :Data, end :Data, limit :Int32, reverse :Bool, stream :ListStream, prefix :Data);
    put @1 (entries :List(KeyValue));
    delete @2 (keys :List(Data)) -> (numDeleted :Int32);

    getMultiple @4 (keys :List(Data), stream :ListStream);
    # `stream` will be provided with a `KeyValue` for each key in `keys` that is present in storage
    # in order. So if we had `keys = [c, b, a]` and b was absent, `stream` would be provided with
    # [(c, 2), (a, 1)].

    deleteAll @5 () -> (numDeleted :Int32);

    rename @9 (entries :List(KeyRename)) -> (renamed :List(Data));

    getAlarm @6 () -> (scheduledTimeMs :Int64);
    setAlarm @7 (scheduledTimeMs :Int64);
    deleteAlarm @8 (timeToDeleteMs :Int64) -> (deleted :Bool);
  }

  struct DbSettings {
    enum Priority {
      default @0;
      low @1;
    }
    priority @0 :Priority;
    asOfTimeMs @1 :Int64;
  }

  interface Stage @0xdc35f52864c57550 extends(Operations) {
    txn @0 (settings :DbSettings) -> (transaction :Transaction);

    interface Transaction extends(Operations) {
      commit @0 ();
      rollback @1 ();
    }
  }

  interface ListStream {
    values @0 (list :List(KeyValue)) -> stream;
    end @1 ();
  }

  const maxKeys :UInt32 = 128;
  # The maximum number of keys that clients should be allowed to modify in a single storage
  # operation. This should be enforced for operations that access or modify multiple keys. This
  # limit will not be enforced upon the total count of keys involved in explicit transactions.

  const renameLimit :UInt32 = 1000;
  # The maximum number of keys in a rename() operation.
}
