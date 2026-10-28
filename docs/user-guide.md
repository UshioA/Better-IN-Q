# Better-IN-Q User Guide

A quick quest-restart tool for Monster Hunter: World Iceborne (15.23.00), shipped as an in-game
plugin: it is loaded with the game, and `Insert` opens the panel.

## Install

1. Install Stracker's Loader first: `dinput8.dll` and `loader.dll` must be in the game folder.
   Mod managers only copy files; without the loader the plugin is never loaded.
2. Merge the `nativePC` folder from the archive into the game folder.
3. Start the game; when you are back at a hub, the chat box shows "Better-IN-Q 0.7.2.1 已就绪".

Uninstall: delete `nativePC\plugins\BetterINQ` (settings live there too).
Do not run the original IN-Q.exe at the same time.

## First launch

- The "ready" message in chat means the plugin loaded.
- If there is an original IN-Q `config.txt` next to the plugin (or one level up) and you have no
  loadouts yet, it is imported automatically once.
- After that, change everything in the panel; changes apply immediately and are saved for you.

## Opening the panel

| Action | Input |
| --- | --- |
| Open / close the panel | `Insert` (keyboard only) |
| Move | D-pad / left stick / arrow keys |
| Confirm | `A` (Enter or Space) |
| Back | `B` (Esc) |
| Enter the sidebar | move left from the content area |

The focus starts on "Restart" — press A to restart right away.
While the panel is open the game receives no keyboard, mouse or gamepad input; the mouse is
free to click the panel.

## Quests

- The top shows the quest that will be restarted, with "Restart" and "Reset only" buttons.
- "Change quest" lists every quest by group. The search box accepts:

| Input | Examples |
| --- | --- |
| Quest name (Chinese / English / Japanese) | 传说中的黑龙 / Fade to Black / 伝説の黒龍 |
| Chinese pinyin and initials | heilong, chuanshuo, hl |
| Japanese kana and romaji | でんせつ, densetsu, kokuryuu |
| Quest ID | 66845 |

## Loadouts

A loadout is equipment set + item set + meal + quest, applied together when restarting.

| Action | Input |
| --- | --- |
| Select / open the editor | `A`, Space, Enter; clicking a row that is not active selects it, clicking the active row edits it |
| Apply directly | `Y` |
| Rename | `F` or gamepad `X` (up to 50 characters, duplicates allowed) |
| Reorder | `L3` or `E` enters move mode, up/down swaps, `L3`/`A`/`E` drops, `B` cancels |

"New loadout" asks for a name first; leave it empty to use the default name.
Inside a loadout, a dropdown set to "—" means "do not change this".

## Meals

A meal stores health, stamina, attack, defence, elemental resistance and three skills.
Controls are the same as on the loadout page. If the active loadout carries its own meal, the
page says so and the rows here can only be edited — the loadout's meal wins.

## Settings

- Language: 中文 / English / 日本語 (defaults to your system language)
- Restart mode: restart directly / reset and re-accept at the hub / accept only, do not depart
- Wingdrake start, shorter transition fades, panel opacity
- Three hotkey rows: restart / open panel / reset only. Select a cell with A or a click, then
  press the key you want; pressing gamepad X or Delete clears the cell
- Save slot: 1 / 2 / 3 (see below)

## Reset only

Same as the original IN-Q "Reset only": resets the current quest back to the hub without
accepting or departing. It has no hotkey by default; use the button on the home page, or record
one on the third hotkey row in Settings.

## Importing from the original IN-Q

Open Settings and click "Import from IN-Q" at the bottom, then pick the `config.txt` of the
original IN-Q. The browser starts in the game folder; ".." goes up one level.

The import replaces everything and brings over loadout names, equipment sets, item sets, radial
link, quest IDs, meals, language and reset mode. Gamepad hotkeys are not imported.

## Save slots

Different Steam accounts on the same PC, or different saves of the same account, keep separate
loadouts and meals:
- the account is detected automatically;
- pick the save slot 1 / 2 / 3 in Settings (do it after switching saves in game).

Switching only swaps loadouts and meals; panel settings, hotkeys and language always follow you.
Clicking the current slot does nothing.

## HDR displays

Panel colours adapt to HDR automatically. If the panel looks too bright or washed out, set
`Hdr` to `off` under `[General]` in `BetterINQ.ini` and restart the game.

## Troubleshooting

| Symptom | Check first |
| --- | --- |
| No "ready" message | is `dinput8.dll` / `loader.dll` in the game folder? |
| Keyboard or mouse still reaches the game while the panel is open | send me the last lines of `BetterINQ.log` |
| The game closes the window but the process stays, audio keeps playing | it ends by itself within ~10 seconds |
| Cannot type Chinese / Japanese in the panel | just type in the field; the IME candidate window follows it |

Log file: `nativePC\plugins\BetterINQ\BetterINQ.log`
