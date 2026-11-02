# Button positions: the face buttons checked by the user

User decisions 2026-11-02 (Retroid Pocket 5: its system settings switch the face buttons between two
layouts, and the hints then showed the wrong position).

## The problem

The app binds and shows the face buttons by position (south / east / west / north; the hint glyph is
four circles, the pressed one filled). The position comes from SDL, which takes the device's word for
it. Devices that swap their face buttons in firmware (the Retroid's layout setting) or report them
another way make every hint and the confirm / cancel buttons point at the wrong button. A table of
devices cannot keep up with Android's variety.

## Adopted

- A check the user does once per controller model: two pictures side by side, each the four face
  buttons as four circles. Pressing a face button fills a circle in both: on the left where the
  controller says it is, on the right with the two pairs swapped (south <-> east, west <-> north).
  The user picks the side whose filled circle is where the pressed button is: left / right on the
  D-pad or a stick, or a tap on that side; no confirm button (it is the thing being checked). No
  brand names (Xbox, Nintendo ...) appear.
- Shown for every new controller model whose layout is not certain (SDL's type standard / unknown:
  not a known Xbox, PlayStation, Switch controller, not Steam's virtual pads, not a Steam Deck), when
  no game is playing (the library, or a paused game). Settings > Controls > ABXY Buttons > ABXY Button
  Positions checks the connected controllers again (user 2026-11-02: not under Controller, so Reset
  Controls never changes it; the page also has Confirm Button, moved there).
- Remembered per model (SDL GUID + name) in <config>/face-layouts.txt. The choice is applied where a
  button becomes a position (InputRouter::padButton, the printed labels): hints, game bindings and the
  UI's confirm / cancel all follow the physical buttons.

## Not adopted

- Reading a device's own setting (the Retroid's `button_set`): one device; the check covers all.
- Asking which brand the controller is: users do not know which layout a brand means; the picture is
  what they see.

## Done when

- Unit tests: the swap of a pad's positions; the file round trip; which controllers are asked.
- On the Retroid Pocket 5 in both of its layouts: the check, then the hints match the buttons.
