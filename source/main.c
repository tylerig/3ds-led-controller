/*
 * LED Control - 3DS Notification LED Controller
 *
 * Controls the RGB notification LED on the 3DS using the MCUHWC service.
 * Settings are saved to the SD card and restored on next launch.
 * Includes a 10-level undo stack that also persists across restarts.
 *
 * Controls:
 *   Touch          - Tap presets, drag sliders, tap buttons
 *   A              - Apply current color to LED
 *   B              - Turn LED off  (undoable)
 *   X              - Toggle blink mode
 *   Y              - Undo last applied color
 *   START          - Exit (LED stays on; settings already saved)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <3ds.h>
#include <citro2d.h>

// ─────────────────────────────────────────────────────────────
//  Screen dimensions
// ─────────────────────────────────────────────────────────────
#define TOP_W  400
#define TOP_H  240
#define BOT_W  320
#define BOT_H  240

// ─────────────────────────────────────────────────────────────
//  Persistence
//    Save file lives at sdmc:/3ds/led_control/settings.bin
// ─────────────────────────────────────────────────────────────
#define SAVE_DIR   "sdmc:/3ds/led_control"
#define SAVE_PATH  "sdmc:/3ds/led_control/settings.bin"
#define SAVE_MAGIC 0x4C454443u   // "LEDC"
#define SAVE_VER   2
#define UNDO_MAX   10

typedef struct {
    u8   r, g, b;
    bool blink;
    bool led_on;
} LEDSetting;

typedef struct {
    u32        magic;
    u8         version;
    u8         _pad[3];
    LEDSetting current;
    u8         undo_count;
    u8         _pad2[3];
    LEDSetting undo[UNDO_MAX];  // [0] = most recent previous state
} SaveFile;

// ─────────────────────────────────────────────────────────────
//  LED helpers
//
//  NOTE: If you get compile errors, open
//    $(DEVKITPRO)/portlibs/3ds/include/3ds/services/mcuhwc.h
//  and verify the field names in InfoLedPattern match below.
// ─────────────────────────────────────────────────────────────
static void ledSetSolid(InfoLedPattern *pat, u8 r, u8 g, u8 b)
{
    memset(pat, 0, sizeof(InfoLedPattern));
    memset(pat->redPattern,   r, 32);
    memset(pat->greenPattern, g, 32);
    memset(pat->bluePattern,  b, 32);
    pat->smoothing = 0xFF;
}

static void ledSetBlink(InfoLedPattern *pat, u8 r, u8 g, u8 b, u8 speed)
{
    memset(pat, 0, sizeof(InfoLedPattern));
    memset(pat->redPattern,   r, 32);
    memset(pat->greenPattern, g, 32);
    memset(pat->bluePattern,  b, 32);
    pat->smoothing  = 0x00;
    pat->blinkSpeed = speed;
}

static Result ledApplySetting(const LEDSetting *s)
{
    InfoLedPattern pat;
    if (s->blink)
        ledSetBlink(&pat, s->r, s->g, s->b, 0x50);
    else
        ledSetSolid(&pat, s->r, s->g, s->b);
    return MCUHWC_SetInfoLedPattern(&pat);
}

static void ledTurnOff(void)
{
    InfoLedPattern pat;
    memset(&pat, 0, sizeof(InfoLedPattern));
    MCUHWC_SetInfoLedPattern(&pat);
}

// ─────────────────────────────────────────────────────────────
//  Color presets
// ─────────────────────────────────────────────────────────────
typedef struct {
    const char *name;
    u8 r, g, b;
    u32 c2d_color;    // C2D_Color32(r,g,b,255) equivalent
    bool blink;
} ColorPreset;

static const ColorPreset PRESETS[] = {
    { "Red",     255,   0,   0, 0xFF0000FF, false },
    { "Green",     0, 255,   0, 0x00FF00FF, false },
    { "Blue",      0,   0, 255, 0x0000FFFF, false },
    { "Yellow",  255, 200,   0, 0xFFC800FF, false },
    { "Cyan",      0, 200, 255, 0x00C8FFFF, false },
    { "Magenta", 255,   0, 200, 0xFF00C8FF, false },
    { "White",   255, 255, 255, 0xFFFFFFFF, false },
    { "Orange",  255,  80,   0, 0xFF5000FF, false },
    { "Blink R", 255,   0,   0, 0xFF0000FF, true  },
    { "Blink G",   0, 255,   0, 0x00FF00FF, true  },
    { "Blink B",   0,   0, 255, 0x0000FFFF, true  },
    { "Blink W", 255, 255, 255, 0xFFFFFFFF, true  },
};
#define NUM_PRESETS ((int)(sizeof(PRESETS)/sizeof(PRESETS[0])))

// ─────────────────────────────────────────────────────────────
//  App state
// ─────────────────────────────────────────────────────────────
typedef struct {
    // Editing values (shown in sliders/swatch, not yet applied)
    u8   r, g, b;
    bool blink;
    int  selected;          // preset index or -1 for custom

    // What is actually live on the LED hardware right now
    LEDSetting active;

    // Undo stack — [0] is the most recent previous state
    LEDSetting undo[UNDO_MAX];
    int        undo_count;

    char status[80];
} AppState;

// ─────────────────────────────────────────────────────────────
//  Save / Load
// ─────────────────────────────────────────────────────────────
static void saveSettings(const AppState *s)
{
    mkdir(SAVE_DIR, 0777);   // create directory if missing; ignore errors

    FILE *f = fopen(SAVE_PATH, "wb");
    if (!f) return;

    SaveFile sf;
    memset(&sf, 0, sizeof(sf));
    sf.magic       = SAVE_MAGIC;
    sf.version     = SAVE_VER;
    sf.current     = s->active;
    sf.undo_count  = (u8)(s->undo_count > UNDO_MAX ? UNDO_MAX : s->undo_count);
    for (int i = 0; i < sf.undo_count; i++)
        sf.undo[i] = s->undo[i];

    fwrite(&sf, sizeof(sf), 1, f);
    fclose(f);
}

// Returns true if valid save data was found and loaded.
static bool loadSettings(AppState *s)
{
    FILE *f = fopen(SAVE_PATH, "rb");
    if (!f) return false;

    SaveFile sf;
    bool ok = (fread(&sf, sizeof(sf), 1, f) == 1);
    fclose(f);

    if (!ok || sf.magic != SAVE_MAGIC || sf.version != SAVE_VER)
        return false;

    s->active    = sf.current;
    s->r         = sf.current.r;
    s->g         = sf.current.g;
    s->b         = sf.current.b;
    s->blink     = sf.current.blink;

    s->undo_count = (sf.undo_count > UNDO_MAX) ? UNDO_MAX : sf.undo_count;
    for (int i = 0; i < s->undo_count; i++)
        s->undo[i] = sf.undo[i];

    return true;
}

// ─────────────────────────────────────────────────────────────
//  Undo stack
// ─────────────────────────────────────────────────────────────

// Shift [0..n-1] up to [1..n] and insert at [0]. Drops oldest if full.
static void undoPush(AppState *s)
{
    int slots = (s->undo_count < UNDO_MAX) ? s->undo_count : UNDO_MAX - 1;
    for (int i = slots; i > 0; i--)
        s->undo[i] = s->undo[i-1];
    s->undo[0] = s->active;
    if (s->undo_count < UNDO_MAX) s->undo_count++;
}

// Pop [0], shift remaining down. Returns false if stack empty.
static bool undoPop(AppState *s, LEDSetting *out)
{
    if (s->undo_count == 0) return false;
    *out = s->undo[0];
    for (int i = 0; i < s->undo_count - 1; i++)
        s->undo[i] = s->undo[i+1];
    s->undo_count--;
    return true;
}

// ─────────────────────────────────────────────────────────────
//  Apply / Undo actions
// ─────────────────────────────────────────────────────────────
static void applyLED(AppState *s)
{
    LEDSetting next = { s->r, s->g, s->b, s->blink, true };

    undoPush(s);   // save current active before replacing it

    Result rc = ledApplySetting(&next);
    if (R_SUCCEEDED(rc)) {
        s->active = next;
        saveSettings(s);
        snprintf(s->status, sizeof(s->status),
                 "Applied: R=%d G=%d B=%d%s  [undo: %d]",
                 s->r, s->g, s->b,
                 s->blink ? " blink" : "",
                 s->undo_count);
    } else {
        // Roll back push on failure
        if (s->undo_count > 0) s->undo_count--;
        snprintf(s->status, sizeof(s->status),
                 "Error: 0x%08lX", rc);
    }
}

static void applyOff(AppState *s)
{
    undoPush(s);
    ledTurnOff();
    LEDSetting off = { 0, 0, 0, false, false };
    s->active = off;
    saveSettings(s);
    s->selected = -1;
    snprintf(s->status, sizeof(s->status),
             "LED off.  [undo: %d]", s->undo_count);
}

static void doUndo(AppState *s)
{
    LEDSetting prev;
    if (!undoPop(s, &prev)) {
        snprintf(s->status, sizeof(s->status), "Nothing to undo.");
        return;
    }

    Result rc = 0;
    if (!prev.led_on) {
        ledTurnOff();
    } else {
        rc = ledApplySetting(&prev);
    }

    if (R_SUCCEEDED(rc)) {
        s->active   = prev;
        s->r        = prev.r;
        s->g        = prev.g;
        s->b        = prev.b;
        s->blink    = prev.blink;
        s->selected = -1;
        saveSettings(s);
        if (!prev.led_on)
            snprintf(s->status, sizeof(s->status),
                     "Undo → LED off.  [undo: %d]", s->undo_count);
        else
            snprintf(s->status, sizeof(s->status),
                     "Undo → R=%d G=%d B=%d%s  [undo: %d]",
                     prev.r, prev.g, prev.b,
                     prev.blink ? " blink" : "",
                     s->undo_count);
    } else {
        snprintf(s->status, sizeof(s->status),
                 "Undo failed: 0x%08lX", rc);
    }
}

// ─────────────────────────────────────────────────────────────
//  UI layout constants (bottom screen, 320×240)
// ─────────────────────────────────────────────────────────────
#define PRESET_COLS   4
#define PRESET_W     72
#define PRESET_H     30
#define PRESET_PAD    4
#define PRESET_X_OFF  8
#define PRESET_Y_OFF  8

#define SLIDER_X     20
#define SLIDER_Y    110
#define SLIDER_W    220
#define SLIDER_H     10
#define SLIDER_STEP   3

// Bottom button row
#define BTN_Y         195
#define BTN_H          38
#define BTN_APPLY_X    5
#define BTN_APPLY_W   95
#define BTN_OFF_X    108
#define BTN_OFF_W     95
#define BTN_UNDO_X   211
#define BTN_UNDO_W    99

// ─────────────────────────────────────────────────────────────
//  Drawing helpers
// ─────────────────────────────────────────────────────────────
static void drawRect(float x, float y, float w, float h, u32 c)
{
    C2D_DrawRectSolid(x, y, 0.5f, w, h, c);
}

static void drawBordered(float x, float y, float w, float h,
                         float b, u32 fill, u32 outline)
{
    drawRect(x, y, w, h, outline);
    drawRect(x+b, y+b, w-2*b, h-2*b, fill);
}

static u32 rgb8(u8 r, u8 g, u8 b)         { return C2D_Color32(r,g,b,255); }
static u32 rgba8(u8 r, u8 g, u8 b, u8 a)  { return C2D_Color32(r,g,b,a); }

// ─────────────────────────────────────────────────────────────
//  Touch helper
// ─────────────────────────────────────────────────────────────
static bool touchIn(touchPosition t, int x, int y, int w, int h)
{
    return (t.px >= x && t.px < x+w && t.py >= y && t.py < y+h);
}

// ─────────────────────────────────────────────────────────────
//  Draw bottom screen
// ─────────────────────────────────────────────────────────────
static void drawBottom(const AppState *s)
{
    drawRect(0, 0, BOT_W, BOT_H, rgb8(30,30,40));

    // Preset grid
    for (int i = 0; i < NUM_PRESETS; i++) {
        int col = i % PRESET_COLS;
        int row = i / PRESET_COLS;
        int x   = PRESET_X_OFF + col*(PRESET_W+PRESET_PAD);
        int y   = PRESET_Y_OFF + row*(PRESET_H+PRESET_PAD);
        bool sel = (s->selected == i);
        u32 brd = sel ? rgb8(255,255,255) : rgb8(70,70,90);
        drawBordered(x, y, PRESET_W, PRESET_H, sel?2.0f:1.0f,
                     PRESETS[i].c2d_color, brd);
        if (PRESETS[i].blink)
            drawRect(x+PRESET_W-10, y+4, 6, 6, rgb8(255,230,80));
    }

    // RGB sliders
    const u8 *ch[3] = { &s->r, &s->g, &s->b };
    u32 sc[3] = { rgb8(200,50,50), rgb8(50,200,50), rgb8(50,50,220) };
    for (int i = 0; i < 3; i++) {
        int y  = SLIDER_Y + i*22;
        int fw = (int)(*ch[i] * SLIDER_W / 255);
        drawRect(SLIDER_X, y, SLIDER_W, SLIDER_H, rgb8(60,60,70));
        drawRect(SLIDER_X, y, fw, SLIDER_H, sc[i]);
        drawRect(SLIDER_X+fw-4, y-4, 8, SLIDER_H+8, rgb8(230,230,230));
    }

    // Color preview swatch
    drawBordered(248, 110, 60, 54, 2,
                 rgb8(s->r, s->g, s->b), rgb8(200,200,200));

    // Blink badge below swatch
    u32 bb = s->blink ? rgb8(230,180,40) : rgba8(55,55,70,255);
    drawBordered(248, 168, 60, 20, 1, bb, rgb8(150,150,150));

    // Apply button (green)
    drawBordered(BTN_APPLY_X, BTN_Y, BTN_APPLY_W, BTN_H, 2,
                 rgb8(35,130,65), rgb8(170,220,170));

    // Off button (red)
    drawBordered(BTN_OFF_X, BTN_Y, BTN_OFF_W, BTN_H, 2,
                 rgb8(140,35,35), rgb8(220,170,170));

    // Undo button (blue-gray, dimmed when nothing to undo)
    bool can_undo = (s->undo_count > 0);
    u32 uf = can_undo ? rgb8(60,85,140)    : rgb8(44,44,54);
    u32 ub = can_undo ? rgb8(150,175,230)  : rgb8(80,80,95);
    drawBordered(BTN_UNDO_X, BTN_Y, BTN_UNDO_W, BTN_H, 2, uf, ub);

    // Depth indicator bar inside undo button
    if (can_undo) {
        int bar_w = s->undo_count * (BTN_UNDO_W-12) / UNDO_MAX;
        drawRect(BTN_UNDO_X+6, BTN_Y+BTN_H-8, bar_w, 4,
                 rgba8(170,195,255,200));
    }
}

// ─────────────────────────────────────────────────────────────
//  Draw top screen
// ─────────────────────────────────────────────────────────────
static void drawTop(const AppState *s)
{
    drawRect(0, 0, TOP_W, TOP_H, rgb8(20,20,30));

    bool on = s->active.led_on;
    u32 led_col = on ? rgb8(s->active.r, s->active.g, s->active.b)
                     : rgb8(38,38,45);

    // Soft glow backdrop
    if (on)
        drawRect(125, 45, 150, 150,
                 rgba8(s->active.r/5, s->active.g/5, s->active.b/5, 220));

    // LED block
    drawBordered(155, 70, 90, 90, 4, led_col, rgb8(120,120,130));

    // Outer glow ring
    u32 ring = on ? rgba8(s->active.r, s->active.g, s->active.b, 100)
                  : rgba8(40,40,50,200);
    drawBordered(147, 62, 106, 106, 3, rgba8(0,0,0,0), ring);

    // RGB bar readout
    u8  bv[3] = { s->active.r, s->active.g, s->active.b };
    u32 bc[3] = { rgb8(200,50,50), rgb8(50,200,50), rgb8(50,50,200) };
    for (int i = 0; i < 3; i++) {
        int bw = on ? (bv[i]*180/255) : 0;
        drawRect(110, 183+i*14, 180, 10, rgb8(42,42,52));
        drawRect(110, 183+i*14, bw,  10, bc[i]);
    }
}

// ─────────────────────────────────────────────────────────────
//  Input handling
// ─────────────────────────────────────────────────────────────
static void handleInput(AppState *s, u32 kDown, u32 kHeld,
                        touchPosition touch, bool touched)
{
    if (kDown & KEY_A) applyLED(s);
    if (kDown & KEY_B) applyOff(s);
    if (kDown & KEY_Y) doUndo(s);
    if (kDown & KEY_X) s->blink = !s->blink;

    // D-Pad nudges (custom mode only)
    if (s->selected < 0) {
        if (kHeld & KEY_RIGHT) { if (s->r<255) s->r += SLIDER_STEP; }
        if (kHeld & KEY_LEFT)  { if (s->r>0)   s->r -= SLIDER_STEP; }
        if (kHeld & KEY_UP)    { if (s->g<255) s->g += SLIDER_STEP; }
        if (kHeld & KEY_DOWN)  { if (s->g>0)   s->g -= SLIDER_STEP; }
        if (kHeld & KEY_L)     { if (s->b<255) s->b += SLIDER_STEP; }
        if (kHeld & KEY_R)     { if (s->b>0)   s->b -= SLIDER_STEP; }
    }

    if (!touched) return;

    // Preset taps
    for (int i = 0; i < NUM_PRESETS; i++) {
        int col = i%PRESET_COLS, row = i/PRESET_COLS;
        int x = PRESET_X_OFF + col*(PRESET_W+PRESET_PAD);
        int y = PRESET_Y_OFF + row*(PRESET_H+PRESET_PAD);
        if (touchIn(touch, x, y, PRESET_W, PRESET_H)) {
            s->selected = i;
            s->r = PRESETS[i].r;
            s->g = PRESETS[i].g;
            s->b = PRESETS[i].b;
            s->blink = PRESETS[i].blink;
            applyLED(s);
            return;
        }
    }

    // Slider drags
    for (int i = 0; i < 3; i++) {
        int y = SLIDER_Y + i*22;
        if (touchIn(touch, SLIDER_X, y-6, SLIDER_W, SLIDER_H+12)) {
            int raw = touch.px - SLIDER_X;
            if (raw < 0) raw = 0;
            if (raw > SLIDER_W) raw = SLIDER_W;
            u8 val = (u8)(raw*255/SLIDER_W);
            switch (i) {
                case 0: s->r=val; break;
                case 1: s->g=val; break;
                case 2: s->b=val; break;
            }
            s->selected = -1;
            return;
        }
    }

    // Blink badge tap
    if (touchIn(touch, 248, 168, 60, 20)) { s->blink=!s->blink; return; }

    // Bottom buttons
    if (touchIn(touch, BTN_APPLY_X, BTN_Y, BTN_APPLY_W, BTN_H)) { applyLED(s); return; }
    if (touchIn(touch, BTN_OFF_X,   BTN_Y, BTN_OFF_W,   BTN_H)) { applyOff(s); return; }
    if (touchIn(touch, BTN_UNDO_X,  BTN_Y, BTN_UNDO_W,  BTN_H)) { doUndo(s);   return; }
}

// ─────────────────────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────────────────────
int main(void)
{
    gfxInitDefault();

    if (R_FAILED(mcuHwcInit())) {
        gfxExit();
        return 1;
    }

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget *top = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    C3D_RenderTarget *bot = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    // ── Initialize state ─────────────────────────────────────
    AppState state;
    memset(&state, 0, sizeof(state));
    state.selected = -1;

    bool restored = loadSettings(&state);
    if (restored && state.active.led_on) {
        ledApplySetting(&state.active);   // re-drive hardware to match saved state
        snprintf(state.status, sizeof(state.status),
                 "Restored: R=%d G=%d B=%d%s  [undo: %d]",
                 state.active.r, state.active.g, state.active.b,
                 state.active.blink ? " blink" : "",
                 state.undo_count);
    } else if (restored) {
        snprintf(state.status, sizeof(state.status),
                 "LED was off last session.  [undo: %d]", state.undo_count);
    } else {
        state.r = 0; state.g = 0; state.b = 255;
        snprintf(state.status, sizeof(state.status),
                 "Welcome! Tap a color or adjust sliders, then Apply.");
    }

    // ── Main loop ────────────────────────────────────────────
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        u32 kHeld = hidKeysHeld();

        if (kDown & KEY_START) break;

        touchPosition touch = {0};
        bool touched = ((kDown | kHeld) & KEY_TOUCH) != 0;
        if (touched) hidTouchRead(&touch);

        handleInput(&state, kDown, kHeld, touch, touched);

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        C2D_TargetClear(top, C2D_Color32(20,20,30,255));
        C2D_SceneBegin(top);
        drawTop(&state);

        C2D_TargetClear(bot, C2D_Color32(30,30,40,255));
        C2D_SceneBegin(bot);
        drawBottom(&state);

        C3D_FrameEnd(0);
    }

    // EXIT: LED intentionally stays on. Save is already up to date
    // (written on every Apply/Off/Undo), but save once more to be safe.
    saveSettings(&state);

    C2D_Fini();
    C3D_Fini();
    mcuHwcExit();
    gfxExit();
    return 0;
}
