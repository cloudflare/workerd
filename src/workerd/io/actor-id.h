#pragma once

#include <kj/common.h>
#include <kj/string.h>

namespace workerd {

// Behavior mode for getting an actor
enum class ActorGetMode {
  // Creates the actor if it does not already exist, otherwise gets the existing actor.
  GET_OR_CREATE,

  // Get an already-created actor, throwing an error if it does not exist.
  GET_EXISTING
};

// An abstract class that implements generation of global actor IDs in a particular namespace.
//
// This is NOT at I/O type. Each global actor namespace binding holds one instance of this which
// it may call from any thread.
class ActorIdFactory {
public:
  // Abstract actor ID.
  //
  // This is NOT an I/O type. An ActorId created in one IoContext can be used in other
  // IoContexts. `ActorChannel` and `Actor`, however, are context-specific I/O types. It is
  // expected that an ActorChannel's get() method can accept any ActorId generated for the same
  // worker (by the IoChannelFactory for any IoContext), but will detect if the ID is not valid
  // for the specific namespace.
  class ActorId {
  public:
    // Get the string that could be passed to `idFromString()` to recreate this ID.
    virtual kj::String toString() const = 0;

    // If the ActorId was created using `idFromName()`, return a copy of the name that was passed
    // to it. Otherwise, returns null.
    virtual kj::Maybe<kj::StringPtr> getName() const = 0;

    // Compare with another ID.
    //
    // This is allowed to assume the other ID was created by some other ActorIdFactory passed to
    // one of the worker's other bindings, i.e. if all factories produce the same ID type, then
    // this can downcast to that without a dynamic check.
    virtual bool equals(const ActorId& other) const = 0;

    virtual kj::Own<ActorId> clone() const = 0;
  };

  virtual kj::Own<ActorId> newUniqueId(kj::Maybe<kj::StringPtr> jurisdiction) = 0;
  virtual kj::Own<ActorId> idFromName(kj::String name) = 0;
  virtual kj::Own<ActorId> idFromString(kj::String str) = 0;
  virtual bool matchesJurisdiction(const ActorId& id) = 0;
  virtual kj::Own<ActorIdFactory> cloneWithJurisdiction(kj::StringPtr jurisdiction) = 0;
};

}  // namespace workerd
