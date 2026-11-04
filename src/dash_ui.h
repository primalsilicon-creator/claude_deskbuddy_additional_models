#pragma once
// Dashboard and session screens for the Waveshare 3.49 (428x158 panel) and
// the landscape 2.8" ESP32-32E (308x134 panel, 8-bit colour, touch only).
// Included once from main.cpp, after the shared drawing helpers, so it can
// use spr, W, H, the palette, tama and the stats.
//
// Panel screens are tabs along the top: dash, session, log (the transcript),
// pet, info. PWR (A) or a tap on the pet steps through them; a tap on a tab
// jumps straight to it.
#if defined(SESSION_MODEL)

const int TAB_H = 12;
const uint16_t DASH_YEL  = 0xFFE0;
const uint16_t DASH_CYAN = 0x07FF;
#if defined(BOARD_WAVESHARE_349)
const uint16_t DASH_TILE = 0x10A2;   // tile background, a touch above black
const int TAB_W = 62;
const int TAB_HIT_H = TAB_H + 6;     // touch zone for the tab strip
const bool DASH_WIDE = true;         // room for the tools list beside the chart
#else
// The panel sprite is 8-bit (RGB332): 0x10A2 would round down to black.
const uint16_t DASH_TILE = 0x4208;   // RGB332 (2,2,1): dark grey. 0x39E7 lost its blue and went olive
const int TAB_W = 52;
// Resistive touch is coarse; the strip is drawn 12px tall but takes taps
// down to 24px.
const int TAB_HIT_H = 24;
const bool DASH_WIDE = false;
#endif

static const char* const TAB_NAMES[] = { "dash", "session", "log", "pet", "info" };
uint8_t sessionScroll = 0;
static uint16_t _lastFeedGen = 0;

// Print s at (x, y) cut to maxW pixels in the current font, ending in ".."
// when it doesn't fit.
static void dashFit(const char* s, int x, int y, int maxW) {
  char b[104];
  strncpy(b, s ? s : "", sizeof(b) - 1); b[sizeof(b) - 1] = 0;
  spr.setCursor(x, y);
  if (spr.textWidth(b) <= maxW) { spr.print(b); return; }
  int dots = spr.textWidth("..");
  size_t n = strlen(b);
  while (n > 0) {
    b[--n] = 0;
    if (spr.textWidth(b) + dots <= maxW) break;
  }
  spr.print(b); spr.print("..");
}

static void dashFmtTokens(char* out, size_t n, uint32_t v) {
  if (v >= 1000000)   snprintf(out, n, "%lu.%luM", (unsigned long)(v / 1000000), (unsigned long)((v / 100000) % 10));
  else if (v >= 1000) snprintf(out, n, "%lu.%luK", (unsigned long)(v / 1000), (unsigned long)((v / 100) % 10));
  else                snprintf(out, n, "%lu", (unsigned long)v);
}

static void dashFmtDur(char* out, size_t n, uint32_t ms) {
  uint32_t s = ms / 1000;
  if (s >= 3600)    snprintf(out, n, "%luh%02lu", (unsigned long)(s / 3600), (unsigned long)((s / 60) % 60));
  else if (s >= 60) snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  else              snprintf(out, n, "%lus", (unsigned long)s);
}

static void dashFmtAge(char* out, size_t n, uint32_t ms) {
  uint32_t s = (millis() - ms) / 1000;
  if (s < 60)        snprintf(out, n, "%lus", (unsigned long)s);
  else if (s < 3600) snprintf(out, n, "%lum", (unsigned long)(s / 60));
  else               snprintf(out, n, "%luh", (unsigned long)(s / 3600));
}

// Tab strip across the top of the panel. `label` overrides the active tab's
// text (for "pet 1/2", "info 3/6").
static void drawTabs(uint8_t active, const char* label = nullptr) {
  const Palette& p = characterPalette();
  spr.fillRect(0, 0, W, TAB_H, p.bg);
  spr.setTextFont(1); spr.setTextSize(1);
  for (uint8_t i = 0; i < 5; i++) {
    int x = i * TAB_W;
    bool on = i == active;
    const char* t = (on && label) ? label : TAB_NAMES[i];
    if (on) {
      spr.fillRoundRect(x, 0, TAB_W - 4, TAB_H - 1, 3, p.body);
      spr.setTextColor(p.bg, p.body);
    } else {
      spr.drawRoundRect(x, 0, TAB_W - 4, TAB_H - 1, 3, 0x4208);
      spr.setTextColor(p.textDim, p.bg);
    }
    int tw = spr.textWidth(t);
    spr.setCursor(x + (TAB_W - 4 - tw) / 2, 2);
    spr.print(t);
  }
  // Right end: link state and the clock.
  spr.setTextColor(dataBtActive() ? GREEN : (tama.connected ? DASH_YEL : HOT), p.bg);
  spr.setCursor(5 * TAB_W + 2, 2);
  spr.print(dataBtActive() ? "BT" : (tama.connected ? "USB" : "--"));
  if (dataRtcValid()) {
    char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(W - spr.textWidth(hm) - 2, 2);
    spr.print(hm);
  }
}

// Which tab a tap at panel x lands on, or -1.
static int tabHit(int px) {
  int i = px / TAB_W;
  return (px >= 0 && i < 5) ? i : -1;
}

static void dashTile(int x, int y, int w, int h, const char* label, const char* value,
                     const char* sub, uint16_t valCol, uint16_t bg) {
  const Palette& p = characterPalette();
  spr.fillRoundRect(x, y, w, h, 5, bg);
  spr.setTextFont(1); spr.setTextSize(1);
  spr.setTextColor(p.textDim, bg);
  spr.setCursor(x + 6, y + 4); spr.print(label);
  int lw = spr.textWidth(label);
  spr.setTextFont(4);
  bool big = spr.textWidth(value) <= w - 10 && h >= 42;
  if (!big) spr.setTextFont(2);
  spr.setTextColor(valCol, bg);
  dashFit(value, x + 6, big ? y + 14 : y + h - 19, w - 10);
  spr.setTextFont(1);
  if (sub && *sub && lw + spr.textWidth(sub) + 14 <= w) {
    spr.setTextColor(p.textDim, bg);
    int sw = spr.textWidth(sub);
    spr.setCursor(x + w - sw - 5, y + 4);
    spr.print(sub);
  }
  spr.setTextFont(1);
}

// "Approve on desktop" with what is waiting, filling the bottom h rows.
static void drawDashWait(int y, int h) {
  const SessionModel& m = session();
  bool on = (millis() / 700) % 2;
  uint16_t bg = on ? HOT : 0x8000;
  spr.fillRoundRect(0, y, W, h, 6, bg);
  spr.setTextColor(WHITE, bg);
  spr.setTextFont(2); spr.setTextSize(1);   // (already set by callers; kept for safety)
  char waited[12]; dashFmtDur(waited, sizeof(waited), millis() - m.waitChangedMs);
  spr.setCursor(8, y + 3); spr.print("Approve on desktop");
  spr.setCursor(W - spr.textWidth(waited) - 8, y + 3); spr.print(waited);
  const FeedItem* f = sessionApprovalItem();
  char line[140];
  if (f) snprintf(line, sizeof(line), "%s: %s", f->name, f->text);
  else   snprintf(line, sizeof(line), "%u session%s waiting for you", m.waiting, m.waiting == 1 ? "" : "s");
  dashFit(line, 8, y + h - 19, W - 16);
  spr.setTextFont(1);
}

static void drawDash() {
  const Palette& p = characterPalette();
  const SessionModel& m = session();
  spr.fillRect(0, 0, W, H, p.bg);
  spr.setTextSize(1);   // the ASCII pet leaves it at 2
  drawTabs(DISP_DASH);
  char v[16], sub[24];
  const int GAP = 4, TW = (W - 3 * GAP) / 4, TY = TAB_H + 3, TH = DASH_WIDE ? 46 : 40;
  bool live = tama.connected;

  // Row 1: four tiles
  snprintf(v, sizeof(v), live ? "%u" : "-", tama.sessionsRunning);
  snprintf(sub, sizeof(sub), live ? "of %u" : "", tama.sessionsTotal);
  dashTile(0, TY, TW, TH, "RUNNING", v, sub, tama.sessionsRunning ? GREEN : p.text, DASH_TILE);

  bool fresh = live && sessionFreshWait();
  snprintf(v, sizeof(v), live ? "%u" : "-", tama.sessionsWaiting);
  const char* wsub = !live || !tama.sessionsWaiting ? "" : fresh ? "now" : "stale?";
  dashTile(TW + GAP, TY, TW, TH, "WAITING", v, wsub,
           fresh ? WHITE : (tama.sessionsWaiting ? DASH_YEL : p.text), fresh ? HOT : DASH_TILE);

  dashFmtTokens(v, sizeof(v), tama.tokensToday);
  dashTile(2 * (TW + GAP), TY, TW, TH, "TODAY", live || tama.tokensToday ? v : "-", "tokens",
           p.text, DASH_TILE);

  if (m.inTurn) {
    dashFmtDur(v, sizeof(v), millis() - m.turnStartMs);
    dashTile(3 * (TW + GAP), TY, TW, TH, "TURN", v, m.thinking ? "thinking" : "working", DASH_CYAN, DASH_TILE);
  } else if (m.lastTurnMs) {
    dashFmtDur(v, sizeof(v), m.lastTurnMs);
    dashTile(3 * (TW + GAP), TY, TW, TH, "LAST TURN", v, "", p.text, DASH_TILE);
  } else {
    dashTile(3 * (TW + GAP), TY, TW, TH, "TURN", "-", "", p.textDim, DASH_TILE);
  }

  // Row 2 left: output tokens per 5 minutes over the last 2 hours
  const int R2 = TY + TH + (DASH_WIDE ? 6 : 4), R2H = DASH_WIDE ? 48 : 36;
  const int BAR_PITCH = DASH_WIDE ? 8 : W / TOK_BUCKETS;   // full width on the 2.8"
  const int CHART_W = TOK_BUCKETS * BAR_PITCH;
  uint32_t peak = 1;
  for (uint8_t i = 0; i < TOK_BUCKETS; i++) if (sessionTokBucket(i) > peak) peak = sessionTokBucket(i);
  spr.setTextFont(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(0, R2); spr.print("tokens / 5 min, 2h");
  char pk[12]; dashFmtTokens(pk, sizeof(pk), peak > 1 ? peak : 0);
  spr.setCursor(CHART_W - spr.textWidth(pk), R2); spr.print(pk);
  const int BAR_TOP = R2 + 11, BAR_H = R2H - 12;
  spr.drawFastHLine(0, BAR_TOP + BAR_H, CHART_W, 0x4208);
  for (uint8_t i = 0; i < TOK_BUCKETS; i++) {
    uint32_t b = sessionTokBucket(TOK_BUCKETS - 1 - i);   // oldest on the left
    int bh = b ? (int)((uint64_t)b * BAR_H / peak) : 0;
    if (b && bh < 2) bh = 2;
    if (bh) spr.fillRect(i * BAR_PITCH, BAR_TOP + BAR_H - bh, BAR_PITCH - 2, bh,
                         i == TOK_BUCKETS - 1 ? DASH_CYAN : p.body);
  }

  // Row 2 right: most used tools (on the 2.8" they go in the footer)
  const int TX = CHART_W + 14, TWID = W - TX;
  char hdr[40];
  uint8_t order[TOOL_CAP]; uint8_t nt = m.nTools;
  for (uint8_t i = 0; i < nt; i++) order[i] = i;
  for (uint8_t i = 1; i < nt; i++) {               // sort by count, descending
    uint8_t k = order[i]; int j = i - 1;
    while (j >= 0 && m.tools[order[j]].n < m.tools[k].n) { order[j + 1] = order[j]; j--; }
    order[j + 1] = k;
  }
  if (DASH_WIDE) {
  snprintf(hdr, sizeof(hdr), "tools  %u calls", m.toolCalls);
  spr.setCursor(TX, R2); spr.print(hdr);
  if (m.toolErrors) {
    snprintf(hdr, sizeof(hdr), "%u err", m.toolErrors);
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - spr.textWidth(hdr), R2); spr.print(hdr);
  }
  uint16_t top = nt ? m.tools[order[0]].n : 1;
  for (uint8_t r = 0; r < 4 && r < nt; r++) {
    const ToolCount& tc = m.tools[order[r]];
    int y = R2 + 11 + r * 9;
    spr.setTextColor(p.text, p.bg);
    dashFit(tc.name, TX, y, 78);
    int bw = (int)((TWID - 110) * tc.n / top); if (bw < 2) bw = 2;
    spr.fillRect(TX + 82, y + 1, bw, 6, p.body);
    char c[8]; snprintf(c, sizeof(c), "%u", tc.n);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(W - spr.textWidth(c), y); spr.print(c);
  }
  if (!nt) { spr.setTextColor(p.textDim, p.bg); spr.setCursor(TX, R2 + 20); spr.print("no tool calls yet"); }
  }

  // Row 3: what's happening now, or the approval banner
  const int R3 = R2 + R2H + (DASH_WIDE ? 4 : 2), R3H = H - R3;
  // The banner needs two lines of font 2; on the 2.8" it covers the chart too.
  if (fresh) { if (R3H >= 38) drawDashWait(R3, R3H); else drawDashWait(R2, H - R2); return; }

  spr.drawFastHLine(0, R3 - 2, W, 0x4208);
  spr.setTextFont(2);
  char now[140];
  const FeedItem* last = m.feedN ? &sessionFeedAt(m.feedN - 1) : nullptr;
  if (!live)                    snprintf(now, sizeof(now), "No Claude connected");
  else if (m.thinking)          snprintf(now, sizeof(now), "thinking...");
  else if (last && last->kind == FK_TOOL && last->status == TS_RUNNING)
                                snprintf(now, sizeof(now), "%s: %s", last->name, last->text);
  else if (last && last->kind != FK_DONE)
                                snprintf(now, sizeof(now), "%s: %s", last->name, last->text);
  else                          snprintf(now, sizeof(now), "%s", tama.msg);
  spr.setTextColor(live ? p.text : p.textDim, p.bg);
  dashFit(now, 0, R3, W);

  spr.setTextFont(1);
  spr.setTextColor(p.textDim, p.bg);
  char foot[120];
  if (!DASH_WIDE) {
    // Narrow: the footer lists the most used tools instead.
    int n = snprintf(foot, sizeof(foot), nt ? "tools %u:" : "no tool calls yet", m.toolCalls);
    for (uint8_t r = 0; r < nt && r < 4 && n < (int)sizeof(foot) - 24; r++)
      n += snprintf(foot + n, sizeof(foot) - n, " %s %u", m.tools[order[r]].name, m.tools[order[r]].n);
    dashFit(foot, 0, H - 9, W);
    return;
  }
  int n = snprintf(foot, sizeof(foot), "you %u  claude %u  turns %u", m.userMsgs, m.claudeMsgs, m.turnsDone);
  if (m.desktopApprovals)
    n += snprintf(foot + n, sizeof(foot) - n, "  desk ok %u (avg %lus)", m.desktopApprovals,
                  (unsigned long)(m.waitedTotalS / m.desktopApprovals));
  snprintf(foot + n, sizeof(foot) - n, "  Lv %u  appr %u  deny %u",
           stats().level, stats().approvals, stats().denials);
  dashFit(foot, 0, H - 9, W);
}

// Session feed: one row per user message, Claude text block, tool call or
// finished turn, newest at the bottom.
static void drawSession() {
  const Palette& p = characterPalette();
  const SessionModel& m = session();
  spr.fillRect(0, 0, W, H, p.bg);
  spr.setTextSize(1);   // the ASCII pet leaves it at 2
  drawTabs(DISP_SESSION);
  bool fresh = tama.connected && sessionFreshWait();
  const int BANNER = fresh ? 40 : 0;
  const int TOP = TAB_H + 2, LH = 17;
  const int ROWS = (H - TOP - BANNER) / LH;

  if (m.feedGen != _lastFeedGen) { sessionScroll = 0; _lastFeedGen = m.feedGen; }
  uint8_t n = m.feedN;
  uint8_t maxBack = n > ROWS ? n - ROWS : 0;
  if (sessionScroll > maxBack) sessionScroll = maxBack;

  if (!n) {
    spr.setTextFont(2);
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, TOP + 20);
    spr.print(tama.connected ? "Nothing yet: start a chat in Claude" : "No Claude connected");
    spr.setTextFont(1);
    if (fresh) drawDashWait(H - BANNER, BANNER);
    return;
  }

  int end = n - sessionScroll;
  int start = end - ROWS; if (start < 0) start = 0;
  bool blink = (millis() / 500) % 2;
  for (int i = start; i < end; i++) {
    const FeedItem& f = sessionFeedAt(i);
    int y = TOP + (i - start) * LH;
    char age[8]; dashFmtAge(age, sizeof(age), f.ms);
    spr.setTextFont(1);
    spr.setTextColor(p.textDim, p.bg);
    int aw = spr.textWidth(age);
    spr.setCursor(W - aw, y + 5); spr.print(age);
    int x = 0, maxW = W - aw - 6;
    spr.setTextFont(2);
    switch (f.kind) {
      case FK_USER:
        spr.setTextColor(DASH_CYAN, p.bg); spr.setCursor(0, y); spr.print("you");
        x = 34; spr.setTextColor(p.text, p.bg);
        dashFit(f.text, x, y, maxW - x);
        break;
      case FK_CLAUDE:
        spr.setTextColor(p.body, p.bg); spr.setCursor(0, y); spr.print("claude");
        x = 50; spr.setTextColor(p.text, p.bg);
        dashFit(f.text, x, y, maxW - x);
        break;
      case FK_TOOL: {
        uint16_t dot = f.status == TS_OK ? GREEN : f.status == TS_ERROR ? HOT
                     : f.status == TS_APPROVAL ? (blink ? HOT : p.bg) : DASH_YEL;
        spr.fillCircle(5, y + 8, 3, dot);
        spr.setTextColor(f.status == TS_APPROVAL ? HOT : p.body, p.bg);
        spr.setCursor(14, y);
        char nm[24]; strncpy(nm, f.name, sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0;
        spr.print(nm);
        x = 14 + spr.textWidth(nm) + 6;
        spr.setTextColor(p.textDim, p.bg);
        dashFit(f.text, x, y, maxW - x);
        break;
      }
      case FK_DONE:
        spr.drawFastHLine(0, y + 8, 10, 0x4208);
        spr.setTextColor(p.textDim, p.bg);
        dashFit(f.text, 14, y, maxW - 14);
        break;
      default:
        spr.setTextColor(p.textDim, p.bg);
        dashFit(f.text, 0, y, maxW);
    }
  }
  if (sessionScroll) {
    spr.setTextFont(1);
    char s[8]; snprintf(s, sizeof(s), "-%u", sessionScroll);
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - spr.textWidth(s) - 30, TOP + 4); spr.print(s);
  }
  spr.setTextFont(1);
  if (fresh) drawDashWait(H - BANNER, BANNER);
}

#endif  // SESSION_MODEL
