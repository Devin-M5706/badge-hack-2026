# Demo script

Fill this in **early**, not at hour 7. Writing down what the demo has to show
is the cheapest way to find out which features are actually load-bearing and
which ones you can cut when time runs short.

## The 30-second version

<!-- If a judge only watches 30 seconds, what has to happen? Write it as a
     sequence of things they SEE, not features you built. -->

1.
2.
3.

## What a judge sees in the first 5 seconds

<!-- Hardware demos live or die on instant legibility. If someone has to be
     told what they are looking at, the demo is too slow. What reads
     immediately from across a table? -->

## Run of show

| Time | Who | What | Fallback if it breaks |
|---|---|---|---|
| 0:00 | | | |
| 0:20 | | | |
| 0:45 | | | |

## Fallbacks

Hardware fails at demo time. Decide now, while you are calm:

- **Battery browns out:** run on USB power. Keep a cable on the table.
- **Screen comes up mirrored:** `rot 3` on the serial console.
- **A button stops responding:** inject it over serial with `btn <name>`.
- **Board will not flash:** hold Start while plugging in USB (download mode).
- **Everything is broken:** have a recorded video. Record it at hour 5, while
  things still work, not at hour 7 when they do not.

## Pre-demo checklist

- [ ] Fresh batteries in, and a USB cable as backup
- [ ] `main` builds green in CI
- [ ] Flashed from a clean checkout of `main`, not from a dirty working tree
- [ ] Demo run end to end at least twice without touching the code
- [ ] Video recorded as a fallback
- [ ] Serial monitor closed (it holds the port)
