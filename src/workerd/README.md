The subdirectories are organized as follows:

* **util:** Contains random unrelated utilities that don't depend on each other or the rest of the system.
* **jsg:** Contains magic template library for auto-generating FFI glue between C++ and V8 JavaScript.
* **io:** Generally contains code that handles the I/O layer which allows APIs to talk to the rest of the world. Also includes basic Worker lifecycle and event delivery.
* **api:** Contains implementations of publicly documented application-visible JavaScript APIs.
* **server:** Contains the high-level server implementation.
* **tools:** Contains additional meta-programs, notably a script for exporting API types. 