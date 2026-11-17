# Recomp Achievements lists

JSON for the RecompLauncher hub (PC recomps → Achievements). Upload these files in the hub Achievements tab. The port talks to the launcher over `RL_SESSION` and **fires the `event` names from `src/runtime/ach.c` while you play**. Without that code, the lists would never unlock.

| File | Game | Catalog id |
|---|---|---|
| `fe1.json` | Fear Effect | `project-helix` |
| `fe2.json` | Fear Effect 2: Retro Helix | `project-helix` |

Titles, descriptions and types follow the public RetroAchievements sets (Fear Effect [11380](https://retroachievements.org/game/11380) by AntonioFNN, Fear Effect 2 [11381](https://retroachievements.org/game/11381) by lagoooona). Points are remapped to Recomp's 5 / 10 / 25 / 50 scale.

## Why these are events lists, not RA memory conditions

The public RetroAchievements API only returns an MD5 of each trigger. The real `MemAddr` strings come from the Connect patch API, which is for logged-in emulator clients. RA's Connect entry point states that achievement trigger logic belongs to the RA community and **must not be re-hosted or redistributed**. Putting those strings in a RecompLauncher-hosted list would do exactly that, so they are not in this repository.

The port still maps PS1 RAM the same way DuckStation does (`0x000000`–`0x1FFFFF` main RAM, `0x200000` scratchpad). Conditions in `ach.c` come from **this port's own RAM map** (options/stats next to `gameopt`, character item/weapon records, overlay ident, HUD text, actor HP), checked against keep-state dumps (`I_stats`, `I_virus`, `Ibad`, `A_tapeB`, `d4_choice`).

## What `ach.c` fires

- **Disc / chapter progression** from SWAP (FE1 module 9 / FE2 SWAP overlay) and from LOC overlay changes. Hard-mode siblings fire when the in-game difficulty byte is 1.
- **FE1 CD4 / endings** when disc-4 `LOCF`/`LOCG` returns to `GSHELL` (Hana vs Glas vs Hard).
- **FE2 endings** from the good-route word (`0x80082B04`), bad-route word (`0x8007B93C`), and CREDITS with neither flag (`dead_bodies_everywhere`). Helium Effect 2 is the game's own big-head bit (`0x8007B4F8` bit 7).
- **Speed / saves**: in-game time is options+8 at 30 Hz (FE1 `0x8007780C`, FE2 `0x8007B91C`). Telephobia is FE2 phone-save count `0x8007B924` ≤ 8 at CREDITS. The 2h30 speedrun uses the same clock.
- **Items / weapons** on a rising edge of the live item bytes (FE1 Hana `0x8008DF64`, FE2 Code Master + 0x438) and weapon owned flags (`0x8008BF50 + 0x100*who + 0x18 + 8k`).
- **Stealth / melee counts** from FE2 kill / sneak totals (`0x8007B928` / `0x8007B92C`) on overlay leave; FE1 uses the red target-icon bit (`0x80082C42 & 0x200`) and actor HP (`+0x68`) for “perfect” / damageless siblings.
- **FE2 art gallery** (`Purveyor of Fine Art I–IV`) when extras gallery is actually on screen, for the inserted disc.
- **HUD / prompt strings** from the game's text routine (Po Mon, Stone Sword, Videotape, Steam, Planner, …).

Some missables are still **helix-native proxies** (overlay + HP/detection/HUD) rather than a full per-room script: timed puzzles that only exist inside overlay minigames, “three bullets,” and “knife only” loadouts are not encoded as ammo/weapon restrictions. `FE_DBG_ACH=1` logs overlay / room / HUD text.

Headless check: `python src/tools/ach_e2e.py` (needs the built `fe1port` / `fe2port` and the fe_port discs). It fires every catalog event through `--agent` **and** proves each slug from RAM / HUD / overlay pokes, not only `ach_fire`.

## Ranked

`game/cheat_health` and `game/cheat_ammo` are voids. Infinite health/ammo, GameShark codes, and save-state loads send `casual`. Loading a memory-card save sends `save_loaded`.
