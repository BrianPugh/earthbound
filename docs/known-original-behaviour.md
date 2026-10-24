# Known original behaviour

Things in the C port that look like bugs but match the original game. Check this list before investigating a suspected divergence. Whenever a check concludes "matches the original" (emu session, asm reading or ROM data), add a line here, and a short comment at the code site if one routine explains it.

Columns: what you see; where (save `playtest/saves/NN-*.srm` or relay `run-NN.srm`, position, scene); how it was verified (emu/port sessions under `playtest/out/sessions/`, asm routine, or data); date found. Positions are world pixels `x,y`.

## Rendering and viewport

| What you see | Where | Verified | Date |
|---|---|---|---|
| Battle BG is a narrow horizontal band with black above and below (the port extends the band to 320 px) | ordinary battles: Runaway Dog, Attack Slug, Rowdy Mouse, Black Antoid, Onett cops, Peaceful Rest Valley enemies, Foppy, Urban Zombie, Mini Barf, Cave Boy, UFO, Octobot, Guardian; bosses fill the screen | emu run02o/0002, gso1/0003-0004, ta6o/0002; asm letterbox HDMA (`build_letterbox_hdma_table`) | 2026-10-24 |
| Purple 8-px vertical stripe at world x 1008-1024 | forest west of the Onett→Twoson path, near 1168,3081 | emu run09o/0001 vs port run09 0042 | 2026-10-24 |
| Purple rectangle (right edge x 4352, bottom y≈2056); it is the winding path's sector | north of Peaceful Rest Valley, `teleport 4400 2088` | emu pve/0002 vs port pv1/0004 | 2026-10-24 |
| Flat blue world columns x<256 next to the brick tunnel | Dusty Dunes west tunnel mouth, 330-400,10032 | emu run31o/0001 vs port run31a/0059 | 2026-10-24 |
| Blue rectangles in the top rows and the right gutter | Fourside, 3040,3400 and 3700,3500 (save 16) | emu swe16 sheet-0002, 0005 | 2026-10-24 |
| Green-grey Fourside palette | near the Fourside hospital, 3160,3814 | emu run31o/0003 | 2026-10-24 |
| Teal block in the bottom-left (x<80) of the sea | Summers, 4432,3150 (save 23) | emu swe23 sheet-0002 | 2026-10-24 |
| Off-map columns at the world's west edge are flat blue | Scaraba, 140,5600 then LEFT (save 28) | emu swe28 sheet-0002 | 2026-10-24 |
| Black blocks on the Deep Darkness approach | 4502,7080 (save 28) | emu swe28b sheet-0002 | 2026-10-24 |
| Purple Scaraba sea | Scaraba coast | emu | 2026-10-23 |
| Black area below Ness in the room after a stay (camera sits at the room corner) | Hotel Onett, `teleport 7944 1656 5` from run-03 | emu run04e/0001 vs port run04h/0001 | 2026-10-24 |
| Small interiors sit in black map space, often off-centre (houses, hotel lobbies, tents, cabins, mine rooms, Monotoli floors, Fourside café/bakery) | everywhere | emu run04e; many relay legs | 2026-10-24 |
| Black bottom rows y≥225 | Belch sewer room (save 25) | emu swe25/0002 | 2026-10-24 |
| 5-px rock strip at the left gutter edge is a real rock pillar, black void to its left | Lost Underworld PD2, 5698,336; `flag 11 1; teleport 5620 336 2` on 32-LumineHall | emu gl-luo/0001-0002 vs port gl-lu/0007, 0009 | 2026-10-24 |
| Giygas death static has horizontal bands that jump between bands | 36-CavePast, after the final prayer | emu gl-go/0156-0163 vs port gl-g/0072-0079 | 2026-10-24 |
| Gutter NPCs vanish partway into the battle swirl | any overworld battle start | emu | 2026-10-23 |
| Deep Darkness stays dark until Hawk Eye | Deep Darkness (save 30) | emu | 2026-10-23 |
| Lumine Hall wall text is clipped at band x≈236 | Lumine Hall, hotspot 30 (save 32) | emu teo1 sheet-0003 vs port te1 sheet-0005 | 2026-10-23 |
| Black Antoids undrawn for a moment right after the door | Giant Step rope-room door (save 03) | emu gso1 | 2026-10-24 |
| Palette darkens | Magicant (save 34) | emu p33m | 2026-10-23 |
| Screen quake on entry | Fire Spring, door 1880,3096 (save 33) | emu p33a | 2026-10-23 |

## Battle

| What you see | Where | Verified | Date |
|---|---|---|---|
| Bash does only 40-50 to Mondo Mole | Lilliput Steps (save 09) | ROM enemy data (defense 50) | 2026-10-24 |
| Jeff's Shoot does 1 HP to Master Belch | Belch Base | ROM enemy data (defense 88) | 2026-10-24 |
| Huge enemy hits (Skelpion 118, Mystical Record 207) with no SMAAAASH text | desert, Dungeon Man | `asm/battle/smaaaash.asm` 4×offense−defense; `MSG_BTL4_RESULT_SMAASH_MONSTER` is a special-gfx glyph the TEXT decoder doesn't show | 2026-10-24 |
| Long Ness's Nightmare fight, most Bashes "Just missed!" | Magicant (save 34) | MISS_CALC reads the Casey bat's miss rate; Nightmare runs out of its 882 PP | 2026-10-23 |
| Damage, status and reward distributions (Frank, Mondo Mole heals, Smilin' Sphere group, Carbon Dog, Plague Rat, Thunder and Storm, Hard Crocodile + Zap Eel) | see coverage doc "Battle parity" | statistical emu comparison, bt-p1..8 vs bt-e1..8 | 2026-10-24 |
| Turn-by-turn RNG replay against the emu drifts | any battle | original has CPU-lag frames that skip WINDOW_TICK's RAND; compare distributions instead | 2026-10-24 |
| Battle-end RAND ticks land on one frame in the port, spread over ~30 frames in the original; the total matches | last prompt of a won battle, normal or debug instant win (02-Sharks, Spiteful Crow at 2660,440) | `rng` seeded at the last prompt on both sides: 17450 21161 → 29218 17098 (3 RANDs) on port and emu; instant win 4660 22136 → 24722 9369 on both; sessions bxp1/bxo1 | 2026-10-24 |
| Rolling HP meter lets a member survive a mortal hit | any battle | asm | 2026-10-23 |
| Zap Eel fires 2 bolts | Deep Darkness | asm | 2026-10-23 |
| Lopsided Titanic Ant / Antoid layout | Giant Step (save 03) | asm (random back-row edge) | 2026-10-23 |
| "[enemy] was able to move!" after a solidified member's turn | any battle | asm | 2026-10-23 |
| Failed Teleport α chars the party | anywhere | asm | 2026-10-23 |
| An overworld enemy touching the party during a PSI Teleport α run starts a battle (the teleport fails) | Scaraba desert, run-54 session run54a (Great Crested Booka during the run west along y 4581 from 1863) | asm psi_teleport_alpha_tick.asm:34-40 sets state 2 + BATTLE_MODE on BATTLE_SWIRL_COUNTDOWN; C overworld_teleport.c:447-451 same | 2026-10-24 |
| Spiteful Crow steals an item first, which frees a slot | 01-Start | ROM data (drop rate 7 = always), port log vfa1 | 2026-10-23 |
| Original's battle entry takes longer | every battle | CPU lag; the fade speeds match the asm | 2026-10-23 |
| PSI Healing α "had no visible effect" on a poisoned member; Healing β cures it | overworld, run-48 cabin (Ness poisoned by a Skelpion) | `asm/battle/actions/healing_alpha.asm` only cures COLD/SUNSTROKE (+ temporary group) | 2026-10-24 |
| Window turns red after a fight where the last party member is unconscious | any battle | LOAD_CHARACTER_WINDOW_PALETTE (death flavour palette) | 2026-10-23 |
| Pink text window during Pokey's escape speech | after the final Giygas prayer (save 36) | Poo is down, so LOAD_CHARACTER_WINDOW_PALETTE loads the death palette; emu gl-go/0148 | 2026-10-24 |
| Kraken's "breathed fire" hits all 4 for 180-300 each (Fire γ) and goes straight through PSI Shield Σ; its Crashing Boom Bang also ignores the shield (only the Franklin badge deflects it) | run-51.srm, Summers sailor npc1065 → Kraken battle | action_table.asm FIRE_BREATH = type OTHER + BTLACT_PSI_FIRE_G; PSI_SHIELD_NULLIFY only acts on ACTION_TYPE::PSI; port run52a (242/225/199 with the shield up) | 2026-10-24 |

## Text and windows

| What you see | Where | Verified | Date |
|---|---|---|---|
| "Paula could not use the Broken tube very well." | Paula Uses the Big bottle rocket from her slot 12 (run-32) | emu run32o/0001; USE_ITEM sets argument memory to the inventory *slot*, item 12 is the Broken tube (original bug) | 2026-10-24 |
| "There is a IQ capsule inside!" | present npc1499, 6248,5176 (save 17) | emu run35o/0004 | 2026-10-24 |
| "(There's no response.)" for a failed Talk to | inside the Happy Happy HQ | script (FLG_HAPPY_SYSMSGCHG) | 2026-10-23 |
| Dad's phone window opens ~3 frames before the original's | any phone call | CPU lag (SELECTION_MENU confirm has no frame waits, selection_menu.asm:424-490); letter cadence identical, vff4 vs vffq | 2026-10-23 |
| HP/PP meters roll through unrelated values | Magicant power montage (save 34) | HPPP flipout mode, turned on by the montage text (trigger_event 13 → TOGGLE_HPPP_FLIPOUT_MODE(1)) | 2026-10-23 |
| Everdred's house hidden by the text window | Twoson | TEXT_STANDARD box covers it | 2026-10-23 |
| Phone call keeps the pause menu open | any phone | asm | 2026-10-23 |
| B toggles the HP/PP window | overworld | asm | 2026-10-23 |
| TEXT shows "triedoid" | Giant Step, leg 7 | harness decoder artifact; screen is clean (gs-tried/0001) | 2026-10-24 |

## Overworld and collision

| What you see | Where | Verified | Date |
|---|---|---|---|
| UP+LEFT doesn't move Ness; LEFT does | Giant Step north path, 1976,1024 / 1997,928 / 1926,925 | emu gso1 vs port gs2 | 2026-10-24 |
| Walking diagonally into a wall doesn't slide along it | anywhere | TEST_COLLISION_DIAGONAL | 2026-10-23 |
| Talk to range is \|dy\|≤12/\|dx\|≤15 or \|dy\|≤7/\|dx\|≤25 | anywhere | asm | 2026-10-23 |
| OBJECT NPCs (phones, ATMs, presents) need Check; presents open at dy 0-12, not 16; Ness's sprite covers a box he stands on | anywhere | emu vfbo/0003, 0005 | 2026-10-23 |
| Empty present caps the ladder up to the coffee-break plateau | Saturn Valley, ladder 488,7568, npc1470 at 496,7536 | emu (stuck at 496,7544 too) | 2026-10-24 |
| Ladders stop at their top tile (dead ends) | Saturn Valley, 496,7544 and 304,7312 | emu tco14/0002, tco14b/0002 | 2026-10-23 |
| Door 4232,2040 only works walking DOWN from the pink winding path; UP from the grass below stops at y 2048. The "winding path" is Giygas's lair (music 71 The Place) and the "mist" it leads to is the Cave of the Past (music 70; its door 192,6040 back to the lair is TEXT_DOOR_943, the final-battle door). Neither has a walking link to Grapefruit Falls or Saturn Valley; 904,7360 is Cave of the Past too | lair 4100-4400,<2048, next to the Peaceful Rest Valley grass (reached in save 13 only by teleport) | emu tfo13; door table; music ids | 2026-10-23, corrected 2026-10-24 |
| Cave of the Past walk from the lair door arrival 200,6048 down-left then right into door 416,6360 → 904,7360 takes the same steps in both | save 13, `flag 11 1; teleport 4232 2020 4; 20 DOWN`, then 40 frames each of DOWN, DOWN+LEFT, DOWN ×3, DOWN+RIGHT, RIGHT ×4, DOWN+RIGHT | emu sc3o vs port sc3 (door fires 4 frames into the last step on both) | 2026-10-24 |
| Fence gap south of Ness's yard lets the party through only from x 2624-2654 (2624-2630 slide to 2632); x ≤ 2622 stops at y 383-384, x ≥ 2656 doesn't move | Onett, Ness's yard, walking DOWN from y 360 (save 01) | emu sc1o vs port sc1, 2-px sweep x 2614-2666, identical end positions | 2026-10-24 |
| Lumine Hall floor hole (doors 6440/6448,4808 → fall to the Lost Underworld) only fires when the leader enters its tile row: LEFT along y 4812, UP from x 6448, UP+LEFT from 6480,4840. LEFT along y 4824 (or from y 4800, which slides down to 4824) and UP from x 6436-6440 pass it by; no hotspot visit is needed | Lumine Hall, save 32 | emu sc2o vs port sc2, 6 approaches identical | 2026-10-24 |
| Party walks 0.5 px/frame | Lost Underworld, toward the talking rock (walk style 10) | emu tg2o3 (46 px in 100 frames) | 2026-10-23 |
| Leader sticks after the arrival DOWN steps (4926 → 4927 → stuck on surface 0x91) | Moonside café counter arrival, 6512,4927 (run-40, `flag 145 1; teleport 5523 6592 0; wait 20; press L; until idle 2500 advance; 16 DOWN`) | emu run42o/0001 vs port run42s; SET_LEADER_POSITION_AND_LOAD_PARTY keeps the old sub-pixel fraction | 2026-10-24 |
| A monkey that gave way drifts back across its doorway | Monkey Caves | asm EVENT_43 → EVENT_6_12_ENTRY_2 (random wander 8-24 px every 30-120 frames) | 2026-10-24 |
| 4 HQ wanderers freeze in a pile | Happy Happy HQ, 7770-7776,2344-2360 (save 07) | emu hqo1/0004; SPAWN_NPCS_AT_SECTOR has no occupancy check | 2026-10-23 |
| Geyser keeps erupting while off screen, stays put | Lost Underworld npc1303, 2672,2992 (save 32) | emu WRAM watcher (gy); ME1's IS_ENTITY_ON_SCREEN branch is dead | 2026-10-24 |
| Everdred is fenced in by the crowd (npc953-958) with no walking approach | Fourside, 2676,3888 | emu | 2026-10-24 |
| Diamond only works from behind the desk (4448,4928); from below the desk or the left side it doesn't | Topolla manager's office (run-37) | emu run38o/0003, 0006, 0007 | 2026-10-24 |
| Fly Honey slug still blocks after "you may pass" | Belch Base | emu | 2026-10-23 |
| STATE shows `idle=leader-frozen` after battles while walking works | Winters caves (Jeff + Bubble Monkey) | emu ta11o | 2026-10-23 |
| Enemies spawn only on 64-px boundaries, 24% chance | overworld | asm | 2026-10-23 |
| `ow_enemies` counter wraps | overworld | asm | 2026-10-23 |

## Story, scripts and flags

| What you see | Where | Verified | Date |
|---|---|---|---|
| Hospital "Who do you want to see?" on a conscious (poisoned) member answers "We don't have a person like that in our hospital." and cures nothing | Summers hospital nurse npc1047, run-53 | port run54a; ROM script ESHOP2 nurse branch: no matching ailment → `_NURSE_NOPLAYER` | 2026-10-24 |
| Star Master taking Poo outside the pyramid exit (1728,5514) also fully revives and heals the party | run-54 pyramid exit, walk DOWN from 1728,5472 | port run54a (Paula 1 HP, Jeff unconscious → all full); ROM script `MSG_SKRB_ELDER_WELCOME_POO` calls `MSG_SHOP2_SUB_ALLHEAL_SANCTUARY` | 2026-10-24 |
| Pay phones take $1 when you pick up the receiver | any pay phone | script `MSG_GLOBAL_TELEPHONE_2` (`take_from_wallet: 1`) | 2026-10-24 |
| Game over: only the leader revives, PP is zeroed, money is halved | Continue → Yes | `asm/overworld/spawn.asm:81-83` | 2026-10-24 |
| Hotel stay doesn't cure colds, mushroomization, sunstroke or poison (also the free Dusty Dunes cabin, MSG_DSRT_HOTEL) | any hotel; cabin 8104,1896 (run-48, Ness stayed poisoned) | script (MSG_SHOP2_SUB_HOTEL_HEAL_ALL_PARTY restores HP/PP only) | 2026-10-24 |
| Hot spring prints "washed all of the extremely dirty gunk" but doesn't heal conscious members | Saturn Valley, 508,7352 | emu; the script only heals unconscious members | 2026-10-24 |
| Doctor says "I can only treat an illness" for mushroomization | Twoson hospital | script `_SUB_DOCTOR_1` | 2026-10-24 |
| Front door stays shut after Pokey joins at night | Ness's house (run-01) | emu run02e; flag 467 clears only on Dad's first call | 2026-10-23 |
| Present npc1412 doesn't spawn | Tracy's room, 01-Start | emu (flag 805 already set) | 2026-10-23 |
| Cookie present says "took it" with 14 items | 01-Start | asm | 2026-10-23 |
| Goodbye Exit sign blocks until flag 762 (Yariman B, Yes) | Dungeon Man (save 29) | emu dm1 | 2026-10-24 |
| Password door prints only "@....." | Grapefruit Falls, 6376,264, without flag 131 | script `MSG_EVT2_GPFT_GUARD_PASSWORD` (Mr. Saturn npc739 sets FLG_DOSEI_SECRETCODE) | 2026-10-24 |
| Gold Mine cliff and cave missing; the hole leads to the small dig cave | Dusty Dunes, 3656,9632, without flag 174 | emu (save 17); set by Topolla manager npc839 | 2026-10-24 |
| Monotoli Building door stays shut | Fourside, without flag 143 | script | 2026-10-23 |
| Winters lab: Andonuts says the Sky Runner is ready but it isn't there (npc621/623 hidden by flag 748) | run-49 (Rainy Circle done before Andonuts' "cave north of Stonehenge" talk); lab 7824,2864 | `MSG_WINS_ANDONUT_RAINY_CIRCLE` jumps to `_SKY_RUNNER_READY` when FLG_POWR_RAIN is set, skipping `clear_event_flag FLG_WINS_SKYW_DISAPPEAR`; only reachable after the relay's harness teleport past Bubble Monkey (leg 50 used `flag 748 0`) | 2026-10-24 |
| Paralysis survives the Summers doctor ("I can only treat an illness. Sorry.", $70 not charged), Healing β ("no visible effect") and a hotel stay; only Healing γ cures it | run-50.srm, Summers hospital doctor npc1049 (door 6504,2736 → nurse room, top door 6800,9656 → 7360,9704; talk from 7265,9696 facing UP), L'hotel de Summers | ESHOP2 `_SUB_DOCTOR_1` (`_DCTR_DAIYA` for diamond and paralysis), `MSG_SHOP2_SUB_HOTEL_HEAL_ALL_PARTY` skips both; asm `healing_beta.asm` cures only poison/nausea/crying/strange; port run51a | 2026-10-24 |
| Summers museum charges the $3 admission again after you leave and come back | run-51.srm, museum lobby ticket lady npc1033 | DOOR_SCRIPTS `TEXT_DOOR_647` clears FLG_GLOBAL_MUSEUM_PAID; port run52a | 2026-10-24 |
| Checking the hieroglyph plaque (8040,9560, facing UP) only prints the exhibit description while Poo is unconscious; Poo reads the hieroglyphs (and Omar then hands over the Hieroglyph copy) only when he is conscious | run-51.srm, hieroglyph room | EBGMESS `MSG_SIGN_SUMMERS_HIEROGLYPHS_PYRAMID` (`MSG_SYS_CHECK_CHAR_KO` on Poo → `_READ_SUMS_HIERO_DEAD`); port run52a | 2026-10-24 |
| The Shattered Man vases (npc1039/1040) start their battle when you walk into them, no Talk needed | run-51.srm, hieroglyph room ~8038,9568 | NPC script MSG_BTL9_FMON_HIEROGLYPH_SHATTERED_A/B (touch-triggered field monster); port run52a | 2026-10-24 |

## Audio

| What you see | Where | Verified | Date |
|---|---|---|---|
| Threed keeps the zombie music | Threed, until FLG_WIN_GEPPU | event music table entry 6 | 2026-10-24 |
