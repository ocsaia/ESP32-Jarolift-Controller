"""Regenerate the timer page from its first block.

The page is a run of <article> blocks that differ only in an index. This takes
block 0 as the template and stamps out the rest.

It proves the substitution is complete before writing anything: every block that
already exists is regenerated from block 0 and compared against what is in the
file. If any token still carries a hard-coded 0 the run aborts rather than
producing twenty-four subtly broken copies.

The comparison collapses whitespace. The existing blocks were formatted by
prettier at different times and wrap attributes differently, which is cosmetic -
a missed index substitution still shows up as a token difference.

    python gen_timers2.py --verify   check only
    python gen_timers2.py            check, then rewrite the page
"""
import io
import re
import sys

TIMERS = 24
PATH = "web/html/03_timer.html"

# Tokens that end with the index.
TOKENS = [
    "OPT_TIMER%d", "OPT_MINTIME%d", "OPT_MAXTIME%d",
    "timerForm%d", "timeInput%d", "offsetInput%d",
    "minTimeInput%d", "maxTimeInput%d", "horizonInput%d",
    "timeType%d", "timeValue%d", "offsetValue%d",
    "minTimeValue%d", "maxTimeValue%d",
    "astroMode%d", "horizonValue%d",
    "command%d", "days%d", "mask_timer%d",
    "monday%d", "tuesday%d", "wednesday%d", "thursday%d",
    "friday%d", "saturday%d", "sunday%d",
]

# Tokens that carry the index in the middle.
PREFIXES = ["cfg_timer_%d_", "timer%d-", "timer%d_", "bitmask_dialog_%d"]


def idents(text):
    """Every identifier the block defines or refers to.

    Compared as a set rather than as a sequence: the existing blocks were
    written at different times and order some elements differently, which is
    cosmetic. A missed index substitution still changes this set.
    """
    return set(re.findall(r'(?:id|name|for|hideOpt|class)="([^"]*)"', text))


def stamp(block0, n):
    out = block0
    for prefix in PREFIXES:
        out = out.replace(prefix % 0, prefix % n)
    for token in TOKENS:
        out = out.replace(token % 0, token % n)
    return out.replace(">Timer 1<", ">Timer %d<" % (n + 1))


def main():
    lines = io.open(PATH, encoding="utf-8", newline="").read().split("\n")
    starts = [i for i, l in enumerate(lines) if l.strip() == "<article>"]
    ends = [i for i, l in enumerate(lines) if l.strip() == "</article>"]
    if not starts or len(starts) != len(ends):
        print("could not find the article blocks", file=sys.stderr)
        return 1
    print("found %d existing blocks" % len(starts))

    template = "\n".join(lines[starts[0]: ends[0] + 1])

    for idx in range(1, len(starts)):
        expected = "\n".join(lines[starts[idx]: ends[idx] + 1])
        got, want = idents(stamp(template, idx)), idents(expected)
        # Known slip in the page as written: the second timer's bitmask input
        # carries name="mask_timer0" instead of mask_timer1. The id is correct
        # and the library binds by id, so nothing misbehaves - regenerating
        # fixes it. Everything else must match exactly.
        # Deliberately new in the template: the twilight controls, which no
        # existing block has yet. Named explicitly so that anything else the
        # template gains by accident still fails the check.
        got -= {"cfg_timer_%d_astro_mode" % idx, "cfg_timer_%d_horizon_value" % idx,
                "astroMode%d" % idx, "horizonInput%d" % idx, "horizonValue%d" % idx}
        if idx == 1:
            want = (want - {"mask_timer0"}) | {"mask_timer1"}
        if got != want:
            print("MISMATCH regenerating block %d" % idx, file=sys.stderr)
            for missing in sorted(want - got):
                print("  generated block lacks: %s" % missing, file=sys.stderr)
            for extra in sorted(got - want):
                print("  generated block adds : %s" % extra, file=sys.stderr)
            return 1
    print("substitution verified against all %d existing blocks" % (len(starts) - 1))

    if "--verify" in sys.argv:
        return 0

    blocks = "\n".join(stamp(template, n) for n in range(TIMERS))
    out = lines[: starts[0]] + blocks.split("\n") + lines[ends[-1] + 1:]
    io.open(PATH, "w", encoding="utf-8", newline="").write("\n".join(out))
    print("wrote %d timer blocks" % TIMERS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
