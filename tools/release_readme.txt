TITANIUM LEGION {version}
A recompilation of Steel Battalion for the original Xbox, running natively on
Windows.
https://github.com/AsukaNeo/titanium-legion

This is a PRE-ALPHA PREVIEW. The first mission plays on a normal pad, but expect
problems. Report them at https://github.com/AsukaNeo/titanium-legion/issues
(describe what happened; never attach game files).

WHAT YOU NEED
  - Your own copy of Steel Battalion for the original Xbox, USA release
    (version 1): its disc image (.iso), or its files (default.xbe and the media
    folder). Nothing from the game is included in this download.
  - Windows 10 or 11, 64-bit, and a graphics card with Direct3D 11.
  - An Xbox-style controller is recommended.

FIRST START
  1. Unzip this folder anywhere you like.
  2. Double-click titanium_legion.exe and choose your copy when it asks.
     It checks that this is the supported release.

EASY PILOT (on by default)
  The game was built for a 40-button controller. Easy Pilot plays it on a normal
  pad, with prompts through the start-up. Turn it off on the title card, in the
  F10 settings or with F7, for the classic layout and no prompts.

  1. Choose Play, then press A or Start at the game's title.
  2. Choose CAMPAIGN, then LIST AS NEW or LISTED PILOT. In menus the left stick
     or d-pad moves, A or Start accepts, X goes back.
  3. New pilot: pick each letter and accept it, go down to COMPLETE, then accept
     the birth date and YES, and pick the mission. The pilot is only saved once
     you pick the mission.
  4. Press Start to skip the intro movie.
  5. In the cockpit, follow the prompt and press Start for each step.
  6. When the five diagnostics bars rise and fall, HOLD Start until the engine
     starts.
  7. Left stick up walks, further for a higher gear. Pull back to brake, and keep
     it back to reverse.
  8. Right stick aims, RT fires, LT locks on.

  Hold Tab or press RB+Back for a card of every control.

KNOWN PROBLEMS
  - The second start-up in one run can get stuck at the Start step. Close the
    game and load your pilot again.
  - The continue menu doesn't draw correctly.
  - The frame rate can drop to the low 20s.
  - With all 8 pilot slots full, or a blank pilot name, the screen dims and
    nothing says why.

SETTINGS AND SAVES
  - F10: settings, with Exit to Windows. F11 or Alt+Enter: windowed / full
    screen. F12: screenshot.
  - titaniumlegion.ini, next to the exe, holds the settings. If aiming feels too
    slow or too fast, change aim_sensitivity under [controls], from 10 to 100.
  - Saved pilots are kept in the "saves" folder here.

LEGAL
  Titanium Legion is MIT-licensed open source (see the licenses folder). It is
  an independent fan project, not affiliated with the rights holders of Steel
  Battalion, or with Microsoft. It contains no game data and works only with your
  own copy. Parts of the xboxrecomp toolkit compiled into it are LGPL-2.1 (from
  xemu); their source is available at https://github.com/sp00nznet/xboxrecomp
  and this project's patch at https://github.com/AsukaNeo/titanium-legion.
