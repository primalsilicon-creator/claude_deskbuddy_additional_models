# Hardware Buddy BLE protocol: what the desktop actually sends

This is a field-observed companion to [`REFERENCE.md`](../REFERENCE.md). It is
written from an Apple PacketLogger capture of Claude for macOS talking to a
Buddy board, not from the desktop source, so it describes behaviour seen on
the wire. Where it disagrees with `REFERENCE.md`, the section
[Differences from REFERENCE.md](#differences-from-referencemd) says so.

Transcript text below is shortened, and file paths, tool-use ids and
thinking signatures are redacted.

The desktop app's own help page ("Connect maker devices to Claude") gives a
shorter version of `REFERENCE.md` with the same UUIDs, heartbeat, `prompt`,
permission reply and 4 KB turn-event limit. It says the bridge serves both
**Claude Cowork and Claude Code**. This capture covers only a Code-tab
session, so whether Cowork sessions relay `prompt` is still untested.

## The capture

| | |
| --- | --- |
| Host | Mac (Apple Silicon), macOS 27, Claude desktop app, **Code tab**, Ask-before-edits mode |
| Device | Buddy firmware advertising `Claude-4796` |
| Window | 2026-11-04 15:49:20 to 15:50:30 UTC (70 s) |
| Started | Mid-connection: connect, MTU exchange, GATT discovery and the one-shot messages are not in it |
| Traffic | 107 ATT writes from the desktop, reassembling into **65 JSON lines** (47 heartbeats, 18 turn events). **Zero** notifications from the board. |
| Activity | One Claude Code session ran two turns, including two `Bash` tool calls that needed approval. The user approved the first one in the desktop UI. |

Decode it yourself with [`tools/pklg_buddy.py`](../tools/pklg_buddy.py):

```
python3 tools/pklg_buddy.py capture.pklg
```

Other BLE peripherals in the capture (mouse, keyboard, watch) use other
connection handles. The script drops any stream that never carries a JSON
line.

## Transport as observed

- Every desktop-to-board packet is an **ATT Write Command** (opcode `0x52`,
  write without response) to the NUS RX characteristic (ATT handle 45 in
  this capture). The desktop never used Write Request.
- Writes are at most **180 bytes** of value, so the negotiated ATT MTU was at
  least 183. A line longer than that spans several writes back to back, with
  no gap between them in the log.
- **Each JSON line starts in a new write.** In all 65 lines, the first write
  began with `{` and the last ended with `\n`. The desktop never packed two
  lines into one write. Firmware should still not rely on this.
- Line framing is `\n` only, with no `\r`. All 65 lines parsed as JSON.
- Longest line seen: 1,086 bytes, an assistant turn event that carried a
  thinking signature.

## Message 1: heartbeat (desktop to board)

```json
{"total":243,"running":2,"waiting":1,"msg":"Sending the prompt now.",
 "entries":["Sending the prompt now."],"tokens":391345,"tokens_today":299237}
```

All 47 heartbeats had exactly these seven keys, always in this order:

| Field | Type | Observed behaviour |
| --- | --- | --- |
| `total` | int | 243 the whole time. This counts every session the app knows about, including old and finished ones, so it is not "open sessions". |
| `running` | int | 1 while idle, 2 while the session was generating. The baseline of 1 points to another session (likely stale) counted as running. |
| `waiting` | int | 1 while idle. It went to 2 the moment a `Bash` tool call was emitted, and back to 1 when that call was approved on the desktop. The baseline 1 is a stale prompt from an older session. |
| `msg` | string | Always equals the last element of `entries`. Cut at about 88 characters. |
| `entries` | string[] | The current message split on newlines, **oldest first**, 1 to 7 items, each cut at about 88 characters. See below. |
| `tokens` | int | Changes only when a turn finishes (391,345 to 392,212), not while streaming. |
| `tokens_today` | int | Changes together with `tokens`. |
| `prompt` | object | **Never present**, including while `waiting` was raised for a tool call that needed approval. |

### What goes in `entries` and `msg`

`entries` is not a running log of timestamped lines. It follows the
session's latest event:

| Session state | `msg` / last entry |
| --- | --- |
| Assistant is writing text | Lines of that text block, one entry per line |
| Assistant is thinking, or between blocks | `(no messages)` |
| Assistant called a tool | `(called <tool name>)`, for example `(called Bash)` |
| Turn finished | `done (success), N turns`, appended after the final text lines |
| User sent a message | The previous final line, then the user's text |

When a new assistant block starts, the list is replaced, not appended to.
Firmware that keeps its own history (as `src/data.h` does) has to merge and
de-duplicate.

### Timing

- **Keepalive every 10 s** when nothing changes. Gaps of 10.01 s, 10.02 s,
  10.18 s and 10.25 s were seen while idle or waiting on approval. The longest
  gap in the capture was 10.25 s.
- **Change-driven updates are fast**: 100 to 400 ms apart while a turn
  streams, which is about 5 per second.
- The same heartbeat content is often sent several times in a row, either as
  separate keepalives or as change bursts where nothing visible changed.
  Expect duplicates.

## Message 2: turn event (desktop to board)

```json
{"evt":"turn","role":"assistant","content":[{"type":"text","text":"Sending the prompt now."}]}
```

Each event has exactly the keys `evt`, `role` and `content`. 18 were seen:

| `role` | `content` | Count | Notes |
| --- | --- | --- | --- |
| `user` | string | 4 | Typed user message. **Each one was sent twice**, 0.5 to 0.6 s apart. |
| `user` | `[{type:"tool_result", tool_use_id, content, is_error?}]` | 4 | Tool output. `content` is a string or a list of text blocks. |
| `assistant` | `[{type:"text", text}]` | 3 | The full text block, untruncated. |
| `assistant` | `[{type:"thinking", thinking:"", signature}]` | 2 | Thinking text is empty, but the opaque base64 signature (about 1 KB) is included. Useless to a device. |
| `assistant` | `[{type:"tool_use", id, name, input, caller}]` | 5 | Full tool input, for example a whole Bash command line and its description. |

Observations:

- Events fire **per content block as it completes**, not once per turn. One
  assistant reply with thinking, text and two tool calls produced five
  separate assistant events.
- A `tool_use` event and its `tool_result` event can arrive in the same
  millisecond, before the next heartbeat.
- The `tool_use` event for a call that needs approval arrives first, then the
  heartbeat with `waiting` raised. That event (with `name` and `input`) is
  the only place a device can see what is waiting. It has no request id, so
  the device cannot answer it.
- The tool calls seen were `Bash` and two internal desktop tools
  (`mcp__ccd_turn__update_plan`, `mcp__ccd_turn__wrap_up`).
- All events were under 4 KB, so the 4 KB drop rule was not exercised.

## Board to desktop

The board sent **nothing** during the 70 s. That matches the firmware: it
only answers `cmd` messages, and the desktop sent none in this window.

## What was not seen

| Message | Status in this capture |
| --- | --- |
| `prompt` object in a heartbeat | Never, during two approval-requiring tool calls |
| `{"cmd":"permission",...}` from the board | Never. Nothing to answer, since no `prompt.id` was sent |
| `{"cmd":"status"}` poll | Never. The Hardware Buddy window was most likely closed |
| `{"time":[...]}`, `{"cmd":"owner",...}` | Not in window: they are sent on connect, before the capture started |
| `name`, `unpair`, folder push (`char_begin`, `file`, `chunk`, ...) | Not exercised |
| Any `ack` from the board | None, since no `cmd` was sent |

## Timeline of the approval test

Times are seconds from the first line.

| t (s) | Direction | Message |
| --- | --- | --- |
| 0.00, 10.01, 20.03 | desktop→board | Idle keepalives: running 1, waiting 1, `done (success), 1 turns` |
| 21.05, 21.59 | desktop→board | `evt:turn` user message (sent twice) |
| 21.09 | desktop→board | Heartbeat: running 2 |
| 21.90 | desktop→board | `evt:turn` assistant thinking (signature only) |
| 22.28 | desktop→board | `evt:turn` assistant text |
| 22.85 | desktop→board | `evt:turn` tool_use `update_plan` + tool_result, same ms |
| 24.86 | desktop→board | `evt:turn` tool_use `Bash` (full command) |
| 24.86 | desktop→board | Heartbeat: **waiting 2**, `(called Bash)`, no `prompt` |
| 35.04 | desktop→board | Keepalive, still waiting 2 |
| 37.72 | desktop→board | Heartbeat: waiting 1. The user clicked approve on the desktop, about 13 s later |
| 37.85 | desktop→board | `evt:turn` tool_result for the Bash call |
| 40.34 | desktop→board | `evt:turn` tool_use `wrap_up` + tool_result |
| 42.16 | desktop→board | `evt:turn` assistant final text |
| 44.07 | desktop→board | Heartbeat: running 1, `done (success), 5 turns`, tokens up |
| 46.48, 47.12 | desktop→board | `evt:turn` next user message (sent twice) |
| 49.17 | desktop→board | `evt:turn` tool_use `Bash` |
| 49.18 | desktop→board | Heartbeat: waiting 2, no `prompt` |
| 59.44 | desktop→board | Keepalive, still waiting 2 (capture ends at about 70 s) |

## Differences from REFERENCE.md

| Topic | REFERENCE.md | Observed |
| --- | --- | --- |
| `prompt` | Present when a permission decision is needed | Never sent for Code-tab sessions, even when `waiting` went up for a `Bash` call. Approve/Deny on the device is impossible for these sessions. Not fixable in firmware. |
| `waiting` | Sessions blocked on a prompt | Includes stale prompts from other sessions (baseline stuck at 1), so `waiting > 0` alone does not mean a fresh prompt |
| `running` | Sessions actively generating | Baseline 1 while nothing was visibly generating |
| `total` | Count of all sessions | 243, so all sessions ever known, not open ones. `total == 0` is not a usable idle signal. |
| `entries` order | Newest first | **Oldest first**: `msg` equals the last element |
| `entries` content | Timestamped lines such as `10:42 git push` | No timestamps. Lines of the current message, `(called X)`, `(no messages)` or `done (success), N turns` |
| Line length | Not stated | `msg` and each entry cut at about 88 characters |
| Heartbeat cadence | On change, plus a 10 s keepalive | Matches. Change bursts reach about 5 per second, with many duplicates |
| Turn events | One per completed turn | One per content block, user messages duplicated, tool results sent as `role:"user"` |
| Thinking | Not mentioned | Thinking blocks arrive with empty text and about 1 KB of signature |
| Turn event size | Over 4 KB dropped | All seen were under 1.1 KB; the drop rule was not tested |
| Status poll | Every couple of seconds | None in 70 s with the window presumably closed. It looks like the poll only runs while the Hardware Buddy window is open. |
| Writes | Not stated | Write Command (no response), at most 180 bytes, each line starting in a new write |

## Advice for firmware

1. Treat `waiting > 0` without `prompt` as "approve on the desktop". Show the
   last `evt:turn` `tool_use` name and a short hint as context, but give no
   Approve/Deny buttons, since there is no id to answer with.
2. Do not let `evt:turn` lines reset heartbeat state or a pending prompt.
   They are not heartbeats (already handled in `src/data.h`).
3. Expect duplicate heartbeats and duplicate user turn events. De-duplicate
   by content.
4. Skip `thinking` blocks before keeping a turn event around. The signature
   is the bulk of the bytes.
5. Keep the line buffer at 4 KB or more (`src/data.h` uses 4096). That is
   the desktop's drop limit for turn events; lines up to about 1.1 KB were seen.
6. Use a timeout of 25 to 30 s for "link dead": keepalives are 10 s apart.
