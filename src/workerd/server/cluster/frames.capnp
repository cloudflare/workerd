# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xf699edadaece33b7;
# The format of a Durable Object's log in the bucket. An object's state is
# a set of lanes, each a file; its log is a stream of frames, each a change
# to one lane, written by one session (an ownership epoch) in sequence.
# Compression and encryption are not part of it.

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("workerd::server::cluster");

struct FileHeader {
  # The first message of every file in the bucket. A file's level and
  # range are in its key, not here.

  union {
    log @0 :Void;
    # Frames follow.

    snapshot @1 :SnapshotHeader;
    # Each lane's content follows.
  }
}

struct Frame {
  # A batch of frames in the bucket starts and ends at a commit, and is
  # applied whole.

  session @0 :UInt32;
  # The ownership epoch that wrote the frame.

  sequence @1 :UInt32;
  # Rises within a session. A frame that merges a run of commits takes the
  # run's last sequence.

  lane @2 :UInt32;
  # A lane is numbered on first use, with the lowest free number.

  union {
    newLane @3 :LaneDescription;
    deleteLane @4 :Void;

    pages :group {
      # For a `sqlitePages` lane: the newest version of each page a commit,
      # or a run of commits, wrote, and the database's size after, in
      # pages.
      commit @5 :UInt32;
      list @6 :List(Page);
    }
  }
}

struct Page {
  number @0 :UInt32;
  data @1 :Data;
}

struct LaneDescription {
  name @0 :Text;
  # The lane's stable identifier; numbers differ between objects.

  type @1 :Type;

  enum Type {
    sqlitePages @0;
    # A SQLite database whose writes are pages, so a run of commits merges
    # to one copy of each page.
  }
}

struct SnapshotHeader {
  # The whole state of an object at a session and frame: this header, then
  # each lane's content in lane order. The log is replayed from `frame`
  # on.

  session @0 :UInt32;
  frame @1 :UInt32;

  lanes @2 :List(Lane);
  # Every lane, by number.

  struct Lane {
    name @0 :Text;

    union {
      deleted @1 :Void;
      # A lane number freed while later ones are in use.

      sqlitePages :group {
        databaseSize @2 :UInt64;
      }
    }
  }
}
