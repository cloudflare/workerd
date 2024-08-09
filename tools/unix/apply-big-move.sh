#! /bin/bash
#
# This script applies the most recent "big change".
#
# This script is designed so that it can help you rebase an in-flight PR across this change. Once
# the big move has been merged, you may simply run this script inside your branch, and it will
# take care of updating your change in order to avoid any merge conflicts.
#
# How it does this:
# 1. Rebases your change to the commit just before the Big Move.
# 2. Applies the same logical changes that the Big Move did, but now applies this to the changes
#    on your branch too.
# 3. Rewrites git history so that your commits are now based on the commit immediately after the
#    Big Move.
#
# To use the script, just run it in your branch. It'll take care of everything, and you'll end up
# with the changes in your branch all based on the commit after The Big Move.

set -euo pipefail

AFTER_MOVE_TAG=after-big-move-1
BEFORE_MOVE_TAG=before-big-move-1
NEXT_BIG_MOVE=before-big-move-2
ORIG_BRANCH=$(git branch --show-current)

fail() {
  echo -e '\e[0;1;31mFAILED:\e[0m '"$@" >&2
  exit 1
}

check_big_move_dependencies() {
  source "$(dirname -- $BASH_SOURCE)/find-python3.sh"
  PYTHON_PATH=$(get_python3)
  if [[ -z "$PYTHON_PATH" ]]; then
    fail "Cannot find python3. Run install python3 first."
  fi
}

apply_big_move() {
  echo "Applying format..."

  source "$(dirname -- $BASH_SOURCE)/find-python3.sh"
  PYTHON_PATH=$(get_python3)
  $PYTHON_PATH "$(dirname -- $BASH_SOURCE)/../cross/format.py"

  echo "Changes applied successfully."
}

rebase_and_rerun() {
  # Rebase on top of tag $1 and then run the script again.

  BASE=$1

  echo -e '\e[0;1;34mRebasing onto tag '"$BASE"'...\e[0m'
  git rebase $BASE || \
      fail "Rebase had conflicts. Please resolve them and run the script again."
  git submodule update

  # Make sure we're using the correct version of the script by starting it over.
  exec ./apply-big-move.sh
}

check_for_next_move() {
  # Check if there's a new "Big Move" in the future and, if so, prompt the user to apply it too.

  if git describe $NEXT_BIG_MOVE >/dev/null 2>&1; then
    while true; do
      echo -en '\e[0;1;33mIt looks like there is another big move after this one. Should we apply it now? [y/n]\e[0m '
      read -n1 CHOICE
      echo
      case "$CHOICE" in
        y | Y )
          rebase_and_rerun $NEXT_BIG_MOVE
          ;;
        n | N )
          echo "OK. When you are ready, please run the script again to apply the next move."
          exit 0
          ;;
      esac
      echo "??? Please press either y or n."
    done
  fi
}

main() {
  if [ "x$(git status --untracked-files=no --porcelain)" != "x" ]; then
    fail "You have uncommitted changes. Please commit or discard them first."
  fi

  check_big_move_dependencies

  # Use --apply to just apply the Big Move changes to the current tree without committing or
  # rewriting any history. This is intended to be used to create the Big Move commit in the first
  # place.
  if [ "${1:-}" = "--apply" ]; then
    apply_big_move
    exit 0
  fi

  # Make sure our tags are up-to-date so we can find the relevant Big Move tags.
  echo "Fetching tags..."
  git fetch origin --tags --force

  # Check that the Big Move is actually ready. (This script will be committed to the repo before
  # the Big Move itself is, so this checks if someone is running the script too early.)
  if ! git describe $BEFORE_MOVE_TAG >/dev/null 2>&1; then
    fail "The Big Move hasn't happened yet (tags not found)."
  fi

  # Check if we already applied The Big Move in this branch, as indicated by $AFTER_MOVE_TAG being
  # in our history.
  if git merge-base --is-ancestor $AFTER_MOVE_TAG HEAD; then
    # This branch already includes The Big Move, but maybe there is a future Big Move and the
    # user was actually intending to apply that?
    if git describe $NEXT_BIG_MOVE >/dev/null 2>&1; then
      # Indeed there is, let's skip forward to that.
      rebase_and_rerun $NEXT_BIG_MOVE
    fi

    echo "The Big Move has already been applied to this branch."
    exit 0
  fi

  # Check if $BEFORE_MOVE_TAG is in this branch's history. If not, we need to rebase first.
  if ! git merge-base --is-ancestor $BEFORE_MOVE_TAG HEAD; then
    # Branch is not yet based on $BEFORE_MOVE_TAG, so rebase onto it.
    rebase_and_rerun $BEFORE_MOVE_TAG
    fail "(can't get here -- rebase_and_rerun should not return)"
  fi

  # Get the list of commits that we need to rebase over The Big Move.
  COMMITS=($(git log --reverse --format='%H' $BEFORE_MOVE_TAG..HEAD))

  # Checkout $AFTER_MOVE_TAG in order to start building on it.
  git checkout -q $AFTER_MOVE_TAG
  git submodule update

  # Apply each commit.
  for COMMIT in ${COMMITS[@]}; do
    git log -1 --format='%CblueRewriting commit %h:%Creset %s' $COMMIT

    # Update the working tree to match the code from the source commit.
    git checkout $COMMIT .
    git submodule update

    # Apply the Big Move on top of that.
    apply_big_move

    # Commit to edgeworker (including updating the workerd submodule), reusing the old commit
    # message. It's possible that the diff is now empty, in which case we skip this commit, much
    # like `git rebase` does by default.
    if git commit -a --dry-run > /dev/null 2>&1; then
      git commit -aC $COMMIT
    fi
  done

  # Record the final commit.
  FINAL_COMMIT=$(git log -1 --format=%H)

  # Check out the original branch, and reset it to the final commit.
  git checkout -q "$ORIG_BRANCH"
  git reset --hard "$FINAL_COMMIT"

  # Success!
  echo -e '\e[0;1;32mSUCCESS:\e[0m Applied Big Move.'

  # Check if there's another move upcoming that we should apply next.
  check_for_next_move

  exit 0
}

# Ensure that if the file changes while bash is reading it, bash won't go off the rails. Normally,
# bash reads one line from the file at a time and then executes it before reading the next.
# Wrapping the whole script body in a function ensures that bash buffers it in memory. Placing
# `exit 0` on the same line ensures that bash won't attempt to read anything more after the
# function returns.
#
# This ensures that if further changes are made to this file before the Big Move actually happens,
# then the rebase command that rebases to the commit before the Big Move won't confuse bash.
main "$@"; exit 0
