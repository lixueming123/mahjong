// Frozen wire enum values for PROTOCOL v2.0 (game/docs/PROTOCOL.md §1.3.1).
//
// The single source of truth is game/protocol/enums.json. This module keeps a checked-in
// typed copy because a browser cannot read a local JSON file without an async fetch; it is
// verified against the JSON (and against the C++ header) by
// game/tests/protocol_enums_consistency_test.py. 0 = INVALID in every domain, real values
// are contiguous 1..N. NEVER renumber: append only and bump the protocol version.

export const DOMAINS = Object.freeze({
  frame_type: Object.freeze({
    INVALID: 0,
    login: 1,
    register: 2,
    ping: 3,
    stats_request: 4,
    list_rooms: 5,
    create_room: 6,
    join_room: 7,
    ready: 8,
    sync: 9,
    discard: 10,
    action: 11,
    login_ok: 12,
    error: 13,
    room_joined: 14,
    game_start: 15,
    draw: 16,
    discard_event: 17,
    action_request: 18,
    action_event: 19,
    state_sync: 20,
    game_end: 21,
    pong: 22,
    stats: 23,
    room_list: 24,
    match_end: 25,
    tenpai: 26,
    room_update: 27,
  }),
  action: Object.freeze({
    INVALID: 0,
    pass: 1,
    pong: 2,
    chow: 3,
    kong: 4,
    ankan: 5,
    kakan: 6,
    win: 7,
    riichi: 8,
  }),
  window: Object.freeze({ INVALID: 0, discard: 1, claim: 2, chankan: 3 }),
  mode: Object.freeze({ INVALID: 0, classic: 1, riichi: 2 }),
  length: Object.freeze({ INVALID: 0, tonpu: 1, hanchan: 2 }),
  meld_type: Object.freeze({
    INVALID: 0,
    pong: 1,
    chow: 2,
    kong: 3,
    ankan: 4,
    kakan: 5,
  }),
  reason: Object.freeze({
    INVALID: 0,
    win_ron: 1,
    win_tsumo: 2,
    draw_exhausted: 3,
  }),
  error_code: Object.freeze({
    INVALID: 0,
    INVALID_NAME: 1,
    NOT_LOGGED_IN: 2,
    ROOM_NOT_FOUND: 3,
    NOT_YOUR_TURN: 4,
    NO_PENDING_ACTION: 5,
    ILLEGAL_ACTION: 6,
    UNKNOWN_TILE: 7,
    BAD_REQUEST: 8,
    FRAME_TOO_LARGE: 9,
    RATE_LIMITED: 10,
    INTERNAL: 11,
    AUTH_FAILED: 12,
    USERNAME_TAKEN: 13,
    ALREADY_ONLINE: 14,
  }),
});

function constants(domain) {
  const out = {};
  for (const [name, value] of Object.entries(DOMAINS[domain])) {
    out[name.toUpperCase()] = value;
  }
  return Object.freeze(out);
}

export const FRAME_TYPE = constants('frame_type');
export const ACTION = constants('action');
export const WINDOW = constants('window');
export const MODE = constants('mode');
export const LENGTH = constants('length');
export const MELD_TYPE = constants('meld_type');
export const REASON = constants('reason');
export const ERROR_CODE = constants('error_code');

export const MAXS = Object.freeze(
  Object.fromEntries(
    Object.entries(DOMAINS).map(([domain, values]) => [
      domain,
      Math.max(...Object.values(values)),
    ]),
  ),
);

export function nameOf(domain, value) {
  const hit = Object.entries(DOMAINS[domain]).find(([, number]) => number === value);
  return hit ? hit[0] : 'INVALID';
}
