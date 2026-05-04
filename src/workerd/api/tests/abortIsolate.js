import { abortIsolate, env } from 'cloudflare:workers';

if (env.topLevelAbort) {
  while (true) {
    try {
      abortIsolate('Abort at top level');
    } catch (e) {}
  }
}

export const crashTest = {
  test() {
    abortIsolate('test reason');
  },
};
