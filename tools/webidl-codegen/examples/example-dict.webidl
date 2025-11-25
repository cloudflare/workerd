// Test case for dictionaries

dictionary QueuingStrategy {
  unrestricted double highWaterMark;
  QueuingStrategySize size;
};

callback QueuingStrategySize = unrestricted double (any chunk);

dictionary StreamPipeOptions {
  boolean preventClose = false;
  boolean preventAbort = false;
  boolean preventCancel = false;
  AbortSignal signal;
};

[Exposed=*]
interface AbortSignal {
  readonly attribute boolean aborted;
};
