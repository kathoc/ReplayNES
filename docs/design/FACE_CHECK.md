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

- A check the user does once per controller model (user 2026-11-02, after the first version): the four
  face buttons as four circles in a diamond, one lit - first south, then east. The user holds the
  button at the lit position for a second (a ring fills meanwhile); a shorter press does not count, so a
  stray press answers nothing. From the two buttons SDL reported: south then east as reported -> where
  they are; east then south -> swapped (west <-> north with them); anything else -> "They did not match",
  from the first again. No choosing between pictures, no confirm button (it is the thing being
  checked), no brand names. Each press and the answer are logged ("input: face check: ...").
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
- The first version (2026-11-02): two pictures, a press filled a circle in both (left as reported,
  right swapped), the user chose a side with left / right or a tap. On the Retroid the user ended up
  with the reverse layout twice; which side means what had to be reasoned out, and a choice could not be
  told from a slip. Holding the button at a lit position needs no reasoning.

## Done when

- Unit tests: the swap of a pad's positions; the file round trip; which controllers are asked.
- On the Retroid Pocket 5 in both of its layouts: the check, then the hints match the buttons.

## Defect 2026-11-02: the menus did not follow the check

Symptom (Retroid Pocket 5, checked as swapped, Confirm Button on the east): the button at the east
position cancelled in the menus while the hint showed the east circle as confirm.

Every reader of a gamepad button, from App::handleEvent on, and which number it used:

| Reader | Purpose | Before |
|---|---|---|
| App::handleFaceCheck | the check | SDL's (it must be) |
| InputRouter::handleEvent -> padButton | game bindings, hotkeys, chords, the settings diagram's pressed state | the position (the swap was inside padButton) |
| PadNavFeed (pad_nav.h) | every menu: confirm / cancel, X / Y actions, held buttons | SDL's |
| UI::textEntryEvent -> oskButton (ui_osk.cpp) | the on-screen keyboard's A / B | SDL's |
| player order | which controller pressed (no meaning) | SDL's |

What is drawn: the hint bar's position glyph (UI::faceGlyph) fills the position the Confirm Button
setting names; the settings diagram's letters come from PadInfo::labels, moved to the positions by
describe(). Both are positions. So on a swapped controller the game and the pictures had the buttons
where they are, the menus and the on-screen keyboard where SDL said - the swap was applied inside one
reader instead of where the events come in.

Fix: App::handleEvent moves a face button to its position once (InputRouter::atFacePosition), right
after the check (which keeps SDL's numbers) and before any other reader; padButton no longer swaps
(it would swap back). Nothing else reads gamepad buttons: no SDL_GetGamepadButton calls, no event
watches or filters, ImGui's SDL3 backend has no gamepads (manual mode), the scripts' events go through
App::handleEvent too.
