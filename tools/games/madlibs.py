#!/usr/bin/env python3
"""
WORKERD MADLIBS - A Mad Libs-style game featuring workerd-themed stories!

Fill in the blanks to create hilarious workerd-related stories.
"""

import random


# Colors for terminal output
class Colors:
    CYAN = "\033[96m"
    GREEN = "\033[92m"
    YELLOW = "\033[93m"
    MAGENTA = "\033[95m"
    BOLD = "\033[1m"
    RESET = "\033[0m"
    DIM = "\033[2m"


# Mad Libs templates - each has placeholders in {TYPE:hint} format
STORIES = [
    {
        "title": "The Great Isolate Incident",
        "story": """
Last {DAY_OF_WEEK}, a {ADJECTIVE} engineer named {SILLY_NAME} was debugging a Worker
that kept {VERB_ING} unexpectedly. The isolate was consuming {NUMBER} megabytes of
memory and making the V8 engine feel very {EMOTION}.

"Holy {EXCLAMATION}!" shouted {SILLY_NAME}, "{PLURAL_NOUN} are leaking everywhere!"

The root cause turned out to be a {ADJECTIVE} Promise that never resolved because
someone forgot to {VERB} the {WORKERD_CONCEPT}. After adding {NUMBER} lines of
{ADJECTIVE} error handling, the Worker finally started {VERB_ING} properly.

The moral of the story: always {VERB} your {PLURAL_NOUN} before deploying to
production, or your {WORKERD_CONCEPT} will end up {VERB_ING} in the void.
""",
    },
    {
        "title": "A Day in the Life of a Durable Object",
        "story": """
Once upon a time, in a {ADJECTIVE} data center, there lived a Durable Object
named {SILLY_NAME}. Every day, {SILLY_NAME} would wake up, {VERB} its SQLite
database, and prepare to handle {NUMBER} requests from {ADJECTIVE} clients.

One morning, a particularly {ADJECTIVE} request arrived asking for {NUMBER}
{PLURAL_NOUN}. "That's {EXCLAMATION}!" exclaimed the Durable Object, "I only
have {NUMBER} {PLURAL_NOUN} in my {WORKERD_CONCEPT}!"

The request kept {VERB_ING} and {VERB_ING} until finally the actor decided to
{VERB} itself into hibernation. When it woke up {NUMBER} milliseconds later,
all the {PLURAL_NOUN} had mysteriously turned into {PLURAL_NOUN}.

From that day forward, {SILLY_NAME} always made sure to {VERB} before
accepting any {ADJECTIVE} requests. The end.
""",
    },
    {
        "title": "The KJ Promise Saga",
        "story": """
In the ancient land of Cap'n Proto, there existed a {ADJECTIVE} Promise that
dreamed of one day being {VERB_PAST}. Its name was kj::Promise<{SILLY_NAME}>,
and it had been pending for {NUMBER} eternities.

"When will I finally {VERB}?" the Promise asked the {ADJECTIVE} event loop.

"You must first {VERB} the {WORKERD_CONCEPT}," replied the event loop {ADVERB},
"and collect {NUMBER} {PLURAL_NOUN} from the {ADJECTIVE} callback queue."

So the Promise set off on its {ADJECTIVE} journey, {VERB_ING} through countless
continuations and {VERB_ING} past {NUMBER} exception handlers. Along the way,
it met a {ADJECTIVE} Maybe<{PLURAL_NOUN}> who taught it the art of {VERB_ING}.

Finally, after {VERB_ING} for what felt like {NUMBER} {PLURAL_NOUN}, the Promise
reached its destination and was fulfilled with the most {ADJECTIVE}
{WORKERD_CONCEPT} anyone had ever seen. The coroutines {VERB_PAST} with joy!
""",
    },
    {
        "title": "fetch() Gone Wrong",
        "story": """
It was a {ADJECTIVE} Tuesday when the fetch() request first started acting
{ADVERB}. Instead of returning a Response, it returned {NUMBER} {PLURAL_NOUN}
wrapped in a {ADJECTIVE} {WORKERD_CONCEPT}.

"{EXCLAMATION}!" yelled the on-call engineer, {SILLY_NAME}. "The {PLURAL_NOUN}
are {VERB_ING} the entire edge network!"

Quickly, {SILLY_NAME} wrote a {ADJECTIVE} middleware that would {VERB} every
incoming request and transform it into a {ADJECTIVE} {WORKERD_CONCEPT}. But
this only made things worse - now the responses were {VERB_ING} {ADVERB}!

After {NUMBER} cups of {LIQUID} and {NUMBER} hours of debugging, {SILLY_NAME}
discovered the truth: someone had accidentally deployed a Worker that would
{VERB} all the {PLURAL_NOUN} and replace them with {ADJECTIVE} {PLURAL_NOUN}.

The fix was simple: just {VERB} the {WORKERD_CONCEPT} and {VERB} harder.
Production was saved, and {SILLY_NAME} was hailed as a {ADJECTIVE} hero.
""",
    },
    {
        "title": "The WebSocket's Lament",
        "story": """
Dear Diary,

Today I am feeling very {EMOTION}. I am a {ADJECTIVE} WebSocket, and nobody
wants to {VERB} with me anymore. They all prefer those {ADJECTIVE} HTTP/3
connections with their fancy {PLURAL_NOUN}.

I remember the good old days when {SILLY_NAME} would send me {NUMBER}
{PLURAL_NOUN} per second. We would {VERB} together for hours, {VERB_ING}
{ADVERB} into the night. But now? Now I just sit here, waiting for someone
to {VERB} my {WORKERD_CONCEPT}.

The Durable Objects next door are always {VERB_ING} and showing off their
{ADJECTIVE} SQLite databases. "Look at us!" they say, "We can {VERB}
{NUMBER} {PLURAL_NOUN} in a single transaction!" Show-offs.

Maybe tomorrow will be different. Maybe someone will finally send me a
{ADJECTIVE} message that makes me feel {EMOTION} again. Until then, I'll
just keep my connection {ADJECTIVE} and dream of {VERB_ING}.

Yours {ADVERB},
A Lonely WebSocket
""",
    },
    {
        "title": "The Bazel Build That Took Forever",
        "story": """
Legend has it that {SILLY_NAME} once started a bazel build and it's still
{VERB_ING} to this day. The build began in the year {NUMBER} when dinosaurs
still {VERB_PAST} the earth and {PLURAL_NOUN} were considered {ADJECTIVE}.

"How hard can it be to {VERB} a simple {WORKERD_CONCEPT}?" {SILLY_NAME} asked
{ADVERB}, not realizing that Bazel would need to download {NUMBER}
{PLURAL_NOUN} and compile {NUMBER} lines of {ADJECTIVE} C++ code.

Hours turned into days. Days turned into {PLURAL_NOUN}. The computer started
{VERB_ING} so loudly that the neighbors complained. The CPU got so {ADJECTIVE}
it could {VERB} {PLURAL_NOUN} on its surface.

Finally, after what felt like {NUMBER} {PLURAL_NOUN}, the build completed with
a single {ADJECTIVE} warning: "Deprecated: please {VERB} your {WORKERD_CONCEPT}."

{SILLY_NAME} stared at the screen {ADVERB}, whispered "{EXCLAMATION}," and
immediately started another build. Some say you can still hear the fans
{VERB_ING} if you listen {ADVERB} enough.
""",
    },
    {
        "title": "Error Handling: A Horror Story",
        "story": """
It was a {ADJECTIVE} and stormy night when {SILLY_NAME} decided to refactor
the error handling in the {WORKERD_CONCEPT}. "I'll just {VERB} a few try-catch
blocks," they said {ADVERB}. "What could go wrong?"

{NUMBER} hours later, the codebase was filled with {ADJECTIVE} exceptions
{VERB_ING} in every direction. KJ_REQUIRE statements multiplied like
{PLURAL_NOUN}. KJ_ASSERT macros lurked in every {ADJECTIVE} corner.

"The {PLURAL_NOUN}... they're everywhere!" screamed {SILLY_NAME} as another
JSG_FAIL message appeared on the screen. The error messages had become
{ADJECTIVE} - some were {VERB_ING}, others were just {EMOTION}.

In desperation, {SILLY_NAME} tried to {VERB} the entire {WORKERD_CONCEPT},
but it was too late. The errors had achieved sentience and were now
{VERB_ING} their own {PLURAL_NOUN}.

The only solution was to {VERB} everything and start from scratch. And so,
{SILLY_NAME} learned the most {ADJECTIVE} lesson of all: always {VERB}
your errors before they {VERB} you.
""",
    },
]

# Word type prompts
PROMPTS = {
    "ADJECTIVE": "Enter an adjective (e.g., fuzzy, gigantic, mysterious)",
    "PLURAL_NOUN": "Enter a plural noun (e.g., cats, databases, promises)",
    "VERB": "Enter a verb (e.g., run, compile, debug)",
    "VERB_ING": "Enter a verb ending in -ing (e.g., running, crashing, deploying)",
    "VERB_PAST": "Enter a verb in past tense (e.g., crashed, deployed, yeeted)",
    "NOUN": "Enter a noun (e.g., isolate, buffer, sandwich)",
    "ADVERB": "Enter an adverb (e.g., quickly, mysteriously, aggressively)",
    "NUMBER": "Enter a number (e.g., 42, 1000000, 3.14)",
    "SILLY_NAME": "Enter a silly name (e.g., BufferOverflowBob, AsyncAnnie)",
    "EXCLAMATION": "Enter an exclamation (e.g., Yikes, Holy callbacks, Great Scott)",
    "EMOTION": "Enter an emotion (e.g., happy, confused, existentially drained)",
    "WORKERD_CONCEPT": "Enter a workerd concept (e.g., isolate, Promise, IoContext)",
    "DAY_OF_WEEK": "Enter a day of the week",
    "LIQUID": "Enter a liquid (e.g., coffee, energy drink, tears)",
}

# Suggestions for each type
SUGGESTIONS = {
    "ADJECTIVE": [
        "asynchronous",
        "nullable",
        "deprecated",
        "immutable",
        "concurrent",
        "recursive",
        "volatile",
        "atomic",
    ],
    "PLURAL_NOUN": [
        "promises",
        "callbacks",
        "isolates",
        "buffers",
        "exceptions",
        "coroutines",
        "sockets",
        "actors",
    ],
    "VERB": [
        "await",
        "yield",
        "compile",
        "serialize",
        "refactor",
        "deprecate",
        "memoize",
        "instantiate",
    ],
    "VERB_ING": [
        "segfaulting",
        "blocking",
        "leaking",
        "thrashing",
        "deadlocking",
        "panicking",
        "buffering",
        "polling",
    ],
    "VERB_PAST": [
        "yeeted",
        "borked",
        "nuked",
        "obliterated",
        "corrupted",
        "deprecated",
        "refactored",
        "optimized",
    ],
    "NOUN": [
        "mutex",
        "semaphore",
        "vtable",
        "heap",
        "stack",
        "pointer",
        "iterator",
        "destructor",
    ],
    "ADVERB": [
        "asynchronously",
        "recursively",
        "frantically",
        "ominously",
        "suspiciously",
        "chaotically",
        "elegantly",
    ],
    "NUMBER": [
        "404",
        "1337",
        "42",
        "65535",
        "2147483647",
        "3.14159",
        "0xDEADBEEF",
        "∞",
    ],
    "SILLY_NAME": [
        "SegfaultSally",
        "NullPointerNate",
        "AsyncAnnie",
        "BufferBob",
        "DeadlockDave",
        "MemleakMary",
    ],
    "EXCLAMATION": [
        "Holy heap allocation",
        "Sweet subprocess",
        "Great Gosling",
        "By the power of V8",
        "Jumpin' JavaScripts",
    ],
    "EMOTION": [
        "caffeinated",
        "undefined",
        "garbage-collected",
        "race-conditioned",
        "stack-overflowed",
        "pending",
    ],
    "WORKERD_CONCEPT": [
        "IoContext",
        "Isolate",
        "DurableObject",
        "Promise",
        "EventLoop",
        "ActorCache",
        "WebSocket",
    ],
    "DAY_OF_WEEK": ["Monday", "Failday", "Crashmas", "Debugsday", "Throwsday"],
    "LIQUID": [
        "coffee",
        "Monster Energy",
        "cold brew",
        "developer tears",
        "liquid nitrogen",
    ],
}


def print_header():
    """Print the game header."""
    print()
    print(
        f"{Colors.BOLD}{Colors.CYAN}╔═══════════════════════════════════════════════════════════╗{Colors.RESET}"
    )
    print(
        f"{Colors.BOLD}{Colors.CYAN}║            WORKERD MADLIBS                                ║{Colors.RESET}"
    )
    print(
        f"{Colors.BOLD}{Colors.CYAN}║    Fill in the blanks for hilarious stories!              ║{Colors.RESET}"
    )
    print(
        f"{Colors.BOLD}{Colors.CYAN}╚═══════════════════════════════════════════════════════════╝{Colors.RESET}"
    )
    print()
    print(f"  {Colors.DIM}Type 'quit' at any time to exit{Colors.RESET}")
    print(f"  {Colors.DIM}Press Enter for a random suggestion{Colors.RESET}")
    print()


def get_input(word_type, word_num, total_words):
    """Get user input for a word type."""
    prompt = PROMPTS.get(word_type, f"Enter a {word_type.lower().replace('_', ' ')}")
    suggestions = SUGGESTIONS.get(word_type, [])

    print(f"{Colors.YELLOW}[{word_num}/{total_words}]{Colors.RESET} {prompt}")
    if suggestions:
        print(f"  {Colors.DIM}Suggestions: {', '.join(suggestions[:4])}{Colors.RESET}")

    try:
        response = input(f"  {Colors.GREEN}>{Colors.RESET} ").strip()
    except (EOFError, KeyboardInterrupt):
        return None

    if response.lower() == "quit":
        return None

    # If empty, pick a random suggestion
    if not response and suggestions:
        response = random.choice(suggestions)
        print(f"  {Colors.MAGENTA}Using: {response}{Colors.RESET}")
    elif not response:
        response = f"[{word_type}]"

    return response


def extract_placeholders(story):
    """Extract unique placeholders from a story."""
    import re

    # Find all {TYPE} patterns
    placeholders = re.findall(r"\{([A-Z_]+)\}", story)
    # Return in order of first appearance, preserving duplicates for counting
    seen = set()
    unique_ordered = []
    for p in placeholders:
        if p not in seen:
            seen.add(p)
            unique_ordered.append(p)
    return unique_ordered, placeholders


def fill_story(story_template):
    """Fill in a story template with user inputs."""
    unique_placeholders, all_placeholders = extract_placeholders(story_template)
    total_words = len(unique_placeholders)

    replacements = {}
    word_num = 0

    for placeholder in unique_placeholders:
        word_num += 1
        response = get_input(placeholder, word_num, total_words)
        if response is None:
            return None
        replacements[placeholder] = response
        print()

    # Replace all placeholders
    filled_story = story_template
    for placeholder, value in replacements.items():
        filled_story = filled_story.replace(
            "{" + placeholder + "}",
            f"{Colors.BOLD}{Colors.MAGENTA}{value}{Colors.RESET}",
        )

    return filled_story


def display_story(title, story):
    """Display the completed story."""
    print()
    print(f"{Colors.CYAN}{'═' * 60}{Colors.RESET}")
    print(f"{Colors.BOLD}{Colors.YELLOW}  {title}{Colors.RESET}")
    print(f"{Colors.CYAN}{'═' * 60}{Colors.RESET}")
    print(story)
    print(f"{Colors.CYAN}{'═' * 60}{Colors.RESET}")
    print()


def select_story():
    """Let user select a story or pick random."""
    print(f"{Colors.BOLD}Choose a story:{Colors.RESET}")
    print()
    print(f"  {Colors.YELLOW}0{Colors.RESET}. Random story")
    for i, story in enumerate(STORIES, 1):
        print(f"  {Colors.YELLOW}{i}{Colors.RESET}. {story['title']}")
    print()

    while True:
        try:
            choice = input(
                f"  {Colors.GREEN}Enter choice (0-{len(STORIES)}):{Colors.RESET} "
            ).strip()
        except (EOFError, KeyboardInterrupt):
            return None

        if choice.lower() == "quit":
            return None

        try:
            choice_num = int(choice)
            if choice_num == 0:
                return random.choice(STORIES)
            elif 1 <= choice_num <= len(STORIES):
                return STORIES[choice_num - 1]
            else:
                print(f"  Please enter a number between 0 and {len(STORIES)}")
        except ValueError:
            print("  Please enter a valid number")


def play_game():
    """Main game loop."""
    print_header()

    story_data = select_story()
    if story_data is None:
        return False

    print()
    print(
        f"{Colors.BOLD}Let's create: {Colors.CYAN}{story_data['title']}{Colors.RESET}"
    )
    print(f"{Colors.DIM}Answer the prompts below to fill in the story!{Colors.RESET}")
    print()

    filled_story = fill_story(story_data["story"])
    if filled_story is None:
        return False

    display_story(story_data["title"], filled_story)
    return True


def main():
    """Main entry point."""
    while True:
        success = play_game()

        if not success:
            print(
                f"\n  {Colors.CYAN}Thanks for playing WORKERD MADLIBS!{Colors.RESET}\n"
            )
            break

        try:
            again = (
                input(f"  {Colors.GREEN}Play again? (y/n):{Colors.RESET} ")
                .strip()
                .lower()
            )
        except (EOFError, KeyboardInterrupt):
            print(
                f"\n\n  {Colors.CYAN}Thanks for playing WORKERD MADLIBS!{Colors.RESET}\n"
            )
            break

        if again != "y":
            print(
                f"\n  {Colors.CYAN}Thanks for playing WORKERD MADLIBS!{Colors.RESET}\n"
            )
            break

        print("\n" + "=" * 60 + "\n")


if __name__ == "__main__":
    main()
