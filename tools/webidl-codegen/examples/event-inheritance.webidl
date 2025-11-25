// Example showing inheritance with Event
// https://dom.spec.whatwg.org/#interface-event

[Exposed=*]
interface Event {
  constructor(DOMString type, optional EventInit eventInitDict = {});

  readonly attribute DOMString type;
  readonly attribute EventTarget? target;
  readonly attribute EventTarget? currentTarget;
  readonly attribute unsigned short eventPhase;

  undefined stopPropagation();
  undefined stopImmediatePropagation();

  readonly attribute boolean bubbles;
  readonly attribute boolean cancelable;
  undefined preventDefault();
  readonly attribute boolean defaultPrevented;
};

dictionary EventInit {
  boolean bubbles = false;
  boolean cancelable = false;
};

[Exposed=*]
interface EventTarget {
  undefined addEventListener(DOMString type, EventListener? callback);
  undefined removeEventListener(DOMString type, EventListener? callback);
  boolean dispatchEvent(Event event);
};

callback interface EventListener {
  undefined handleEvent(Event event);
};

// Custom event that extends Event
[Exposed=*]
interface MessageEvent : Event {
  constructor(DOMString type, optional MessageEventInit eventInitDict = {});
  
  readonly attribute any data;
  readonly attribute DOMString origin;
  readonly attribute DOMString lastEventId;
  readonly attribute MessagePort? source;
};

dictionary MessageEventInit : EventInit {
  any data = null;
  DOMString origin = "";
  DOMString lastEventId = "";
  MessagePort? source = null;
};

[Exposed=*]
interface MessagePort : EventTarget {
  undefined postMessage(any message);
  undefined start();
  undefined close();
};
