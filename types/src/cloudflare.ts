// This file extends `standards.ts` with specific comments overrides for Cloudflare Workers APIs
// that aren't adequately described by a standard .d.ts file

import { CommentsData } from "./transforms";
export default {
  caches: {
    $: `*
* The Cache API allows fine grained control of reading and writing from the Cloudflare global network cache.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/)
`,
  },
  CacheStorage: {
    $: `*
* The Cache API allows fine grained control of reading and writing from the Cloudflare global network cache.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/)
`,
  },
  Cache: {
    $: `*
* The Cache API allows fine grained control of reading and writing from the Cloudflare global network cache.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/)
`,
    delete: ` [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/#delete) `,
    match: ` [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/#match) `,
    put: ` [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/cache/#put) `,
  },
  crypto: {
    $: `*
* The Web Crypto API provides a set of low-level functions for common cryptographic tasks.
* The Workers runtime implements the full surface of this API, but with some differences in
* the [supported algorithms](https://developers.cloudflare.com/workers/runtime-apis/web-crypto/#supported-algorithms)
* compared to those implemented in most browsers.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/web-crypto/)
`,
  },
  Crypto: {
    $: `*
* The Web Crypto API provides a set of low-level functions for common cryptographic tasks.
* The Workers runtime implements the full surface of this API, but with some differences in
* the [supported algorithms](https://developers.cloudflare.com/workers/runtime-apis/web-crypto/#supported-algorithms)
* compared to those implemented in most browsers.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/web-crypto/)
`,
  },
  performance: {
    $: `*
* The Workers runtime supports a subset of the Performance API, used to measure timing and performance,
* as well as timing of subrequests and other operations.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/performance/)
`,
  },
  Performance: {
    $: `*
* The Workers runtime supports a subset of the Performance API, used to measure timing and performance,
* as well as timing of subrequests and other operations.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/performance/)
`,
    timeOrigin: ` [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/performance/#performancetimeorigin) `,
    now: ` [Cloudflare Docs Reference](https://developers.cloudflare.com/workers/runtime-apis/performance/#performancenow) `,
  },
  self: {
    $: undefined,
  },
  navigator: {
    $: undefined,
  },
  origin: {
    $: undefined,
  },
  WorkerGlobalScope: {
    $: undefined,
  },
  DurableObjectNamespace: {
    $: `*
* A Durable Object namespace is a binding that allows a Worker to send messages to a Durable Object.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/namespace/)
`,
    newUniqueId: `*
* Creates a new unique DurableObjectId. Use this when the Durable Object does not need to be addressed
* by a well-known name, such as for session IDs that can be stored in a cookie or URL.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/namespace/#newuniqueid)
`,
    idFromName: `*
* Creates a DurableObjectId for an instance with the provided name. This method always returns
* the same ID for the same name, making it ideal for objects that need to be addressed by a well-known name.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/namespace/#idfromname)
`,
    idFromString: `*
* Recreates a DurableObjectId from a previously stringified ID. Use this to restore an ID that was
* stored elsewhere, such as in a database or session cookie. Throws if the ID is not a valid 64-digit
* hex number, or if the ID was not originally created for this class.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/namespace/#idfromstring)
`,
    get: `*
* Obtains a stub for the Durable Object instance corresponding to the given ID, creating the Durable
* Object if it doesn't already exist. The stub is a client that can be used to send messages to the
* remote Durable Object.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/namespace/#get)
`,
    getByName: `*
* Obtains a stub for the Durable Object instance with the given name. This is a convenience method
* equivalent to calling \`idFromName()\` followed by \`get()\`.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/namespace/#getbyname)
`,
    getExisting: `*
* Obtains a stub for the Durable Object instance corresponding to the given ID, but only if that
* instance already exists. Unlike \`get()\`, this will not create a new Durable Object.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/namespace/#get)
`,
    jurisdiction: `*
* Creates a subnamespace scoped to the specified jurisdiction. All Durable Object IDs and stubs
* created from the subnamespace will be restricted to the specified jurisdiction.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/namespace/#jurisdiction)
`,
  },
  DurableObjectId: {
    $: `*
* A 64-digit hexadecimal number used to identify a Durable Object. Durable Object IDs are
* constructed via the DurableObjectNamespace interface. Creating an ID does not create the object;
* the object is created lazily when the stub is first used.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/id/)
`,
    toString: `*
* Converts this ID to a 64-digit hex string that can be stored and later used to recreate
* the ID via \`idFromString()\`.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/id/#tostring)
`,
    equals: `*
* Compares this ID with another DurableObjectId for equality.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/id/#equals)
`,
    name: `*
* The name that was used to create this ID via \`idFromName()\`, or undefined if the ID was
* created using \`newUniqueId()\`.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/id/#name)
`,
  },
  DurableObjectStub: {
    $: `*
* A client used to invoke methods on a remote Durable Object. The stub is generic, allowing
* RPC methods defined on the Durable Object class to be invoked directly on the stub.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/stub/)
`,
    id: `*
* The DurableObjectId corresponding to this stub.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/stub/#id)
`,
    name: `*
* The name that was used to create this stub, or undefined if the stub was created from
* an ID generated by \`newUniqueId()\`.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/stub/#name)
`,
  },
  DurableObjectState: {
    $: `*
* Provides access to the Durable Object's storage and other state. Accessible via the \`ctx\`
* parameter passed to the Durable Object constructor.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/)
`,
    waitUntil: `*
* Extends the lifetime of the Durable Object to wait for the given promise. In Durable Objects,
* this has no effect as objects automatically remain active while work is pending.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#waituntil)
`,
    id: `*
* The DurableObjectId of this Durable Object instance.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#id)
`,
    storage: `*
* Provides access to the Durable Object's persistent storage.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#storage)
`,
    blockConcurrencyWhile: `*
* Executes an async callback while blocking delivery of other events to the Durable Object.
* Commonly used in the constructor to ensure initialization completes before handling requests.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#blockconcurrencywhile)
`,
    acceptWebSocket: `*
* Adds a WebSocket to the set of WebSockets attached to this Durable Object for hibernation.
* After calling this, incoming messages will be delivered to the \`webSocketMessage\` handler.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#acceptwebsocket)
`,
    getWebSockets: `*
* Returns all WebSockets attached to this Durable Object via \`acceptWebSocket()\`.
* Optionally filters by tag if one was provided when accepting the WebSocket.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#getwebsockets)
`,
    setWebSocketAutoResponse: `*
* Configures an automatic response for WebSocket ping/pong messages, allowing the Durable Object
* to remain hibernated while still responding to keep-alive messages.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#setwebsocketautoresponse)
`,
    getWebSocketAutoResponse: `*
* Returns the currently configured automatic WebSocket response, or null if none is set.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#getwebsocketautoresponse)
`,
    getWebSocketAutoResponseTimestamp: `*
* Returns the most recent Date when the given WebSocket sent an auto-response, or null if it never has.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#getwebsocketautoresponsetimestamp)
`,
    setHibernatableWebSocketEventTimeout: `*
* Sets the maximum time in milliseconds that a WebSocket event handler can run for.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#sethibernatablewebsocketeventtimeout)
`,
    getHibernatableWebSocketEventTimeout: `*
* Returns the currently configured WebSocket event timeout, or null if none is set.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#gethibernatablewebsocketeventtimeout)
`,
    getTags: `*
* Returns the tags associated with the given WebSocket, as provided to \`acceptWebSocket()\`.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#gettags)
`,
    abort: `*
* Forcibly resets the Durable Object, logging the provided message as an error.
* This error cannot be caught within application code.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/state/#abort)
`,
  },
  DurableObjectStorage: {
    $: `*
* Provides access to the Durable Object's persistent storage. The storage is private to this
* Durable Object instance and supports both key-value and SQL APIs.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/)
`,
    get: `*
* Retrieves the value associated with the given key, or a Map of values for multiple keys.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#get)
`,
    put: `*
* Stores a value associated with the given key, or multiple key-value pairs from an object.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#put)
`,
    delete: `*
* Deletes the given key(s) and associated value(s) from storage.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#delete)
`,
    deleteAll: `*
* Deletes all stored data, effectively deallocating all storage used by the Durable Object.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#deleteall)
`,
    list: `*
* Returns all keys and values in storage, optionally filtered by prefix or key range.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#list)
`,
    transaction: `*
* Runs a sequence of storage operations in a single atomic transaction.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#transaction)
`,
    transactionSync: `*
* Runs a synchronous callback wrapped in a transaction. Only synchronous storage operations
* (such as SQL queries) can be part of the transaction.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#transactionsync)
`,
    getAlarm: `*
* Retrieves the current alarm time in milliseconds since epoch, or null if no alarm is set.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#getalarm)
`,
    setAlarm: `*
* Sets an alarm to trigger the Durable Object's \`alarm()\` handler at the specified time.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#setalarm)
`,
    deleteAlarm: `*
* Deletes the currently set alarm, if any. Does not cancel an alarm handler that is currently executing.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#deletealarm)
`,
    sync: `*
* Synchronizes any pending writes to disk. Returns a promise that resolves when all prior
* writes have been confirmed persisted.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#sync)
`,
    sql: `*
* Provides access to the SQL API for SQLite-backed Durable Objects.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#sql)
`,
    getCurrentBookmark: `*
* Returns a bookmark representing the current point in time in the object's history, for use
* with point-in-time recovery.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#getcurrentbookmark)
`,
    getBookmarkForTime: `*
* Returns a bookmark representing approximately the given point in time, which must be within the last 30 days.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#getbookmarkfortime)
`,
    onNextSessionRestoreBookmark: `*
* Configures the Durable Object to restore its storage to the given bookmark on next restart.
* Typically followed by \`ctx.abort()\` to trigger the restoration.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#onnextsessionrestorebookmark)
`,
  },
  DurableObjectTransaction: {
    $: `*
* Provides transactional access to Durable Object storage within a \`transaction()\` callback.
* Operations on this object are executed atomically.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#transaction)
`,
    get: `*
* Retrieves the value associated with the given key within the transaction.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#get)
`,
    put: `*
* Stores a value associated with the given key within the transaction.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#put)
`,
    delete: `*
* Deletes the given key(s) within the transaction.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#delete)
`,
    list: `*
* Returns all keys and values within the transaction, optionally filtered.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#list)
`,
    rollback: `*
* Ensures all changes made during the transaction will be rolled back rather than committed.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#transaction)
`,
    getAlarm: `*
* Retrieves the current alarm time within the transaction.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#getalarm)
`,
    setAlarm: `*
* Sets an alarm within the transaction.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#setalarm)
`,
    deleteAlarm: `*
* Deletes the alarm within the transaction.
*
* [Cloudflare Docs Reference](https://developers.cloudflare.com/durable-objects/api/sqlite-storage-api/#deletealarm)
`,
  },
} satisfies CommentsData;
