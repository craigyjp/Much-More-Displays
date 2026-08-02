#include "SPI.h"
#include "TFT_eSPI.h"
#include <MIDI.h>
#include "Constants.h"
#include "Parameters.h"
#include "MidiCC.h"
#include "ScreenParams.h"

#include "Hardware.h"

#define MIDI_CHANNEL 1

// Use hardware SPI
TFT_eSPI tft = TFT_eSPI();

// Each dial is rendered off-screen then pushed in one go. This removes the
// anti-aliasing residue you get from repainting arcs in place, and removes
// flicker as a side effect. 64 x 64 x 2 bytes = 8KB.
#define SPR_SIZE 64
#define SPR_C (SPR_SIZE / 2)
TFT_eSprite dial = TFT_eSprite(&tft);

// Screen is used in landscape (rotation 1) = 320 x 240
int screenWidth = 320;
int screenHeight = 240;

// ---------------------------------------------------------------------------
//  Values are native MIDI, 0..127. panelData[] holds them unscaled.
// ---------------------------------------------------------------------------
#define VALUE_MAX 127
#define VALUE_MID 64  // the centre detent position

// ---------------------------------------------------------------------------
//  Dial geometry (4 columns x 3 rows = 12 slots)
// ---------------------------------------------------------------------------
#define NUM_KNOBS 12

// Slot types
#define KNOB_NORMAL 0   // fills from the left stop  (level, amount)
#define KNOB_CENTRE 1   // fills outward from 12 o'clock  (PW, detune, pan)
#define KNOB_EMPTY 255  // slot draws nothing

#define ARC_R 29      // outer radius of the value arc
#define ARC_IR 24     // inner radius of the value arc
#define BODY_R 21     // radius of the solid knob body
#define PTR_IN 16     // pointer starts here
#define PTR_OUT 20    // pointer ends here
#define LABEL_OFF 33  // label distance below the centre

// Pot sweep, in TFT_eSPI arc angles: 0 = 6 o'clock, increasing clockwise.
#define ARC_START 45
#define ARC_END 315
#define ARC_SWEEP (ARC_END - ARC_START)
#define ARC_MID ((ARC_START + ARC_END) / 2)  // 180 = 12 o'clock

// Colours
#define COL_BODY 0x2124   // dark grey knob face
#define COL_RIM 0x4A49    // slightly lighter rim
#define COL_TRACK 0x39E7  // unfilled part of the arc
#define COL_TICK 0xC618   // centre detent mark

// ---------------------------------------------------------------------------
//  HOW TO CONFIGURE THE PAGE
//  One row per grid position (1-12, left-to-right, top-to-bottom):
//
//    KNOB(cc, param, "LABEL")                       normal dial, reads 0..127
//    KNOB_RANGE(cc, param, "LABEL", lo, hi, "u")    normal dial, custom range
//    CENTRE(cc, param, "LABEL")                     centre dial, reads -64..+63
//    CENTRE_RANGE(cc, param, "LABEL", lo, hi, "u")  centre dial, custom range
//    EMPTY_SLOT                                     nothing drawn
//
//  lo/hi are what the dial reads fully anticlockwise / fully clockwise.
//  "u" is an optional unit suffix, e.g. "%" - use "" for none.
//  For a CENTRE dial, make the range symmetric (1..99, -12..12) so the
//  midpoint lands exactly on the detent at MIDI 64.
// ---------------------------------------------------------------------------

struct Knob {
  uint8_t cc;
  uint8_t param;  // index into panelData[]
  const char *label;
  uint8_t type;        // KNOB_NORMAL / KNOB_CENTRE / KNOB_EMPTY
  int16_t dispMin;     // reading at MIDI 0
  int16_t dispMax;     // reading at MIDI 127
  const char *suffix;  // unit text, "" for none
  int16_t cx;          // set automatically
  int16_t cy;          // set automatically
};

#define KNOB(c, p, l) \
  { c, p, l, KNOB_NORMAL, 0, 127, "", 0, 0 }
#define KNOB_RANGE(c, p, l, lo, hi, u) \
  { c, p, l, KNOB_NORMAL, lo, hi, u, 0, 0 }
#define CENTRE(c, p, l) \
  { c, p, l, KNOB_CENTRE, -64, 63, "", 0, 0 }
#define CENTRE_RANGE(c, p, l, lo, hi, u) \
  { c, p, l, KNOB_CENTRE, lo, hi, u, 0, 0 }
#define EMPTY_SLOT \
  { 0, 0, "", KNOB_EMPTY, 0, 0, "", 0, 0 }

// ---- DCO1 page ------------------------------------------------------------
Knob knobs[NUM_KNOBS] = {

  KNOB(CCfilterAttack, P_filterAttack, "F ATT"),
  KNOB(CCfilterDecay, P_filterDecay, "F DEC"),
  KNOB_RANGE(CCfilterSustain, P_filterSustain, "F SUS", 0, 100, ""),
  KNOB(CCfilterRelease, P_filterRelease, "F REL"),

  KNOB(CCampAttack, P_ampAttack, "A ATT"),
  KNOB(CCampDecay, P_ampDecay, "A DEC"),
  KNOB_RANGE(CCampSustain, P_ampSustain, "A SUS", 0, 100, ""),
  KNOB(CCampRelease, P_ampRelease, "A REL"),

  EMPTY_SLOT,
  EMPTY_SLOT,
  EMPTY_SLOT,
  EMPTY_SLOT,
};

//MIDI 5 Pin DIN
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

uint16_t getActiveColor() {
  if (upperSW) return TFT_CYAN;
  if (lowerSW) return TFT_YELLOW;
  return TFT_DARKGREY;  // neither set - shouldn't happen
}

// Position of this value on the dial: 0.0 = anticlockwise stop, 1.0 = clockwise.
// Centre dials map each half separately so MIDI 64 lands exactly on 0.5.
float knobFraction(int i, int value) {
  if (value < 0) value = 0;
  if (value > VALUE_MAX) value = VALUE_MAX;

  if (knobs[i].type == KNOB_CENTRE) {
    if (value >= VALUE_MID)
      return 0.5 + 0.5 * (value - VALUE_MID) / (float)(VALUE_MAX - VALUE_MID);
    else
      return 0.5 * value / (float)VALUE_MID;
  }
  return value / (float)VALUE_MAX;
}

// ---------------------------------------------------------------------------
//  Sprite drawing helpers - all work in sprite-local coordinates
// ---------------------------------------------------------------------------

// Converts a TFT arc angle (0 = 6 o'clock, clockwise) to a screen direction.
void arcAngleToVector(float deg, float *dx, float *dy) {
  float r = deg * DEG_TO_RAD;
  *dx = -sin(r);
  *dy = cos(r);
}

// roundEnds is false: square ends abut cleanly with no overlapping caps.
void sprArc(int start, int end, uint16_t colour) {
  if (end <= start) return;
  dial.drawSmoothArc(SPR_C, SPR_C, ARC_R, ARC_IR, start, end,
                     colour, TFT_BLACK, false);
}

void sprPointer(float deg, uint16_t colour) {
  float dx, dy;
  arcAngleToVector(deg, &dx, &dy);
  dial.drawWedgeLine(SPR_C + PTR_IN * dx, SPR_C + PTR_IN * dy,
                     SPR_C + PTR_OUT * dx, SPR_C + PTR_OUT * dy,
                     1.4, 2.2, colour, COL_BODY);
}

// The detent mark at 12 o'clock, drawn across the arc band.
void sprCentreTick(uint16_t colour) {
  float dx, dy;
  arcAngleToVector(ARC_MID, &dx, &dy);
  dial.drawLine(SPR_C + (int)round((ARC_IR - 1) * dx),
                SPR_C + (int)round((ARC_IR - 1) * dy),
                SPR_C + (int)round((ARC_R + 1) * dx),
                SPR_C + (int)round((ARC_R + 1) * dy), colour);
}

// ---------------------------------------------------------------------------
//  Layout
// ---------------------------------------------------------------------------
void layoutKnobs() {
  const int16_t colX[4] = { 40, 120, 200, 280 };
  const int16_t rowY[3] = { 30, 110, 190 };
  for (int i = 0; i < NUM_KNOBS; i++) {
    knobs[i].cx = colX[i % 4];
    knobs[i].cy = rowY[i / 4];
  }
}

// ---------------------------------------------------------------------------
//  Drawing
// ---------------------------------------------------------------------------

// Only the label now - everything else lives in the sprite.
void drawKnobLabel(int i) {
  if (knobs[i].type == KNOB_EMPTY) return;
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(knobs[i].label, knobs[i].cx, knobs[i].cy + LABEL_OFF);
}

// Renders the whole dial into the sprite, then pushes it in one operation.
void drawKnobValue(int i) {
  if (knobs[i].type == KNOB_EMPTY) return;

  int value = panelData[knobs[i].param];  // 0..127
  if (value < 0) value = 0;
  if (value > VALUE_MAX) value = VALUE_MAX;

  uint16_t active = getActiveColor();
  float frac = knobFraction(i, value);
  int fillTo = ARC_START + (int)lround(frac * ARC_SWEEP);

  dial.fillSprite(TFT_BLACK);

  // Full track first, then the active portion painted over it. Because the
  // buffer is cleared every frame, nothing can accumulate.
  sprArc(ARC_START, ARC_END, COL_TRACK);

  if (knobs[i].type == KNOB_CENTRE) {
    if (fillTo >= ARC_MID) sprArc(ARC_MID, fillTo, active);
    else sprArc(fillTo, ARC_MID, active);
    sprCentreTick(COL_TICK);
  } else {
    sprArc(ARC_START, fillTo, active);
  }

  dial.fillSmoothCircle(SPR_C, SPR_C, BODY_R, COL_BODY, TFT_BLACK);
  dial.drawCircle(SPR_C, SPR_C, BODY_R, COL_RIM);

  sprPointer((float)fillTo, active);

  // Reading, remapped onto this slot's display range
  long span = (long)knobs[i].dispMax - (long)knobs[i].dispMin;
  int shown = knobs[i].dispMin + (int)lround(frac * span);

  char buf[12];
  snprintf(buf, sizeof(buf), "%d%s", shown, knobs[i].suffix);

  dial.setFreeFont(&FreeSans9pt7b);
  if (dial.textWidth(buf) > (BODY_R * 2 - 8)) {
    dial.setFreeFont(NULL);
    dial.setTextFont(2);
  }
  dial.setTextColor(active, COL_BODY);
  dial.setTextDatum(MC_DATUM);
  dial.drawString(buf, SPR_C, SPR_C);

  dial.pushSprite(knobs[i].cx - SPR_C, knobs[i].cy - SPR_C);
}

void drawThickLine(int x0, int y0, int x1, int y1, uint16_t color, int thickness) {
  int t = thickness / 2;
  for (int dx = -t; dx <= t; dx++) {
    for (int dy = -t; dy <= t; dy++) {
      tft.drawLine(x0 + dx, y0 + dy, x1 + dx, y1 + dy, color);
    }
  }
}

void drawThickCurve(int x0, int y0, int x1, int y1, uint16_t color,
                    int thickness, float curve, uint8_t shape) {
  const int steps = 14;
  int px = x0, py = y0;
  for (int i = 1; i <= steps; i++) {
    float t = (float)i / steps;
    float s;
    switch (shape) {
      case ENV_RISE: s = 1.0 - pow(1.0 - t, curve); break;   // convex rise
      case ENV_FALL: s = pow(t, 1.0 / curve); break;   // concave fall
      default:       s = t;                         break;   // linear
    }
    int cx = (int)lround(x0 + (x1 - x0) * t);
    int cy = (int)lround(y0 + (y1 - y0) * s);
    drawThickLine(px, py, cx, cy, color, thickness);
    px = cx;
    py = cy;
  }
}

void drawEnvADSR(int attack, int decay, int sustain, int release,
                 int boxX, int boxY, int boxW, int boxH,
                 int thickness, bool isExponential, bool adr) {

  tft.fillRect(boxX, boxY, boxW, boxH, TFT_BLACK);   // clears only this box

  int pad   = thickness / 2 + 1;
  int x0    = boxX + pad;
  int base  = boxY + boxH - 1 - pad;      // zero line
  int envH  = boxH - 1 - pad * 2;         // full-scale height
  int plotW = boxW - 1 - pad * 2;

  float fatt = constrain(attack  / (float)VALUE_MAX, 0.0, 1.0);
  float fdec = constrain(decay   / (float)VALUE_MAX, 0.0, 1.0);
  float fsus = constrain(sustain / (float)VALUE_MAX, 0.0, 1.0);
  float frel = constrain(release / (float)VALUE_MAX, 0.0, 1.0);

  int x1 = x0 + (int)lround((fatt) * 0);   // placeholder, set below
  int y1 = base - envH;
  int x2, y2, x3, y3, x4, y4;

  if (adr) {
    // Attack -> decay to the sustain level -> release straight to zero.
    // No flat hold; the sustain knob sets the decay/release break point.
    float total = fatt + fdec + frel;
    if (total < 0.01) total = 0.01;
    int attW = (int)lround((fatt / total) * plotW);
    int decW = (int)lround((fdec / total) * plotW);
    int relW = (int)lround((frel / total) * plotW);

    x1 = x0 + attW;                        y1 = base - envH;
    x2 = x1 + decW;                        y2 = base - (int)lround(fsus * envH);
    x3 = x2;                               y3 = y2;                // no hold
    x4 = min(x2 + relW, x0 + plotW);       y4 = base;
  } else {
    // Standard ADSR with a flat sustain leg.
    const float susFrac = 0.5;
    float total = fatt + fdec + frel + susFrac;
    int attW = (int)lround((fatt    / total) * plotW);
    int decW = (int)lround((fdec    / total) * plotW);
    int susW = (int)lround((susFrac / total) * plotW);
    int relW = (int)lround((frel    / total) * plotW);

    x1 = x0 + attW;                        y1 = base - envH;
    x2 = x1 + decW;                        y2 = base - (int)lround(fsus * envH);
    x3 = x2 + susW;                        y3 = y2;
    x4 = min(x3 + relW, x0 + plotW);       y4 = base;
  }

  uint16_t envColor = getActiveColor();
  float   curve = isExponential ? 2.0 : 1.0;
  uint8_t rise  = isExponential ? ENV_RISE : ENV_LIN;
  uint8_t fall  = isExponential ? ENV_FALL : ENV_LIN;

  drawThickCurve(x0, base, x1, y1, envColor, thickness, curve, rise);
  drawThickCurve(x1, y1,   x2, y2, envColor, thickness, curve, fall);
  drawThickLine (x2, y2,   x3, y3, envColor, thickness);          // zero-length in ADR
  drawThickCurve(x3, y3,   x4, y4, envColor, thickness, curve, fall);
}

void drawFilterEnv() {
  bool adr = panelData[P_env2_env3_adsr] != 0;
  drawEnvADSR(panelData[P_filterAttack],  panelData[P_filterDecay],
              panelData[P_filterSustain], panelData[P_filterRelease],
              ENV_BOX_X, ENV_BOX_Y, ENV_BOX_W, ENV_BOX_H,
              3, panelData[P_filterLogLin] != 0, adr);
}

void drawAmpEnv() {
  bool adr = panelData[P_env2_env3_adsr] != 0;
  drawEnvADSR(panelData[P_ampAttack],  panelData[P_ampDecay],
              panelData[P_ampSustain], panelData[P_ampRelease],
              VCA_BOX_X, VCA_BOX_Y, VCA_BOX_W, VCA_BOX_H,
              3, panelData[P_ampLogLin] != 0, adr);
}

void renderCurrentPatchPage() {
  tft.fillScreen(TFT_BLACK);
  for (int i = 0; i < NUM_KNOBS; i++) {
    drawKnobLabel(i);
    drawKnobValue(i);
  }

  drawFilterEnv();
  drawAmpEnv();
}

// ---------------------------------------------------------------------------
//  Setup / MIDI
// ---------------------------------------------------------------------------

void setup() {
  tft.init();
  tft.setRotation(1);

  SetupHardware();

  tft.fillScreen(TFT_BLACK);

  dial.setColorDepth(16);  // 16-bit needed for anti-aliasing
  dial.createSprite(SPR_SIZE, SPR_SIZE);

  layoutKnobs();

  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.setHandleControlChange(myConvertControlChange);
  MIDI.turnThruOn(midi::Thru::Mode::Off);

  renderCurrentPatchPage();
}

void myConvertControlChange(byte channel, byte number, byte value) {

  if (channel == 1) {
    myControlChange(channel, number, value);  // native 0..127, no scaling
  }

  if (channel == 2) {
    myLEDupdate(channel, number, value);
  }
}

// Data-driven: whichever slot uses this CC gets updated.
void myControlChange(byte channel, byte control, int value) {
  for (int i = 0; i < NUM_KNOBS; i++) {
    if (knobs[i].type != KNOB_EMPTY && knobs[i].cc == control) {
      panelData[knobs[i].param] = value;
      drawKnobValue(i);
    }
  }

  if (control == CCfilterAttack  || control == CCfilterDecay ||
    control == CCfilterSustain || control == CCfilterRelease) {
    drawFilterEnv();
  }

  if (control == CCampAttack  || control == CCampDecay ||
    control == CCampSustain || control == CCampRelease) {
    drawAmpEnv();
  }
}

void myLEDupdate(byte channel, byte control, int value) {
  switch (control) {

    case CCupperSW:
      upperSW = 1;
      lowerSW = 0;
      renderCurrentPatchPage();
      break;

    case CClowerSW:
      upperSW = 0;
      lowerSW = 1;
      renderCurrentPatchPage();
      break;

    case CCfilterenvLinLogSW:
      panelData[P_filterLogLin] = value;   // must stay - not in the knob table
      drawFilterEnv();
      break;

    case CCampenvLinLogSW:
      panelData[P_ampLogLin] = value;   // must stay - not in the knob table
      drawAmpEnv();
      break;

    case CCenv2_env3_adsr:
      panelData[P_env2_env3_adsr] = value;
      drawFilterEnv();
      drawAmpEnv();
      break;

  }
}

void loop(void) {
  MIDI.read(MIDI_CHANNEL_OMNI);
}