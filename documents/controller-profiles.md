# Controller/profile menu

One `Input::Profiles` layer handles all entry points: an additional SDL gamepad
connecting, the Guide (PS/Xbox) button, F2, and the existing change-controller event.
macOS may intercept Guide; F2 is the fallback.

Choose a controller with left/right, a configured player profile with up/down,
and assign with X/A. Circle/B or Escape closes the menu. Mouse selection is also
supported. Assigning an occupied profile swaps the two physical controllers.
Assignments last for the current session. The four configured profiles and their
saves are not renamed, moved or rewritten. Connecting an additional pad initially
uses the first free configured profile and opens the menu to review/change it.

The SDL thread owns controller and user-manager mutations. The render thread
only draws a locked snapshot and posts commands to SDL. Both hotplug and Guide
call the same `Open` function. Stable logical `GameController` objects preserve
existing guest pad handles; only physical SDL device pointers are reassigned.
The existing input-capture mechanism neutralizes guest pads while the menu is
open. Opening, assigning and closing clear held input and queued states.

Regression test (requires a completed macOS build):

```sh
python3 scripts/test_profile_menu.py --build Build/x64-Clang-Release \
  --output /tmp/shadps4-profile-tests
```

The test links production controller, menu and user-manager objects with real SDL
virtual pads, and draws the menu with Dear ImGui. It covers 1–5 devices, Guide/F2,
occupied and empty assignments, stable guest handles/user IDs, stale/unplugged
IDs, neutral state queues, and balanced capture. Persistence, the guest clock,
logging, layer registration and binding reset use in-memory adapters; it does
not read live user settings or validate physical Guide delivery by macOS.
