---
name: Hardware bug
about: A peripheral behaves wrong on the board
title: '[hw] '
labels: hardware
---

## What happens

<!-- What the board does. -->

## What should happen

## Which peripheral

<!-- screen / buttons / LEDs / accel / NFC / power -->

## Evidence from the console

Type `state` on the serial monitor and paste the output. For a button problem
the `raw=0x..` byte is the useful part: it shows whether the shift register is
changing at all, which separates a wiring fault from a decode bug.

```
<paste here>
```

## Already ruled out

- [ ] Reflashed and reproduced
- [ ] Checked the value against the HAL guide section for this peripheral
- [ ] On USB power, not just batteries (rules out brown-out)
