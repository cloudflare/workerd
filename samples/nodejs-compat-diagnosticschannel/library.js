import { channel } from "node:diagnostics_channel";

const theChannel = channel('test');

export function doSomething() {
  theChannel.publish('Hello from worker!');
}
