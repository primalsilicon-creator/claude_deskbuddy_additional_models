#pragma once
// Session activity model for the dashboard UI (Waveshare only).
//
// Built from what the desktop actually sends (docs/BLE-PROTOCOL-OBSERVED.md):
//   - heartbeats: total/running/waiting counts, msg, tokens, tokens_today
//   - {"evt":"turn"} events: one per content block (user text, assistant
//     text, thinking, tool_use, tool_result), user messages sent twice
// Heartbeats never carry a `prompt` for Code-tab sessions, so the model
// guesses which tool call a raised `waiting` belongs to: the newest
// tool_use with no tool_result yet, if it arrived just before the rise.
#include <Arduino.h>
#include <ArduinoJson.h>

// Waveshare 3.49 and the landscape 2.8" ESP32-32E. The 2.8" has no PSRAM
// and ~24KB free with BLE up, so it keeps a smaller feed.
#if defined(BOARD_WAVESHARE_349) || (defined(BOARD_ESP32_32E) && defined(SCREEN_LANDSCAPE))
#define SESSION_MODEL 1
#endif

#if defined(SESSION_MODEL)

enum FeedKind : uint8_t { FK_USER, FK_CLAUDE, FK_TOOL, FK_DONE, FK_INFO };
enum ToolStatus : uint8_t { TS_NONE, TS_RUNNING, TS_OK, TS_ERROR, TS_APPROVAL };

struct FeedItem {
  uint8_t  kind;
  uint8_t  status;      // ToolStatus, for FK_TOOL
  uint32_t ms;          // millis() when it arrived
  uint32_t idHash;      // tool_use id hash, to match its tool_result
#if defined(BOARD_WAVESHARE_349)
  char     name[24];    // tool name, or "you" / "claude"
  char     text[100];   // one line: message start, or the tool's main argument
#else
  char     name[20];
  char     text[64];
#endif
};

struct ToolCount { char name[24]; uint16_t n; };

#if defined(BOARD_WAVESHARE_349)
const uint8_t  FEED_CAP   = 40;
const uint8_t  TOOL_CAP   = 12;
#else
const uint8_t  FEED_CAP   = 20;   // about 1.8KB of feed instead of 5.6KB
const uint8_t  TOOL_CAP   = 8;
#endif
const uint8_t  TOK_BUCKETS = 24;           // 24 x 5 min = the last 2 hours
const uint32_t TOK_BUCKET_MS = 5UL * 60 * 1000;

struct SessionModel {
  FeedItem  feed[FEED_CAP];   // ring buffer, oldest first from feedHead
  uint8_t   feedHead, feedN;
  uint16_t  feedGen;          // bumps on every change, lets the UI reset scroll

  ToolCount tools[TOOL_CAP];
  uint8_t   nTools;
  uint16_t  toolCalls, toolErrors, userMsgs, claudeMsgs, thinkingBlocks;
  uint16_t  turnsDone;        // "done (...)" summaries seen
  uint8_t   lastTurnCount;    // N from the last "done (success), N turns"

  // Turn timing: starts at a user message, ends at a "done" summary.
  uint32_t  turnStartMs, lastTurnMs;   // lastTurnMs = duration of the last one
  bool      inTurn;
  bool      thinking;                  // last assistant block was thinking

  // Waiting with no prompt object. waitBase is the level treated as stale:
  // only waiting above it pulses the "Approve on desktop" banner.
  uint8_t   waiting, waitBase;
  uint32_t  waitChangedMs;
  int8_t    approvalItem;     // feed index of the tool awaiting approval, -1 = unknown
  uint16_t  desktopApprovals; // waits that cleared (approved or denied on the desktop)
  uint32_t  waitedTotalS;     // summed seconds of those waits
  bool      haveHeartbeat;

  // Token history: output tokens per 5-minute bucket.
  uint32_t  tokBucket[TOK_BUCKETS];
  uint32_t  tokBucketEpoch;   // which 5-minute slot tokBucket[epoch % N] holds
  uint32_t  lastTokens;
  bool      haveTokens;
  uint32_t  tokensAtBoot;     // first `tokens` seen, for "this session" totals
  char      lastMsg[100];
};

static SessionModel _sm;

inline const SessionModel& session() { return _sm; }
inline uint8_t sessionFeedCount() { return _sm.feedN; }
// i = 0 is the oldest item still kept
inline const FeedItem& sessionFeedAt(uint8_t i) {
  return _sm.feed[(_sm.feedHead + i) % FEED_CAP];
}
inline bool sessionFreshWait() { return _sm.waiting > _sm.waitBase; }
inline const FeedItem* sessionApprovalItem() {
  if (_sm.approvalItem < 0) return nullptr;
  const FeedItem& f = _sm.feed[_sm.approvalItem];
  return f.status == TS_APPROVAL ? &f : nullptr;
}
// Treat the current waiting count as already seen (stop the banner pulsing).
inline void sessionAckWait() { _sm.waitBase = _sm.waiting; }

static uint32_t _smHash(const char* s) {
  uint32_t h = 2166136261u;
  while (s && *s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}

// First line of s, trimmed, into out. Markdown emphasis markers are dropped
// so "**14:50**" reads as "14:50" on the small screen.
static void _smOneLine(const char* s, char* out, size_t n) {
  size_t o = 0;
  if (!s) { out[0] = 0; return; }
  while (*s == ' ' || *s == '\n' || *s == '\t') s++;
  // The built-in fonts are ASCII only: map the common UTF-8 punctuation
  // to ASCII and anything else to '?'.
  auto put = [&](const char* a) { while (*a && o + 1 < n) out[o++] = *a++; };
  while (*s && *s != '\n' && o + 1 < n) {
    uint8_t c = (uint8_t)*s;
    if (c < 0x80) {
      if (c != '*' && c != '`') out[o++] = (c == '\t' || c == '\r') ? ' ' : (char)c;
      s++;
      continue;
    }
    int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    const uint8_t* u = (const uint8_t*)s;
    if (len == 3 && u[0] == 0xE2 && u[1] == 0x80) {
      uint8_t k = u[2];
      if (k == 0x93 || k == 0x94) put("-");
      else if (k == 0x98 || k == 0x99) put("'");
      else if (k == 0x9C || k == 0x9D) put("\"");
      else if (k == 0xA6) put("...");
      else put("?");
    } else if (len == 3 && u[0] == 0xE2 && u[1] == 0x86 && u[2] == 0x92) {
      put("->");
    } else if (len == 2 && u[0] == 0xC2 && u[1] == 0xA0) {
      put(" ");
    } else {
      put("?");
    }
    for (int i = 0; i < len && *s; i++) s++;
  }
  while (o && out[o - 1] == ' ') o--;
  out[o] = 0;
}

static FeedItem* _smPush(uint8_t kind, const char* name, const char* text) {
  uint8_t idx;
  if (_sm.feedN < FEED_CAP) {
    idx = (_sm.feedHead + _sm.feedN) % FEED_CAP;
    _sm.feedN++;
  } else {
    idx = _sm.feedHead;
    _sm.feedHead = (_sm.feedHead + 1) % FEED_CAP;
    if (_sm.approvalItem == (int8_t)idx) _sm.approvalItem = -1;
  }
  FeedItem& f = _sm.feed[idx];
  memset(&f, 0, sizeof(f));
  f.kind = kind;
  f.ms = millis();
  strncpy(f.name, name ? name : "", sizeof(f.name) - 1);
  _smOneLine(text, f.text, sizeof(f.text));
  _sm.feedGen++;
  return &f;
}

static int8_t _smFeedIndex(const FeedItem* f) { return (int8_t)(f - _sm.feed); }

static void _smCountTool(const char* name) {
  for (uint8_t i = 0; i < _sm.nTools; i++) {
    if (!strcmp(_sm.tools[i].name, name)) { _sm.tools[i].n++; return; }
  }
  uint8_t i = _sm.nTools;
  if (i == TOOL_CAP) {   // full: replace the least used
    i = 0;
    for (uint8_t j = 1; j < TOOL_CAP; j++) if (_sm.tools[j].n < _sm.tools[i].n) i = j;
  } else {
    _sm.nTools++;
  }
  strncpy(_sm.tools[i].name, name, sizeof(_sm.tools[i].name) - 1);
  _sm.tools[i].name[sizeof(_sm.tools[i].name) - 1] = 0;
  _sm.tools[i].n = 1;
}

// Tool names like "mcp__server__do_thing" are long; show "do_thing".
static const char* _smShortTool(const char* n) {
  if (!strncmp(n, "mcp__", 5)) {
    const char* last = strstr(n + 5, "__");
    if (last) return last + 2;
  }
  return n;
}

// The one argument that says what a tool call does.
static const char* _smToolHint(JsonObject in) {
  static const char* const KEYS[] = {
    "command", "file_path", "path", "pattern", "url", "query",
    "description", "prompt", "notebook_path", "skill", nullptr
  };
  if (in.isNull()) return "";
  for (int i = 0; KEYS[i]; i++) {
    const char* v = in[KEYS[i]];
    if (v && *v) {
      if (!strcmp(KEYS[i], "file_path") || !strcmp(KEYS[i], "notebook_path")) {
        const char* b = strrchr(v, '/');
        return b ? b + 1 : v;
      }
      return v;
    }
  }
  for (JsonPair kv : in) {            // otherwise the first string argument
    const char* v = kv.value().as<const char*>();
    if (v && *v) return v;
  }
  return "";
}

static FeedItem* _smFindTool(uint32_t h) {
  for (uint8_t i = 0; i < _sm.feedN; i++) {
    FeedItem& f = _sm.feed[(_sm.feedHead + _sm.feedN - 1 - i) % FEED_CAP];
    if (f.kind == FK_TOOL && f.idHash == h) return &f;
  }
  return nullptr;
}

// {"evt":"turn","role":...,"content":...}
static void sessionOnTurn(JsonDocument& doc) {
  const char* role = doc["role"] | "";
  JsonVariant c = doc["content"];
  bool user = !strcmp(role, "user");

  if (c.is<const char*>()) {
    const char* s = c.as<const char*>();
    if (user) {
      // The desktop sends each user message twice, ~0.5s apart.
      static uint32_t lastHash = 0, lastMs = 0;
      uint32_t h = _smHash(s);
      if (h == lastHash && millis() - lastMs < 5000) return;
      lastHash = h; lastMs = millis();
      _sm.userMsgs++;
      _sm.inTurn = true;
      _sm.turnStartMs = millis();
      _sm.thinking = false;
      _smPush(FK_USER, "you", s);
    } else {
      _sm.claudeMsgs++;
      _smPush(FK_CLAUDE, "claude", s);
    }
    return;
  }
  if (!c.is<JsonArray>()) return;

  for (JsonObject b : c.as<JsonArray>()) {
    const char* type = b["type"] | "";
    if (!strcmp(type, "text")) {
      const char* s = b["text"] | "";
      if (!*s) continue;
      _sm.thinking = false;
      if (user) {
        _sm.userMsgs++;
        _smPush(FK_USER, "you", s);
      } else {
        _sm.claudeMsgs++;
        _smPush(FK_CLAUDE, "claude", s);
      }
    } else if (!strcmp(type, "thinking") || !strcmp(type, "redacted_thinking")) {
      // Text is always empty; the signature is opaque. Count it only.
      _sm.thinkingBlocks++;
      _sm.thinking = true;
    } else if (!strcmp(type, "tool_use")) {
      const char* name = b["name"] | "tool";
      const char* sn = _smShortTool(name);
      _sm.toolCalls++;
      _sm.thinking = false;
      _smCountTool(sn);
      FeedItem* f = _smPush(FK_TOOL, sn, _smToolHint(b["input"]));
      f->status = TS_RUNNING;
      f->idHash = _smHash(b["id"] | "");
    } else if (!strcmp(type, "tool_result")) {
      FeedItem* f = _smFindTool(_smHash(b["tool_use_id"] | ""));
      bool err = b["is_error"] | false;
      if (err) _sm.toolErrors++;
      if (f) {
        if (_sm.approvalItem == _smFeedIndex(f)) _sm.approvalItem = -1;
        f->status = err ? TS_ERROR : TS_OK;
        _sm.feedGen++;
      }
    }
  }
}

static void _smTokens(uint32_t tokens) {
  uint32_t epoch = millis() / TOK_BUCKET_MS;
  if (!_sm.haveTokens) {
    _sm.haveTokens = true;
    _sm.lastTokens = tokens;
    _sm.tokensAtBoot = tokens;
    _sm.tokBucketEpoch = epoch;
    return;
  }
  // Clear buckets for the slots that passed with no heartbeat.
  if (epoch - _sm.tokBucketEpoch >= TOK_BUCKETS) {
    memset(_sm.tokBucket, 0, sizeof(_sm.tokBucket));
    _sm.tokBucketEpoch = epoch;
  }
  while (_sm.tokBucketEpoch < epoch) {
    _sm.tokBucketEpoch++;
    _sm.tokBucket[_sm.tokBucketEpoch % TOK_BUCKETS] = 0;
  }
  if (tokens < _sm.lastTokens) {      // desktop restarted: counter reset
    _sm.lastTokens = tokens;
    _sm.tokensAtBoot = 0;
    return;
  }
  _sm.tokBucket[epoch % TOK_BUCKETS] += tokens - _sm.lastTokens;
  _sm.lastTokens = tokens;
}

// Output tokens in bucket `ago` slots before the current one (0 = now).
inline uint32_t sessionTokBucket(uint8_t ago) {
  if (ago >= TOK_BUCKETS) return 0;
  uint32_t e = _sm.tokBucketEpoch;
  return _sm.tokBucket[(e + TOK_BUCKETS - ago) % TOK_BUCKETS];
}

static void sessionOnHeartbeat(JsonDocument& doc) {
  uint32_t now = millis();
  if (doc["tokens"].is<uint32_t>()) _smTokens(doc["tokens"].as<uint32_t>());

  const char* m = doc["msg"];
  if (m && strcmp(m, _sm.lastMsg)) {
    strncpy(_sm.lastMsg, m, sizeof(_sm.lastMsg) - 1);
    // "done (success), 5 turns" closes the turn.
    if (!strncmp(m, "done (", 6)) {
      const char* nt = strstr(m, "), ");
      _sm.lastTurnCount = nt ? (uint8_t)atoi(nt + 3) : 0;
      _sm.turnsDone++;
      if (_sm.inTurn) { _sm.lastTurnMs = now - _sm.turnStartMs; _sm.inTurn = false; }
      _sm.thinking = false;
      _smPush(FK_DONE, "done", m);
    }
  }

  if (doc["waiting"].is<int>()) {
    uint8_t w = doc["waiting"].as<uint8_t>();
    if (!_sm.haveHeartbeat) {
      // First heartbeat after boot: no history, so count it as fresh. If
      // nothing changes for two minutes it is demoted to stale below.
      _sm.haveHeartbeat = true;
      _sm.waiting = w; _sm.waitBase = 0; _sm.waitChangedMs = now;
      _sm.approvalItem = -1;
    } else if (w != _sm.waiting) {
      if (w > _sm.waiting) {
        // Whatever was already waiting before the rise has been on screen
        // a while; once the new wait clears, don't pulse for it again.
        _sm.waitBase = _sm.waiting;
        // Pin the rise on the newest tool call still without a result, if
        // it came in within the last few seconds.
        for (uint8_t i = 0; i < _sm.feedN && i < 6; i++) {
          uint8_t idx = (_sm.feedHead + _sm.feedN - 1 - i) % FEED_CAP;
          FeedItem& f = _sm.feed[idx];
          if (f.kind != FK_TOOL) continue;
          if (f.status == TS_RUNNING && now - f.ms < 4000) {
            f.status = TS_APPROVAL;
            _sm.approvalItem = (int8_t)idx;
            _sm.feedGen++;
          }
          break;
        }
      } else {
        if (sessionFreshWait()) {
          _sm.desktopApprovals++;
          _sm.waitedTotalS += (now - _sm.waitChangedMs) / 1000;
        }
        if (_sm.approvalItem >= 0) {
          FeedItem& f = _sm.feed[_sm.approvalItem];
          if (f.status == TS_APPROVAL) { f.status = TS_RUNNING; _sm.feedGen++; }
          _sm.approvalItem = -1;
        }
      }
      _sm.waiting = w;
      _sm.waitChangedMs = now;
    }
    if (_sm.waitBase > _sm.waiting) _sm.waitBase = _sm.waiting;
    // A wait that hasn't moved in 2 minutes and isn't pinned to a tool call
    // is most likely a stale session the desktop keeps counting.
    if (sessionFreshWait() && !sessionApprovalItem() && now - _sm.waitChangedMs > 120000) {
      _sm.waitBase = _sm.waiting;
    }
  }
}

#endif  // SESSION_MODEL
