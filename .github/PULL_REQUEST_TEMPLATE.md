# What

<!-- One or two sentences. What does this change do? -->

## Why

<!-- The reason, not the restatement. "Buttons double-fired" beats "edit Buttons.cpp". -->

## Hardware verification

CI proves it compiles. It cannot prove it works on the board, so say what you
actually observed:

- [ ] Flashed to a badge and the affected thing behaves correctly
- [ ] Not flashed yet (say why, and what could break)

<!-- If this touches a peripheral, note what you saw: screen renders upright,
     accel values move the right direction, button names map to the right keys,
     LEDs stay dim. -->

## HAL guide

- [ ] Any pin or register value I added matches the HAL guide, and I cited the
      section in a comment
- [ ] No new values are hardcoded outside `firmware/include/badge_pins.h`

## Risk

<!-- Anything that could brown out the board, wedge the I2C bus, allocate a
     large buffer, or block the main loop? Say so here. -->
