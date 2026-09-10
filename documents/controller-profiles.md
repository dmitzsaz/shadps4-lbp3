# Controller/profile menu

One `Input::Profiles` layer handles additional SDL gamepads, the Guide (PS/Xbox)
button, F2, and the existing change-controller event. macOS may intercept Guide;
F2 is the fallback.

The connected-device list always shows real names and their current profiles.
Free player profiles are labeled separately; they are not additional controllers.
Press any button on another pad (or click its name) to select that physical pad.
The first press selects it without also confirming a profile. Up/down chooses a
profile, X/A confirms and returns to the game, Circle/B or Escape closes. Guide
on a different pad selects it; Guide on the selected pad closes the menu.

Assigning an occupied profile swaps the two physical controllers. Assignments
last for the current session. The four configured profiles and their saves are
not renamed, moved or rewritten. An additional pad initially uses the first free
configured profile and opens the same menu to review/change it, including when
it reconnects into the first slot while another pad is still connected.

The SDL thread owns controller and user-manager mutations. The render thread
only draws a locked snapshot and posts commands to SDL. Stable logical
`GameController` objects preserve guest pad handles; physical SDL pointers are
reassigned. Disconnected handles are excluded. Two devices of the same model
remain separate, with numbered names; model/name matching never merges devices.
The existing input capture neutralizes guest pads while the menu is open.
Held input and state queues are cleared on opening, assigning, closing or unplug.

The ImGui SDL backend owns its own opened SDL references. Every list refresh
closes these references before reopening the current set and frees the ID list.
Borrowing gameplay pointers here previously accumulated duplicates on refresh
and let backend cleanup close handles still owned by gameplay.

Regression tests (requires a completed macOS build):

```sh
python3 scripts/test_profile_menu.py --build Build/x64-Clang-Release \
  --output /tmp/shadps4-profile-tests
```

Uses production controller/menu/user-manager objects, the real SDL backend,
real SDL virtual pads and the application's font stack. Outputs a rendered PNG
and checks 1–5 devices, physical sender ownership, Guide/F2, occupied/empty
assignments, guest handle/user-ID stability, neutral queues, unplugged IDs,
12 reconnections into the first slot, capture balance and SDL reference lifetime.
`--backend-source` can compile an earlier SDL backend to reproduce the ownership
regression. Persistence, clock, logging, layer registration and binding reset
use in-memory adapters. Tests never read live user settings and do not validate
physical Bluetooth hardware or Guide delivery by macOS.
