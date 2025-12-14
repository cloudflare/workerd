#!/usr/bin/env python3
"""
WORKERD I SPY - A code exploration game!

The game picks a random item from the workerd codebase and gives you
clues to find it. Can you guess what it is?
"""

import random


class Colors:
    """Terminal color codes."""

    CYAN = "\033[96m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    MAGENTA = "\033[95m"
    RED = "\033[91m"
    BOLD = "\033[1m"
    RESET = "\033[0m"
    DIM = "\033[2m"


# I Spy items - each has clues from vague to specific
ITEMS = [
    {
        "answer": ["IOCONTEXT", "IO CONTEXT", "IO_CONTEXT"],
        "category": "Core Concept",
        "clues": [
            "I spy something that manages the lifecycle of async operations",
            "It lives in src/workerd/io/ and is fundamental to request handling",
            "Workers use this to track pending I/O and enforce limits",
            "It rhymes with 'why-oh context'",
            "It starts with 'Io' and ends with 'Context'",
        ],
    },
    {
        "answer": ["JSG", "JAVASCRIPT GLUE"],
        "category": "Library",
        "clues": [
            "I spy something that bridges two very different worlds",
            "It uses C++ template magic to make JavaScript bindings easier",
            "Without it, every API would need manual V8 boilerplate",
            "It's a three-letter acronym that stands for JavaScript Glue",
            "The letters are J, S, and G",
        ],
    },
    {
        "answer": ["DURABLE OBJECT", "DURABLEOBJECT", "DURABLE OBJECTS", "ACTOR"],
        "category": "Feature",
        "clues": [
            "I spy something that remembers things even after sleeping",
            "It has its own SQLite database and can hibernate",
            "In the code, it's often called an 'actor'",
            "It's 'Durable' and it's an 'Object'",
            "Cloudflare's stateful compute primitive",
        ],
    },
    {
        "answer": ["KJ", "KJ LIBRARY"],
        "category": "Library",
        "clues": [
            "I spy something named after someone's initials",
            "It provides Promise, Own, Maybe, and much more",
            "Cap'n Proto depends on this C++ toolkit library",
            "Two letters, both consonants, sounds like a radio station",
            "The initials are K and J",
        ],
    },
    {
        "answer": ["CAPNP", "CAP'N PROTO", "CAPN PROTO", "CAPNPROTO"],
        "category": "Technology",
        "clues": [
            "I spy something that serializes data blazingly fast",
            "It uses schemas defined in .capnp files",
            "The name sounds like a pirate's rank",
            "It's an alternative to Protocol Buffers",
            "Cap'n _____ (rhymes with 'proto')",
        ],
    },
    {
        "answer": ["V8", "V8 ENGINE"],
        "category": "Technology",
        "clues": [
            "I spy something that executes JavaScript at incredible speed",
            "It was created by Google for Chrome",
            "The name sounds like a vegetable juice or an engine",
            "It powers both Chrome and Node.js",
            "It's a single letter followed by a single digit",
        ],
    },
    {
        "answer": ["ISOLATE"],
        "category": "Core Concept",
        "clues": [
            "I spy something that keeps Workers separated from each other",
            "V8 uses this term for an independent JavaScript runtime",
            "It provides memory and execution isolation",
            "The word means 'to set apart'",
            "Starts with 'Iso', ends with 'late'",
        ],
    },
    {
        "answer": ["BAZEL"],
        "category": "Tool",
        "clues": [
            "I spy something that builds workerd (very slowly, some say)",
            "It was created by Google and uses Starlark for configuration",
            "Build files are named BUILD.bazel",
            "It's an anagram of 'blaze' (almost)",
            "Rhymes with 'hazel'",
        ],
    },
    {
        "answer": ["PROMISE", "KJ::PROMISE"],
        "category": "Type",
        "clues": [
            "I spy something that represents a future value",
            "In KJ, it's used for all async operations",
            "You can .then() it or co_await it",
            "It's a commitment to deliver something later",
            "Starts with 'P', rhymes with 'promise' (because it is)",
        ],
    },
    {
        "answer": ["WEBSOCKET", "WEB SOCKET"],
        "category": "API",
        "clues": [
            "I spy something that enables real-time bidirectional communication",
            "It upgrades from HTTP and stays connected",
            "Messages can flow both ways without new requests",
            "Two words: one is 'Web', one is where you plug things",
            "Web_____",
        ],
    },
    {
        "answer": ["WASM", "WEBASSEMBLY", "WEB ASSEMBLY"],
        "category": "Technology",
        "clues": [
            "I spy something that lets you run compiled code in the browser",
            "C, C++, and Rust can compile to this target",
            "It's a binary instruction format for a stack-based VM",
            "Four letters, stands for Web Assembly",
            "W-A-S-M",
        ],
    },
    {
        "answer": ["FETCH", "FETCH API"],
        "category": "API",
        "clues": [
            "I spy something that retrieves resources from the network",
            "It replaced XMLHttpRequest in modern JavaScript",
            "Dogs do this with sticks, browsers do it with URLs",
            "It returns a Promise<Response>",
            "Rhymes with 'sketch'",
        ],
    },
    {
        "answer": ["MAYBE", "KJ::MAYBE"],
        "category": "Type",
        "clues": [
            "I spy something that might contain a value, or might not",
            "In KJ, it's used instead of null pointers",
            "It's like std::optional but KJ-style",
            "The word expresses uncertainty",
            "_____ yes, _____ no",
        ],
    },
    {
        "answer": ["OWN", "KJ::OWN"],
        "category": "Type",
        "clues": [
            "I spy something that expresses unique ownership",
            "In KJ, it's similar to std::unique_ptr",
            "When it goes out of scope, it cleans up",
            "Three letters, means 'to possess'",
            "You _____ it, nobody else does",
        ],
    },
    {
        "answer": ["CACHE", "CACHE API"],
        "category": "API",
        "clues": [
            "I spy something that stores responses for later",
            "It helps avoid redundant network requests",
            "CDNs are really good at this",
            "Pronounced like 'cash' (money)",
            "The opposite of fetching fresh every time",
        ],
    },
    {
        "answer": ["STREAMS", "STREAMS API", "READABLESTREAM"],
        "category": "API",
        "clues": [
            "I spy something that handles data piece by piece",
            "You don't need to load everything into memory at once",
            "ReadableStream and WritableStream are examples",
            "Rivers have these, so does the Web Platform",
            "Data flows like water in a _____",
        ],
    },
    {
        "answer": ["CRYPTO", "WEB CRYPTO", "WEBCRYPTO"],
        "category": "API",
        "clues": [
            "I spy something that keeps secrets safe",
            "It can hash, encrypt, sign, and verify",
            "SubtleCrypto is part of this API",
            "Short for cryptography",
            "The Web _____ API",
        ],
    },
    {
        "answer": ["WORKERD"],
        "category": "Project",
        "clues": [
            "I spy the thing you're literally working on right now",
            "It's Cloudflare's open-source Workers runtime",
            "The name is 'worker' plus a letter",
            "It ends with a 'd' for 'daemon'",
            "_ _ _ _ _ _ d",
        ],
    },
    {
        "answer": ["ACTOR CACHE", "ACTORCACHE"],
        "category": "Component",
        "clues": [
            "I spy something that makes Durable Objects faster",
            "It caches storage operations in memory",
            "Lives in src/workerd/io/",
            "Two words: what DO's are called + what stores things",
            "_____ Cache (hint: DOs are also called this)",
        ],
    },
    {
        "answer": ["EVENT LOOP", "EVENTLOOP"],
        "category": "Core Concept",
        "clues": [
            "I spy something that goes round and round forever",
            "It processes callbacks and I/O notifications",
            "JavaScript is single-threaded because of this",
            "Two words: something that happens + something circular",
            "The _____ Loop",
        ],
    },
]


def print_header():
    """Print the game header."""
    print()
    print(
        f"{Colors.BOLD}{Colors.CYAN}"
        f"╔═══════════════════════════════════════════════════════════╗"
        f"{Colors.RESET}"
    )
    print(
        f"{Colors.BOLD}{Colors.CYAN}"
        f"║              WORKERD I SPY                                ║"
        f"{Colors.RESET}"
    )
    print(
        f"{Colors.BOLD}{Colors.CYAN}"
        f"║    Guess the workerd concept from the clues!              ║"
        f"{Colors.RESET}"
    )
    print(
        f"{Colors.BOLD}{Colors.CYAN}"
        f"╚═══════════════════════════════════════════════════════════╝"
        f"{Colors.RESET}"
    )
    print()
    print(f"  {Colors.DIM}Type 'quit' to exit, 'skip' to skip{Colors.RESET}")
    print(f"  {Colors.DIM}Type 'hint' for the next clue{Colors.RESET}")
    print()


def normalize(text):
    """Normalize text for comparison."""
    return text.upper().strip().replace("_", " ").replace("-", " ")


def check_answer(guess, answers):
    """Check if the guess matches any valid answer."""
    normalized_guess = normalize(guess)
    for answer in answers:
        if normalize(answer) == normalized_guess:
            return True
    return False


def play_round(item, round_num, total_rounds):
    """Play a single round of I Spy."""
    clues = item["clues"]
    answers = item["answer"]
    category = item["category"]
    clue_index = 0
    max_clues = len(clues)
    attempts = 0
    max_attempts = 10

    print(f"{Colors.BOLD}Round {round_num}/{total_rounds}{Colors.RESET}")
    print(f"{Colors.DIM}Category: {category}{Colors.RESET}")
    print()

    # Show first clue
    print(f"{Colors.YELLOW}Clue 1/{max_clues}:{Colors.RESET} {clues[clue_index]}")
    print()

    while attempts < max_attempts:
        try:
            guess = input(f"  {Colors.GREEN}Your guess:{Colors.RESET} ").strip()
        except (EOFError, KeyboardInterrupt):
            return None, 0

        if guess.lower() == "quit":
            return None, 0

        if guess.lower() == "skip":
            print(
                f"\n  {Colors.RED}Skipped!{Colors.RESET} "
                f"The answer was: {Colors.BOLD}{answers[0]}{Colors.RESET}\n"
            )
            return False, 0

        if guess.lower() == "hint":
            clue_index += 1
            if clue_index < max_clues:
                print(
                    f"\n{Colors.YELLOW}Clue {clue_index + 1}/{max_clues}:{Colors.RESET} "
                    f"{clues[clue_index]}\n"
                )
            else:
                print(f"\n  {Colors.DIM}No more clues available!{Colors.RESET}\n")
                clue_index = max_clues - 1
            continue

        attempts += 1

        if check_answer(guess, answers):
            # Calculate score based on clues used
            clues_used = clue_index + 1
            score = max(1, max_clues - clue_index) * 10
            print(
                f"\n  {Colors.GREEN}{Colors.BOLD}Correct!{Colors.RESET} "
                f"You got it with {clues_used} clue(s)!"
            )
            print(f"  {Colors.MAGENTA}+{score} points{Colors.RESET}\n")
            return True, score

        # Wrong answer
        print(f"  {Colors.RED}Not quite...{Colors.RESET} ", end="")
        if clue_index < max_clues - 1:
            print("Type 'hint' for another clue.")
        else:
            print("No more clues, but keep guessing!")
        print()

    # Out of attempts
    print(
        f"\n  {Colors.RED}Out of attempts!{Colors.RESET} "
        f"The answer was: {Colors.BOLD}{answers[0]}{Colors.RESET}\n"
    )
    return False, 0


def play_game():
    """Main game loop."""
    print_header()

    # Select random items for this game
    num_rounds = min(5, len(ITEMS))
    selected_items = random.sample(ITEMS, num_rounds)

    total_score = 0
    correct = 0

    for i, item in enumerate(selected_items, 1):
        result, score = play_round(item, i, num_rounds)

        if result is None:  # User quit
            return False

        if result:
            correct += 1
            total_score += score

        if i < num_rounds:
            print(f"{Colors.DIM}{'─' * 50}{Colors.RESET}")
            print()

    # Game over - show results
    print(f"{Colors.CYAN}{'═' * 50}{Colors.RESET}")
    print(f"{Colors.BOLD}Game Over!{Colors.RESET}")
    print()
    print(f"  Correct: {Colors.GREEN}{correct}/{num_rounds}{Colors.RESET}")
    print(f"  Score:   {Colors.MAGENTA}{total_score} points{Colors.RESET}")
    print()

    # Rating based on score
    if correct == num_rounds:
        rating = "WORKERD WIZARD! You know the codebase inside out!"
    elif correct >= num_rounds * 0.8:
        rating = "Senior Engineer! Impressive knowledge!"
    elif correct >= num_rounds * 0.6:
        rating = "Solid contributor! Keep exploring!"
    elif correct >= num_rounds * 0.4:
        rating = "Getting there! Time to read more source code."
    else:
        rating = "Newbie! But everyone starts somewhere. Keep learning!"

    print(f"  {Colors.YELLOW}{rating}{Colors.RESET}")
    print(f"{Colors.CYAN}{'═' * 50}{Colors.RESET}")
    print()

    return True


def main():
    """Main entry point."""
    while True:
        success = play_game()

        if not success:
            print(f"\n  {Colors.CYAN}Thanks for playing WORKERD I SPY!{Colors.RESET}\n")
            break

        try:
            again = input(f"  {Colors.GREEN}Play again? (y/n):{Colors.RESET} ").strip()
            again = again.lower()
        except (EOFError, KeyboardInterrupt):
            print(
                f"\n\n  {Colors.CYAN}Thanks for playing WORKERD I SPY!{Colors.RESET}\n"
            )
            break

        if again != "y":
            print(f"\n  {Colors.CYAN}Thanks for playing WORKERD I SPY!{Colors.RESET}\n")
            break

        print("\n" + "=" * 50 + "\n")


if __name__ == "__main__":
    main()
