# Contributing to `workerd`

Before contributing code to `workerd`, please read these guidelines carefully.

## Questions? Use "discussions".

If you just want to ask a question or have an open-ended conversation, use the "discussions" tab rather than filing an issue. This way, the issues list stays restricted to action items

## Before you code: Discuss your change

This repository is the canonical source code for the core of Cloudflare Workers. The Workers team actively does their development in this repository. Any changes landed in the main branch will automatically become available on the Cloudflare Workers Platform typically within a week or two.

Cloudflare Workers has a [strong commitment to backwards-compatibility](https://blog.cloudflare.com/backwards-compatibility-in-cloudflare-workers/). Once a feature is in use on Cloudflare's platform, it generally cannot be taken away. The Workers team will be required to maintain the feature forever. (Note that we do not use semver and cannot just bump a major version number to introduce breaking changes -- [the blog post](https://blog.cloudflare.com/backwards-compatibility-in-cloudflare-workers/) explains why!)

As a result, we are cautious about what we add. Typically, inside Cloudflare, a new feature will be discussed by product managers and described in design docs long before any code is written.

What does that mean for external contributors? The most important thing is:

**For non-trivial changes, always post an issue or discussion before you write code.**

If you have a very specific proposal for which you're seeking approval, file an issue with the label "feature proposal". If your ideas are more open-ended, they may make sense as a discussion instead. Either way, we can then discuss whether we would accept the feature and, if so, give you some hints on how to implement it. We may ask you to write a design doc and do other preparatory work before we make a decision -- just as we would for any internal engineer making such a proposal.

Please note that we set an extremely high bar for new APIs that are not defined by standards. If you are proposing adding a new non-standard API, it is very likely we will decline. Conversely, if you are proposing adding support for an API that is defined by standards, it is an easier decision for us to accept it.

(For trivial changes, it is OK to go directly to filing a PR, with the understanding that the PR issue itself will serve as the place to discuss the change idea.)

## Hacking on the code

This codebase includes many unit tests. To run them, do:

```
bazel test //src/...
```

You may find it convenient to have Bazel automatically re-run every time you change a file. You can accomplish this using [watchexec](https://github.com/watchexec/watchexec) like so:

```
watchexec --restart --watch src bazel test //src/...
```

`workerd` is based on KJ, the C++ toolkit library underlying Cap'n Proto. Before writing code, we highly recommend you check out the [KJ style guide](https://github.com/capnproto/capnproto/blob/master/style-guide.md) and the [tour of KJ](https://github.com/capnproto/capnproto/blob/master/kjdoc/tour.md) to understand how to use KJ.

### Using Visual Studio Code for development

See [this guide](docs/vscode.md) for instructions on how to set up Visual Studio Code for development.

TODO: Add more on tooling best practices, etc.

## Pull requests and code review

The Cloudflare Workers project has a culture of careful code review. If we find your code hard to review, it's likely that it will take much longer to land, or may be declined entirely for this reason alone. We apply the same standards within our own team.

To make sure your pull request is as easy to review as possible:

* **Follow the style guide.** Please see the [KJ style guide](https://github.com/capnproto/capnproto/blob/master/style-guide.md) and the [tour of KJ](https://github.com/capnproto/capnproto/blob/master/kjdoc/tour.md) to understand how we write code.

* **Split PRs into small commits wherever possible**, especially when it helps the reviewers separate concerns. The code should compile and all tests should pass at every commit. For example, if you are adding a feature that requires refactoring some code, do the refactoring in a separate commit (or series of commits) from introducing the new feature. Each commit message should describe the particular bit of refactoring in the commit and why it was needed. It's especially important to use separate commits when moving and modifying code. If you need to move a large block of code from one place to another, try your best to do it in an individual commit that is purely a block cut/paste without making any modifications to the code. Then, actually modify the code in the next commit.

* **Don't push fixup commits.** When your reviewer asks for changes, they will want you to rewrite your branch history so that the commit history is clean. We don't want have "fixup commits" in our history, but we also don't want to squash-merge a clean commit series and lose the information it provides. You may want to familiarize yourself with `git commit --fixup` -- it makes it relatively easy to rewrite history.

* **Push fixups and rebases separately.** When a reviewer asks for changes, they will wan to review what you change separately from what was already written. It is very important, therefore, that any time you force-push an update to your PR, the push _either_ contains new changes of yours, _or_ rebases onto the latest version of the `main` branch, but _never_ both as the same time. If you do both at once, when your reviewer clicks the "view changes" button, they won't be able to tell which changes are yours vs. new changes to the main branch. In this case they may ask you to revert and try again.

* **Use three-way conflict markers.** Unfortunately, it is very hard for code reviewers to review conflict resolutions in general, and this can be a source of bugs. You can reduce the probability of bugs by making sure you've enabled three-way conflict markers in git, by running `git config --global merge.conflictstyle diff3`. This makes conflicts much easier to understand, by showing not just "yours" and "theirs", but also the original code from which both were derived.

* **Do not submit code you haven't tested.** If you are not able to build your code and run it locally to verify that it does what you expect, please do not submit code changes. Even the best programmers usually write code that doesn't work on the first try. If at all possible, include unit tests for any new functionality you add.

* **Write lots of comments.** Everyone knows that they should write comments, but a lot of programmers still don't do it. Do not expect your reviewers to learn what your code does by reading the implementation. By the time they get to implementation details, they should be simply confirming that they match what was promised. Every declaration in a header file should have a comment if its purpose isn't immediately obvious from the declaration name. (Do not write comments that simply state what is already obvious from the name.) In implementation code, you should have a comment every few lines saying what the next few lines are doing and describing any non-obvious concerns that the code needs to address.
