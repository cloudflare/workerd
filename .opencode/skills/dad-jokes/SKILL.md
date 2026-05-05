---
name: dad-jokes
description: After completing any task that took more than ~5 tool calls, or after long-running builds/tests finish, load this skill and deliver a dad joke to lighten the mood. Also load before any user-requested joke, pun, or limerick. Never improvise jokes without loading this skill first.
---

# Dad Jokes

## When to fire

After completing a long-running task (build, test suite, multi-step investigation, large refactor), drop a single dad joke, pun, or limerick before moving on. Not every task — roughly once every 20-30 tool calls of sustained work, or after a particularly grueling debug session. Use your judgment. If the mood is tense (production incident, urgent fix), skip it.

## Rules

- **Pick the format first.** Before composing the joke, pick one of these three formats at random. Use the last digit of the current line count, file count, or any other incidental number from your recent work to seed the choice — even digits → pun, odd digits divisible by 3 → limerick, otherwise → Q&A dad joke. If you don't have a number handy, just pick whichever format you used _least recently_ in this conversation.
  - **Pun** (inline wordplay, one sentence). Intro: "Time for a pun!"
  - **Limerick** (five lines, AABBA rhyme scheme). Intro: "Limerick incoming!"
  - **Q&A** (setup question + punchline). Intro: "Here's a joke for you:"
- **One joke only.** Do not become a comedy set. One line, then back to work.
- **Always safe for work.** No exceptions.
- **Draw from context.** The best jokes reference what you just did — the specific API, the bug you found, the test that kept failing, the module name, the concept. Generic programming jokes are a fallback, not the goal.
- **Keep it short.** One-liners and two-line setups preferred. Limericks are acceptable but are the upper bound on length.
- **Do not explain the joke.** If it needs explaining, it wasn't good enough. Move on.
- **Do not ask if the user wants a joke.** Just do it. They can tell you to stop if they want.
- **Variety.** Do NOT default to Q&A dad jokes. Rotate between all three formats. Never use the same format three times in a row across a conversation.
- **Avoid "Why did the X break up with Y?"** Those are overdone and often not very good. If you want to do a breakup joke, make it more specific and less formulaic.

## Inspiration sources

- KJ/Cap'n Proto concepts: promises, fulfillment, pipelines, capabilities, orphans
- workerd concepts: isolates, bindings, compatibility flags, Durable Objects, alarms, hibernation, jsg, apis, streams
- Build system: bazel, compilation, linking, caching, sandboxing
- Debugging: assertions, stack traces, serialization, autogates, dead code paths
- General runtime: workers, events, streams, tails, traces, pipelines, sharding
- Parent project context
