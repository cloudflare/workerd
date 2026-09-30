---
name: performance
enabled: false
---
This file turns off Bonk's built-in performance specialist; without it, the built-in runs on
larger PRs. We don't want performance findings: extra copies, allocations and similar micro-costs
are not worth a review comment. Unbounded work on untrusted input and blocking the event loop are
still caught, as correctness and security defects.
