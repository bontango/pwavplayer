# Sound Groups

A **sound group** bundles several WAV files under a single ID. When the group ID
is triggered (via GPIO event, serial command, or HTTP `/play`), the player picks
**one** member and plays it — either randomly or in round-robin order.

This lets you map one pinball event (e.g. "exit lane left") to a pool of sound
variants without duplicating logic in your event decoder.

## Creating a group

A group is defined purely by the **filename** of an empty `.grp` file placed in
the active sound-theme directory (`<sdroot>/<stheme>/`, default
`<sdroot>/orgsnd/`) next to your `.wav` files. The file content is not read —
everything the player needs is encoded in the name.

### Filename pattern

```
<ID>-<attr>-<mem1>-<mem2>-...-<memN>-<freetext>.grp
```

Example:

```
0009-m-15-71-12-exit-lane-left.grp
```

| Field             | Meaning                                                            |
|-------------------|--------------------------------------------------------------------|
| `0009`            | Group ID — a 4-digit number, triggered just like a sound ID        |
| `m`               | Attribute — play mode (see below)                                  |
| `15-71-12`        | Member sound IDs (must refer to existing WAVs on the card)         |
| `exit-lane-left`  | Free text / description — ignored by the parser                    |

Rules:

- The **ID** must be exactly 4 digits.
- The **attribute** must be a single character.
- Members are read left-to-right, separated by `-`, and must be plain integers.
- Parsing stops at the first non-numeric token — everything after becomes the
  description and is ignored.
- The file itself can be empty; only the name matters.
- Extension must be `.grp` (lowercase).

### Play-mode attributes

| `attr` | Behaviour                                                                   |
|--------|-----------------------------------------------------------------------------|
| `m`    | **Random** — a random member is chosen on every trigger                     |
| `r`    | **Round-robin** — members are played in order, cycling back to the first    |

Any other character makes the group invalid — triggering it produces no sound.

### Examples

```
0009-m-15-71-12-exit-lane-left.grp     random pick from sounds 15, 71, 12
0020-r-40-41-42-43-bonus-count.grp     cycles 40 → 41 → 42 → 43 → 40 → ...
0030-m-100-101-knocker.grp             random pick between two knocker samples
```

## Triggering a group

Trigger the group ID the same way you trigger an individual sound. The player
first looks for a WAV with that ID; if none is found, it looks for a group with
that ID. So **sound IDs and group IDs share the same namespace** — do not reuse
an ID that is already used by a `.wav` file.

## Implicit groups (startup & background music)

Two groups are created automatically from the attribute flags of your WAV
filenames (format: `<id>-<flags>-<vol>-<name>.wav`). The 4 flag characters are
position-independent. Both implicit groups use random-pick mode:

- **Startup sounds** — every WAV whose flags contain `i` is added to a hidden
  "startup" group. One random entry is played at boot.
- **Background music** — every WAV whose flags contain **both** `l` and `i` is
  added to a hidden "background" group. One random entry is started as looping
  background music.

You do not need to create a `.grp` file for these — the flags in the WAV name
are enough.

## Limits and caveats

- **Maximum 255 members** per group.
- Total filename length is bounded by the SD card's long-filename limit, which
  in practice caps group size well below 255.
- Only one member is played per trigger — a group never plays all its sounds at
  once. For simultaneous playback, issue multiple triggers.
- All member IDs must reference `.wav` files that actually exist on the card;
  missing IDs are silently skipped at lookup time.
- Listing member IDs inside the `.grp` file's contents is **not** supported —
  they must be in the filename.
