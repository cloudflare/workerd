#!/usr/bin/env python3
"""
WORKERD WORD SCRAMBLE - Unscramble the workerd terminology!

The letters of workerd-related words have been jumbled up.
Can you figure out what they are?
"""

import random
import time


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


# Words to scramble with hints
WORDS = [
    # Core concepts
    {"word": "ISOLATE", "hint": "V8's independent JavaScript runtime environment"},
    {"word": "PROMISE", "hint": "Represents a future value in async code"},
    {"word": "CONTEXT", "hint": "IoContext manages the lifecycle of operations"},
    {"word": "WORKER", "hint": "The fundamental unit of compute on Cloudflare"},
    {"word": "BINDING", "hint": "Connects a Worker to external resources"},
    {"word": "HANDLER", "hint": "Function that processes incoming requests"},
    {"word": "CALLBACK", "hint": "Function passed to be called later"},
    {"word": "ASYNC", "hint": "Non-blocking execution style"},
    {"word": "AWAIT", "hint": "Pauses until a Promise resolves"},
    {"word": "EVENT", "hint": "Something that happens that code can respond to"},
    # APIs and features
    {"word": "FETCH", "hint": "Retrieves resources from the network"},
    {"word": "CACHE", "hint": "Stores responses for faster retrieval"},
    {"word": "STREAM", "hint": "Data that flows piece by piece"},
    {"word": "SOCKET", "hint": "Network connection endpoint"},
    {"word": "CRYPTO", "hint": "Handles encryption and hashing"},
    {"word": "HEADERS", "hint": "Metadata in HTTP requests and responses"},
    {"word": "REQUEST", "hint": "Incoming HTTP message to handle"},
    {"word": "RESPONSE", "hint": "Outgoing HTTP message to send"},
    {"word": "WEBSOCKET", "hint": "Bidirectional real-time connection"},
    {"word": "DURABLE", "hint": "_____ Objects: stateful compute primitive"},
    # Libraries and tools
    {"word": "CAPNPROTO", "hint": "Fast serialization library (pirate-themed name)"},
    {"word": "BAZEL", "hint": "Build system that takes forever"},
    {"word": "JAVASCRIPT", "hint": "The language Workers run"},
    {"word": "TYPESCRIPT", "hint": "JavaScript with types"},
    {"word": "WEBASSEMBLY", "hint": "Binary format for running compiled code"},
    # KJ types
    {"word": "MAYBE", "hint": "KJ's optional value type"},
    {"word": "ARRAY", "hint": "KJ's contiguous sequence container"},
    {"word": "STRING", "hint": "KJ's text type"},
    {"word": "EXCEPTION", "hint": "When something goes wrong"},
    {"word": "COROUTINE", "hint": "Suspendable function for async code"},
    # Storage and state
    {"word": "SQLITE", "hint": "Database engine used by Durable Objects"},
    {"word": "STORAGE", "hint": "Where Durable Objects keep their data"},
    {"word": "ACTOR", "hint": "Internal name for Durable Objects"},
    {"word": "HIBERNATE", "hint": "What Durable Objects do to save resources"},
    {"word": "TRANSACTION", "hint": "Atomic database operation"},
    # Workerd specific
    {"word": "WORKERD", "hint": "The open-source Workers runtime"},
    {"word": "ISOLATE", "hint": "Sandboxed JavaScript execution environment"},
    {"word": "GLUE", "hint": "JSG = JavaScript _____"},
    {"word": "TEMPLATE", "hint": "C++ feature JSG uses heavily"},
    {"word": "MACRO", "hint": "JSG uses many C++ _____s"},
    # Misc programming
    {"word": "MUTEX", "hint": "Mutual exclusion lock"},
    {"word": "BUFFER", "hint": "Temporary data storage area"},
    {"word": "QUEUE", "hint": "First-in-first-out data structure"},
    {"word": "STACK", "hint": "Last-in-first-out data structure"},
    {"word": "POINTER", "hint": "Memory address reference"},
    {"word": "NAMESPACE", "hint": "Scope for organizing code"},
    {"word": "MODULE", "hint": "Self-contained unit of code"},
    {"word": "EXPORT", "hint": "Make something available to importers"},
    {"word": "IMPORT", "hint": "Bring in external code"},
    {"word": "COMPILE", "hint": "Transform source to executable"},
]


def scramble_word(word):
    """Scramble a word, ensuring it's different from the original."""
    letters = list(word)
    # Keep scrambling until it's different (for words > 1 char)
    for _ in range(100):
        random.shuffle(letters)
        scrambled = "".join(letters)
        if scrambled != word or len(word) <= 1:
            return scrambled
    return "".join(letters)


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
        f"║            WORKERD WORD SCRAMBLE                          ║"
        f"{Colors.RESET}"
    )
    print(
        f"{Colors.BOLD}{Colors.CYAN}"
        f"║    Unscramble the jumbled workerd terms!                  ║"
        f"{Colors.RESET}"
    )
    print(
        f"{Colors.BOLD}{Colors.CYAN}"
        f"╚═══════════════════════════════════════════════════════════╝"
        f"{Colors.RESET}"
    )
    print()
    print(f"  {Colors.DIM}Type 'quit' to exit, 'skip' to skip{Colors.RESET}")
    print(f"  {Colors.DIM}Type 'hint' to reveal the hint{Colors.RESET}")
    print(f"  {Colors.DIM}Type 'reveal' to show one letter{Colors.RESET}")
    print()


def reveal_letter(word, revealed):
    """Reveal one more letter in the word."""
    for i, char in enumerate(word):
        if i not in revealed:
            revealed.add(i)
            return revealed
    return revealed


def display_word_with_reveals(word, revealed):
    """Display word with some letters revealed."""
    display = []
    for i, char in enumerate(word):
        if i in revealed:
            display.append(f"{Colors.GREEN}{char}{Colors.RESET}")
        else:
            display.append("_")
    return " ".join(display)


def play_round(item, round_num, total_rounds):
    """Play a single round."""
    word = item["word"]
    hint = item["hint"]
    scrambled = scramble_word(word)
    revealed = set()
    hint_shown = False
    attempts = 0
    max_attempts = 10
    start_time = time.time()

    print(f"{Colors.BOLD}Round {round_num}/{total_rounds}{Colors.RESET}")
    print(f"{Colors.DIM}({len(word)} letters){Colors.RESET}")
    print()
    print(
        f"  Scrambled: {Colors.YELLOW}{Colors.BOLD}{' '.join(scrambled)}{Colors.RESET}"
    )
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
                f"The answer was: {Colors.BOLD}{word}{Colors.RESET}\n"
            )
            return False, 0

        if guess.lower() == "hint":
            if not hint_shown:
                print(f"\n  {Colors.MAGENTA}Hint:{Colors.RESET} {hint}\n")
                hint_shown = True
            else:
                print(f"  {Colors.DIM}Hint already shown!{Colors.RESET}\n")
            continue

        if guess.lower() == "reveal":
            revealed = reveal_letter(word, revealed)
            print(f"\n  Letters: {display_word_with_reveals(word, revealed)}\n")
            if len(revealed) >= len(word) - 1:
                print(f"  {Colors.DIM}That's all the reveals you get!{Colors.RESET}\n")
            continue

        attempts += 1

        if guess.upper() == word:
            elapsed = time.time() - start_time
            # Score based on time, reveals used, and hint
            base_score = 100
            time_bonus = max(0, 50 - int(elapsed / 2))
            reveal_penalty = len(revealed) * 15
            hint_penalty = 20 if hint_shown else 0
            score = max(10, base_score + time_bonus - reveal_penalty - hint_penalty)

            print(
                f"\n  {Colors.GREEN}{Colors.BOLD}Correct!{Colors.RESET} "
                f"Solved in {elapsed:.1f}s"
            )
            print(f"  {Colors.MAGENTA}+{score} points{Colors.RESET}\n")
            return True, score

        # Wrong answer - give feedback
        print(f"  {Colors.RED}Not quite...{Colors.RESET} ", end="")

        # Check if any letters match
        guess_upper = guess.upper()
        if len(guess_upper) != len(word):
            print(f"(need {len(word)} letters)")
        else:
            matching = sum(1 for a, b in zip(guess_upper, word, strict=False) if a == b)
            if matching > 0:
                print(f"({matching} letter(s) in correct position)")
            else:
                print("(no letters in correct position)")
        print()

    # Out of attempts
    print(
        f"\n  {Colors.RED}Out of attempts!{Colors.RESET} "
        f"The answer was: {Colors.BOLD}{word}{Colors.RESET}\n"
    )
    return False, 0


def select_difficulty():
    """Let user select difficulty."""
    print(f"{Colors.BOLD}Select difficulty:{Colors.RESET}")
    print()
    print(f"  {Colors.YELLOW}1{Colors.RESET}. Easy   (4-5 letter words)")
    print(f"  {Colors.YELLOW}2{Colors.RESET}. Medium (6-7 letter words)")
    print(f"  {Colors.YELLOW}3{Colors.RESET}. Hard   (8+ letter words)")
    print(f"  {Colors.YELLOW}4{Colors.RESET}. Mixed  (all word lengths)")
    print()

    while True:
        try:
            choice = input(
                f"  {Colors.GREEN}Enter choice (1-4):{Colors.RESET} "
            ).strip()
        except (EOFError, KeyboardInterrupt):
            return None

        if choice.lower() == "quit":
            return None

        if choice == "1":
            return [w for w in WORDS if len(w["word"]) <= 5]
        elif choice == "2":
            return [w for w in WORDS if 6 <= len(w["word"]) <= 7]
        elif choice == "3":
            return [w for w in WORDS if len(w["word"]) >= 8]
        elif choice == "4":
            return WORDS
        else:
            print("  Please enter 1, 2, 3, or 4")


def play_game():
    """Main game loop."""
    print_header()

    word_pool = select_difficulty()
    if word_pool is None:
        return False

    if len(word_pool) < 5:
        word_pool = WORDS  # Fall back to all words if not enough

    print()

    # Select random words for this game
    num_rounds = min(5, len(word_pool))
    selected_words = random.sample(word_pool, num_rounds)

    total_score = 0
    correct = 0

    for i, item in enumerate(selected_words, 1):
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
    if total_score >= 400:
        rating = "ANAGRAM MASTER! Your brain unscrambles at the speed of V8!"
    elif total_score >= 300:
        rating = "Word Wizard! You could debug obfuscated code!"
    elif total_score >= 200:
        rating = "Solid Solver! Keep those neurons firing!"
    elif total_score >= 100:
        rating = "Getting there! Practice makes perfect!"
    else:
        rating = "Keep trying! Every scramble makes you stronger!"

    print(f"  {Colors.YELLOW}{rating}{Colors.RESET}")
    print(f"{Colors.CYAN}{'═' * 50}{Colors.RESET}")
    print()

    return True


def main():
    """Main entry point."""
    while True:
        success = play_game()

        if not success:
            print(
                f"\n  {Colors.CYAN}Thanks for playing WORKERD SCRAMBLE!{Colors.RESET}"
            )
            print()
            break

        try:
            prompt = f"  {Colors.GREEN}Play again? (y/n):{Colors.RESET} "
            again = input(prompt).strip().lower()
        except (EOFError, KeyboardInterrupt):
            print(
                f"\n\n  {Colors.CYAN}Thanks for playing WORKERD SCRAMBLE!{Colors.RESET}"
            )
            print()
            break

        if again != "y":
            print(
                f"\n  {Colors.CYAN}Thanks for playing WORKERD SCRAMBLE!{Colors.RESET}"
            )
            print()
            break

        print("\n" + "=" * 50 + "\n")


if __name__ == "__main__":
    main()
