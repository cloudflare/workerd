# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xf699edadaece33b7;
# The format of a Durable Object's log in the bucket: the storage relay's
# (SRS's), field for field, so the two stay wire-compatible. Workerd writes
# one `sqlite` lane and leaves unset what it has no use for, noted below.
# Compression and encryption are not part of it.
#
# A log batch is a run of frames, each one message, that starts and ends at
# a commit. A snapshot is a `SnapshotHeader` and then each lane's content in
# lane order.

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::server::cluster");

struct Frame {
  # One change to an object's state.

  session @0 :UInt32;
  # The ownership epoch that wrote the frame.

  sequence @1 :UInt32;
  # Position within the session, from zero. A position between two frames
  # is the sequence of the one after it.

  lane @2 :UInt32;
  # A lane is numbered on first use, with the lowest free number.

  const unknownContinueFromSession :UInt32 = 0xffffffff;

  continueFromSession @10 :UInt32 = Frame.unknownContinueFromSession;
  continueFromSequence @11 :UInt32;
  # For a session's first frame: where the last non-empty session ended.
  # Workerd leaves these unknown: own.json names the session a session
  # recovered.

  union {
    newLane @3 :LaneDescription;

    cloneLane :group {
      # Unused by workerd.
      name @13 :Text;
      from @14 :UInt32;
    }

    deleteLane @4 :Void;

    write :group {
      # For a `sqlite` lane, bytes SQLite wrote to the WAL, at an offset
      # that keeps growing across checkpoints, though SQLite sees each WAL
      # start over at zero.
      offset @5 :UInt64;
      data @6 :Data;
    }

    obsolete7 @7 :Void;

    consume :group {
      # For a `sqlite` lane, a checkpoint copied the WAL's committed pages
      # into the database: data before `offset` is not needed again.
      offset @8 :UInt64;
    }

    initialize @9 :Data;
    # For a `sqlite` lane, the whole database file, written before the WAL
    # is in use.

    setReplicaInformation @12 :AnyPointer;
    # Unused by workerd.
  }
}

struct LaneDescription {
  name @0 :Text;
  # The lane's stable identifier; numbers differ between objects.

  union {
    sqlite @1 :Void;
    journal @2 :Void;
    # Unused by workerd.
    replicaInformation @3 :Void;
    # Unused by workerd.
  }
}

enum SnapshotJustification {
  # Workerd writes only `clientRequested` and `coldStartMitigation`.

  unknown @0;
  clientRequested @1;
  # The log written since the last snapshot outgrew the database.
  pointInTimeRecovery @2;
  snapshotFallback @3;
  lostAndFoundRecovery @4;
  coldStartMitigation @5;
  # Workerd's snapshot of the state a session starts from, so the next
  # restore reads one file and no earlier session's log.
  cleanShutdown @6;
  test @7;
  srsTools @8;
  localOnly @9;
}

struct SnapshotHeader {
  # The whole state of an object at a session and frame; the log continues
  # from `frame`.

  session @0 :UInt32;
  frame @1 :UInt32;

  lanes @2 :List(Lane);
  # Every lane, by number.

  struct Lane {
    name @0 :Text;

    union {
      deleted @1 :Void;
      # A lane number freed while later ones are in use.

      sqlite :group {
        databaseSize @2 :UInt64;
        walSize @3 :UInt64;
        # The WAL bytes after the database's. Always 0 in the bucket: a
        # snapshot's database has every committed page.

        walBaseOffset @6 :UInt64;
        # Where the log's next write continues. Workerd snapshots from the
        # log, not the live database, so that write can continue a WAL
        # past its header, at a frame.

        walShiftOffset @7 :UInt64;
        # Always 0: workerd keeps no lane files of its own to shift.
      }

      journal :group {
        # Unused by workerd, as is `replicaInformation`.
        size @4 :UInt64;
        offset @5 :UInt64;
      }

      replicaInformation :group {
        size @8 :UInt64;
      }
    }
  }

  logBytesSinceLastSnapshot @3 :UInt64;
  # The log bytes in the bucket since the previous snapshot.

  continueFromSession @4 :UInt32 = Frame.unknownContinueFromSession;
  continueFromSequence @5 :UInt32;
  # Unknown, as in `Frame`.

  justification @6 :SnapshotJustification;
}
