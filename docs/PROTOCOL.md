# Protocol notes (core API v11)

`OCG_DuelGetMessage` yields a sequence of little-endian frames: `u32 message_length`, followed by that many message bytes. The first byte in each payload is the `MSG_*` kind from `ocgapi_constants.h`.

Phase 1 decodes public lifecycle messages: `MSG_NEW_TURN`, `MSG_NEW_PHASE`, `MSG_DRAW`, `MSG_SUMMONED`, `MSG_ATTACK`, `MSG_DAMAGE`, and `MSG_WIN`. A win frame contains the winning player followed by the win reason.

Decision frames are engine-generated legal-action menus. The implemented smoke agent supports:

- `MSG_SELECT_IDLECMD`: player byte; command lists in this order: summon, special summon, reposition, monster set, spell/trap set, activate; then battle/end/shuffle booleans. It responds with the packed `u32` `(index << 16) | command_type` used by `field::process(SelectIdleCmd)`.
- `MSG_SELECT_BATTLECMD`: player byte; activatable-effect list; attackable-monster list; Main 2 and End Phase booleans. It uses the same packed response format.

The current bridge also encodes engine-offered monster-zone, battle-position, card-selection, and tribute-selection responses. For a human-controlled prompt, it derives a card menu directly from the engine frame; multi-card selections are emitted as bounded combinations, with the selection count stored in the response payload. It deliberately leaves less common response types—yes/no, option, chain selection, counters, sums, sorting, and announcements—to future typed protocol support. Every response remains an answer to a legal engine request, never a client-side rules calculation.
