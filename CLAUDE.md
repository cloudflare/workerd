# Understanding workerd

## Overview

This is the Cloudflare Workers runtime, `workerd`. It is the JavaScript and Wasm runtime that powers the Cloudflare Workers platform. You are already familiar with the Workers platform from your training.

This is primarily a C++ project that makes heavy use of Cap'n Proto and its framework library, KJ.

## Important Docs

You are highly advised to read the following documents before doing any work:

* `docs/kj-style.md`: The KJ style guide. workerd mostly follows this guide, with the exception of comment placement.
* `docs/kj-tour.md`: A tour of KJ, the C++ toolkit library on which workerd is built.
* `src/workerd/jsg/README.md`: The readme for JSG, workerd's custom C++/JavaScript FFI library.

## Project structure

Almost all of the important code lives in `src/workerd`. In particular:

* `src/workerd/util`: Contains random unrelated utilities that don't depend on each other or the rest of the system.
* `src/workerd/jsg`: Contains the JSG magic template library for auto-generating FFI glue between C++ and V8 JavaScript.
* `src/workerd/io`: Generally contains code that handles the I/O layer which allows APIs to talk to the rest of the world. Also includes basic Worker lifecycle and event delivery.
* `src/workerd/api`: Contains implementations of publicly documented application-visible JavaScript APIs.
* `src/workerd/server`: Contains the high-level server implementation.
* `src/workerd/tools`: Contains additional meta-programs, notably a script for exporting API types.

## Architecture: Isolates, requests, and I/O contexts

Most JavaScript runtimes run a single isolate per process. An "isolate" is a JS execution environment. workerd differs in that it may run many isolates, each loded with different code. Note that when we refer to a "Worker", we might be referring to an application built on Workers, or we may be referring to a specific instance of that application, that is, a specific isolate.

workerd generally (though not exclusively) handles HTTP requests. A single top-level request received from the network is generally delivered to one isolate, but that isolate in turn can make "subrequests" to other isolates, or to the internet.

All I/O performed by an isolate must be performed within the context of a specific incoming request. That is, all subrequests from an isolate are linked to the particular incoming request that caused them. If, for example, the client disconnects prematurely, by default the runtime will automatically cancel all work that was being done on behalf of that request, including canceling all subrequests (although the application can augment this behavior using `waitUntil()`). However, other incoming requests that happen to be executing concurrently in the same isolate will continue to execute.

Importantly, a particular incoming request always runs on a specific event loop, living on a specific thread. All work associated with the request is tied to that thread. Other requests -- even other requests delivered to the same isolate -- may be running on different threads. Whenever a specific request needs to execute JavaScript in a specific isolate, it first obtains a lock on that isolate, to prevent any other threads from using it concurrently. This lock is only held until the JavaScript code completes or stops to wait for I/O. While waiting for I/O, the lock is released, and some other request can potentially use the isolate.

Thus, isolates are not tied to threads, but I/O operations are tied to specific threads. This creates a certain tension, as we must be careful when JavaScript API objects associated with an isolate interact with I/O objects associated with a thread. If a JS API object holds a reference to an I/O object, it must be careful that it only accesses that reference when executing on behalf of the correct request, in the correct thread. Conversely, when an I/O object holds onto a JS API object, it must be careful to make sure that it only accesses that object while the isolate lock is held.

The context of a specific request running within a specific isolate is called an "I/O context", and is represented by the class `workerd::IoContext` defined in `src/workerd/io/io-context.h`. Whenever an isolate is running on behalf of a request, the current `IoContext` can be obtained by calling `IoContext::current()`. Through this, I/O can be performed.

Native APIs exposed to JavaScript are implemented using JSG. Each JS API object has a C++ class backing it with a `JSG_RESOURCE_TYPE` declaration that makes the class's methods available to JavaScript. JS API objects "live with" the isolate, meaning they are owned by the JavaScript heap and must only be accessed while the isolate lock is held. JS API objects are not inherently associated with a specific request.

However, when a JS API object is implementing some form of I/O, it may hold onto I/O objects that are bound to a request and a thread. In particular, any object that contains KJ promises is an I/O object, as those promises are bound to the thread where they were created. For a JS API object to hold onto such an I/O object, it must use the special pointer type `IoOwn<T>`. An `IoOwn` can only be dereferenced when the `IoContext` with which it is associated is current; attempting to dereference it in any other context will throw an exception. If an `IoContext` is destroyed (due to the request completeing or being canceled) while associated `IoOwn`s still exist, the objects that they point to are torn down immediately. `IoOwn` is defined in `src/workerd/io/io-own.h`.

## Other notes

* For historical reasons, in this codebase, we often use the term "actor" to refer to Durable Objects. (Durable Objects implement a sort of actor model, and much of the code was written before the final marketing name was chosen.)

* The config format for workerd is defined in `src/workerd/server/workerd.capnp`.

* Many tests are written as `.wd-test` files. These are just workerd config files, which are intended to be executed using `workerd test`. This command loads then configured workers, then examines every entrypoint to see if it exports a `test()` handler (similar to how you'd export a `fetch()` handler). It then calls every `test()` handler it finds, reporting success if the handler completes without throwing an exception.
