# Contributing

Short on purpose. These are the rules that prevent real problems; everything
else is taste.

## Setup

```bash
scripts/setup.ps1     # Windows   (or: scripts/setup.sh)
```

That points git at `.githooks`, installs the commit template, and tells you
whether PlatformIO is present. Safe to re-run.

## The three rules

**1. Pin numbers and register values live in one place.**
GPIOs go in `firmware/include/badge_pins.h`. Peripheral register values go in
the `.cpp` of the class that owns them, as named constants with the HAL guide
section cited. Never inline a magic number at a call site — a GPIO that appears
twice will eventually disagree with itself.

**2. Logic that can be tested without hardware goes in `lib/ButtonLogic`.**
No `#include <Arduino.h>` in there, ever. If you find yourself needing one, the
logic belongs in `lib/Badge` instead. The native test env will catch this for
you at link time.

**3. Cite the guide when you transcribe from it.**
`// §5: counts = raw >> 4, 1 count = 1 mg at +/-2 g` turns a mysterious shift
into a checkable claim. The next person to read it — including you, tomorrow —
can verify it without re-reading the whole document.

## Commits

[Conventional Commits](https://www.conventionalcommits.org/), enforced by
`.githooks/commit-msg`:

```
<type>(<scope>): <subject>

feat(display): add row-clearing text output
fix(buttons): prime debouncer so held Aux1 is not a phantom press
docs(hardware): note that only one process can own the serial port
```

Types: `feat fix docs style refactor perf test build ci chore revert`
Scopes in use: `buttons leds display accel console ci docs hal`

Write the subject in the imperative, and use the body for *why*. In three weeks
`git log` is the only record of your reasoning that still exists.

Bypass a hook knowingly with `--no-verify`. It is there for emergencies, not
for hour 6.

## Before you push

```bash
cd firmware
pio run -e badge        # must compile
pio test -e native      # must pass
```

CI runs both. Formatting is checked but never blocks a merge — a red X over
brace placement at hour 6 only teaches people to ignore CI.

## Tests

Write them for logic that is easy to get silently wrong: bit order, sign
handling, state machines, anything with a boot-time edge case. Do not write
them for things only hardware can answer — nobody can unit-test whether the
screen is upright. That is what the bring-up sketch and the serial console are
for.

## Before a PR

Fill in the hardware verification section of the PR template honestly. "Not
flashed yet" is a fine answer; a silently untested peripheral change is not.
CI proves the code compiles, which is not the same as proving it works.
