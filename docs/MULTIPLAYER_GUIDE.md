# How Perfect Dark Zero multiplayer works

A plain-English tour of what happens under the hood in Perfect Dark Zero's
multiplayer, written from studying the game's own code and data for the
recompiled PC version. Everything here is **Rare's design** unless it is
marked **(ours)**: a change or addition made in this project. Where we're
inferring rather than reading it straight from the code, it says so.

Map names below are the names in the game's map menu. Two of them differ
from Rare's internal level names: **Urban** is `chinatown` and **Old Town**
is `africa`.

## Bases and spawning

**Bases.** Each team-based map has a set of team bases: one per team colour
(red, blue, green, yellow). Most maps have two layouts of them: a "small
game" set close together for small matches and a full set for big ones.
Capture the Flag, Plant the Flag and Protection all use these same base
spots (flags stand on them; Protection's Data Thieves appear on them).

**Spawn rooms and the armoury.** In team games that use spawn rooms, you
don't respawn straight into the map: you appear in a shared room hidden
under the level, the **armoury**, where you can pick up weapons. To get
into the fight you step through its teleporter, which sends you to your
team's own spawn points.

**Hacked teleporters = forward spawns.** Each map with spawn rooms also has
team teleporter consoles out in the level. If your team hacks one, your
armoury teleporter starts sending you to that console instead of your home
spawns: a forward spawn point. Bots do this too: one bot per team at a
time walks to a console and stands at it for ten seconds to hack it
(no minigame for bots). By the code's rules the consoles work in
Killcount, in Team Killcount (unless a match setting turns them off) and in
other modes when team spawn rooms are on.

## Flags

Four flags, one per team colour, which are carried items like weapons. In
Capture the Flag you take an enemy flag home. A dropped flag can be
returned by touching it (the "touch return" option, going by its name).

## The shipped Deathmatch modes

- **Killcount**: everyone for themselves; most kills wins.
- **Team Killcount**: the same in teams.
- **Capture the Flag**: steal an enemy flag and bring it to your base while
  yours is at home. Not offered on Facility or Ruin, which have no flag
  bases.
- **Territorial Gains**: teams fight over zones on the map.

## The shipped DarkOps modes

DarkOps games are played in rounds with money: you earn credits (for kills
and objectives) and spend them on weapons and kit between rounds.

- **Eradication**: team elimination over rounds.
- **Onslaught**: one side attacks, the other defends (with options for how
  the attack is judged).
- **Infection**: players who die join the infected side.
- **Sabotage**: one team defends, the other attacks, swapping each round.
  The defenders' "property" is spread over the map and the attackers earn
  money and points by destroying it:
  - wooden crates and oil drums (weak: 100 health, 100 credits, 1 point);
  - olive ammo caches (500 health, 300 credits, 2 points);
  - comms antennas (3000 health, 2000 credits, 20 points; Tower has
    smaller radio stacks instead: 500 / 500 / 5);
  - power nodes (10,000 health, 4000 credits, 40 points).
  Ammo caches and power nodes can also be destroyed with a **demo kit**:
  a gadget you buy and plant on the target through a short tile puzzle,
  then it counts down and blows. A round ends when time runs out, all the
  property is gone, or one side is wiped out. Each launch map has about
  30-40 pieces of property (Urban and Desert have three power nodes
  each and are worth roughly twice as much). The first map pack's maps
  (Plaza, Rooftops, Trench, Gas Plant) have full sets; the second pack's
  (Villa, Facility, Skedar, Ruin) have none. Rare also recorded announcer
  lines for each kind of target ("fuel dump destroyed", "comms destroyed",
  ...), but the game never plays them.

## The cut modes

Rare built three more modes that never appeared in the menus. Their code
is in the game; we found what was missing and switched them on as an
option **(ours: enabling them, plus the fixes below)**.

**Plant the Flag** (a Deathmatch mode). Instead of stealing the enemy's
flag, you take **your own** flag into an enemy base and hold the button
there for ten seconds to plant it. Rare's code for it was complete
(it even has its own leaderboard and a ranked points limit); it was only
missing from the menu. What we added: a way to pick it, bots that play it
**(ours)**, and random respawns after your first life so planting isn't a
spawn-camping contest **(ours)**. Rare also recorded "planting flag" and
"enemy planting flag" announcer lines that nothing plays yet.

**Protection** (a Deathmatch mode on the six launch maps, which all have
its terminals). Each team has one **hacker** at a time, picked in turn
from the team. The hacker fetches the team's **Data Thief** from the base,
carries it to the one **live terminal** on the map (it moves after every
hack), and hacks it with the Data Thief minigame: press the button as the
cursor passes over the blue blocks; three mistakes in a ring knock you
back a ring. A hack scores a point, a new terminal goes live and every
team gets a new hacker. Killing the enemy hacker resets their attempt.
What was missing or broken, and what we did **(ours)**:
- the hack looped and never finished (a missing "keep the minigame alive"
  mark): fixed;
- the Data Thief has no model you can see in the world or in someone's
  hands (single player never shows it outside the hacking moment): we
  added a map marker and a glowing effect where it lies, and once your
  hacker has picked it up your team sees him on the map with the same icon
  (the enemy team doesn't);
- holding the Data Thief all the time left hackers walking with their
  arms down and unable to climb ladders: the hacker now carries it put
  away (empty hands, normal animations) and it lifts itself when you
  reach the terminal;
- two announcer lines never played ("get to the terminal", "your hacker
  is down", the latter asked for under the wrong name): fixed;
- the game's random numbers were all the same in this port, so the live
  terminal only ever alternated between two: fixed;
- bots never hack (Rare's hacker pick only ever looks at human players);
  experimental bot hackers are being built **(ours)**.

There's a fourth, unlisted DarkOps mode in the code: a round-based version
of Eradication, which looks complete but has no name or menu entry.

**Never built:** announcer lines exist for a "domination" mode, and there
are traces of "challenge" levels and Xbox Live tournaments, but no game
code for them.

## Bots

**Rare's bots.** The launch game had Deathmatch bots only. Rare's title
update added the rest: bots in DarkOps (Eradication, Onslaught and
Infection; never Sabotage), bot personalities chosen per bot (among them
Ghost, Judge, Bully, Venge, Feud and Prey, plus Random), a maximum bot
count of 16 and the bot options page. Bots follow a per-mode script; in Capture the
Flag and Territorial Gains a team "brain" splits them into groups: some
attack or fetch the flag, some guard, some escort their carrier or hunt
the enemy's.

**What we added (ours):**
- Plant the Flag bots, built on the Capture the Flag brain with its goals
  turned round (fetch your own flag, carry it to an enemy base, plant it);
- bots fire the turrets they climb onto: Rare's turret code only ever let
  a human gunner fire, so a bot would sit in a hovercraft's turret doing
  nothing;
- (experimental) Protection bots: bot hackers that fetch the Data Thief,
  go to the live terminal and take Rare's own ten-second bot hack time,
  with teammates escorting them, hunting the enemy hacker and guarding
  the terminal. Since 0.9.2 they spread out: at most three escorts stay with
  the hacker, the terminal's guards stand on their own spots round it,
  and the rest watch the ways in, moving to a new spot every 40 seconds,
  so a team no longer gathers in one crowd;
- Sabotage bots: a bot whose gun gets stuck reloading (in 0.9.1 one could
  replay its reload for a minute at a fuel dump) is given a ready gun
  within about two seconds, then another gun he carries, then his other
  ways to break the target (0.9.2).

## What the maps contain

| Map | Protection terminals | Team teleporter consoles | Sabotage property |
|---|---|---|---|
| Subway | 6 | 4 | 35 |
| Tower | 4 | 2 | 38 |
| Urban | 5 | about 4 | 41 |
| Desert | 4 | 4 | 42 |
| Temple | 4 | 3 | 32 |
| Old Town | 4 | - | 34 |
| Plaza, Rooftops, Trench, Gas Plant | - | - | 34-39 each |
| Villa, Facility, Skedar, Ruin | - | - | none |

Facility and Ruin have no flag bases (so no Capture the Flag or Plant the
Flag there), and the game doesn't offer Sabotage on them either. Two more multiplayer maps, "Bridge" and
"Jungle", appear only by name in the game's stats tables: they were cut
before any map data shipped.

## Credits

Perfect Dark Zero and every system described here are Rare's work. This
guide and the parts marked **(ours)** come from the community recompilation
project; corrections are welcome.
