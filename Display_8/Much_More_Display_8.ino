#include "SPI.h"
#include "TFT_eSPI.h"
#include <MIDI.h>
#include "Constants.h"
#include "Parameters.h"
#include "MidiCC.h"

#include "Hardware.h"


// Use hardware SPI
TFT_eSPI tft = TFT_eSPI();

// Each dial is rendered off-screen then pushed in one go. This removes the
// anti-aliasing residue you get from repainting arcs in place, and removes
// flicker as a side effect. 64 x 64 x 2 bytes = 8KB.
#define SPR_SIZE  64
#define SPR_C     (SPR_SIZE / 2)
TFT_eSprite dial = TFT_eSprite(&tft);

// Screen is used in landscape (rotation 1) = 320 x 240
int screenWidth = 320;
int screenHeight = 240;

// ---------------------------------------------------------------------------
//  Values are native MIDI, 0..127. panelData[] holds them unscaled.
// ---------------------------------------------------------------------------
#define VALUE_MAX   127
#define VALUE_MID    64   // the centre detent position

// ---------------------------------------------------------------------------
//  Dial geometry (4 columns x 3 rows = 12 slots)
// ---------------------------------------------------------------------------
#define NUM_KNOBS   12

// Slot types
#define KNOB_NORMAL 0     // fills from the left stop  (level, amount)
#define KNOB_CENTRE 1     // fills outward from 12 o'clock  (PW, detune, pan)
#define KNOB_EMPTY  255   // slot draws nothing

#define ARC_R       29    // outer radius of the value arc
#define ARC_IR      24    // inner radius of the value arc
#define BODY_R      21    // radius of the solid knob body
#define PTR_IN      16    // pointer starts here
#define PTR_OUT     20    // pointer ends here
#define LABEL_OFF   33    // label distance below the centre

// Pot sweep, in TFT_eSPI arc angles: 0 = 6 o'clock, increasing clockwise.
#define ARC_START   45
#define ARC_END     315
#define ARC_SWEEP   (ARC_END - ARC_START)
#define ARC_MID     ((ARC_START + ARC_END) / 2)   // 180 = 12 o'clock

// Colours
#define COL_BODY    0x2124   // dark grey knob face
#define COL_RIM     0x4A49   // slightly lighter rim
#define COL_TRACK   0x39E7   // unfilled part of the arc
#define COL_TICK    0xC618   // centre detent mark

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
  uint8_t     cc;
  uint8_t     param;    // index into panelData[]
  const char* label;
  uint8_t     type;     // KNOB_NORMAL / KNOB_CENTRE / KNOB_EMPTY
  int16_t     dispMin;  // reading at MIDI 0
  int16_t     dispMax;  // reading at MIDI 127
  const char* suffix;   // unit text, "" for none
  int16_t     cx;       // set automatically
  int16_t     cy;       // set automatically
};

#define KNOB(c, p, l)                      { c, p, l, KNOB_NORMAL,   0, 127, "",  0, 0 }
#define KNOB_RANGE(c, p, l, lo, hi, u)     { c, p, l, KNOB_NORMAL,  lo,  hi,  u,  0, 0 }
#define CENTRE(c, p, l)                    { c, p, l, KNOB_CENTRE, -64,  63, "",  0, 0 }
#define CENTRE_RANGE(c, p, l, lo, hi, u)   { c, p, l, KNOB_CENTRE,  lo,  hi,  u,  0, 0 }
#define EMPTY_SLOT                         { 0, 0, "", KNOB_EMPTY,   0,   0, "",  0, 0 }

// ---- DCO1 page ------------------------------------------------------------
Knob knobs[NUM_KNOBS] = {
  KNOB(         CCLFO1Rate,        P_LFO1Rate,        "RATE" ),
  KNOB(         CCLFO1Slope,       P_LFO1Slope,       "SLPE" ),
  KNOB(         CCLFO1Delay,       P_LFO1Delay,       "DLY" ),
  EMPTY_SLOT,

  KNOB(         CCLFO2Rate,        P_LFO2Rate,        "RATE" ),
  EMPTY_SLOT,
  EMPTY_SLOT,
  EMPTY_SLOT,

  KNOB(         CCLFO3Rate,        P_LFO3Rate,        "RATE" ),
  KNOB(         CCLFO3Delay,       P_LFO3Delay,       "DLY" ),
  KNOB_RANGE(   CCLFO3Waveform,    P_LFO3Waveform,    "WAVE", 1, 16, "" ),
  EMPTY_SLOT,

};

//MIDI 5 Pin DIN
MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

uint16_t getActiveColor() {
  if (upperSW) return TFT_CYAN;
  if (lowerSW) return TFT_YELLOW;
  return TFT_DARKGREY;   // neither set - shouldn't happen
}

// Position of this value on the dial: 0.0 = anticlockwise stop, 1.0 = clockwise.
// Centre dials map each half separately so MIDI 64 lands exactly on 0.5.
float knobFraction(int i, int value) {
  if (value < 0)         value = 0;
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
  *dy =  cos(r);
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
  dial.drawWedgeLine(SPR_C + PTR_IN  * dx, SPR_C + PTR_IN  * dy,
                     SPR_C + PTR_OUT * dx, SPR_C + PTR_OUT * dy,
                     1.4, 2.2, colour, COL_BODY);
}

// The detent mark at 12 o'clock, drawn across the arc band.
void sprCentreTick(uint16_t colour) {
  float dx, dy;
  arcAngleToVector(ARC_MID, &dx, &dy);
  dial.drawLine(SPR_C + (int)round((ARC_IR - 1) * dx),
                SPR_C + (int)round((ARC_IR - 1) * dy),
                SPR_C + (int)round((ARC_R  + 1) * dx),
                SPR_C + (int)round((ARC_R  + 1) * dy), colour);
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

  int value = panelData[knobs[i].param];   // 0..127
  if (value < 0)         value = 0;
  if (value > VALUE_MAX) value = VALUE_MAX;

  uint16_t active = getActiveColor();
  float frac   = knobFraction(i, value);
  int   fillTo = ARC_START + (int)lround(frac * ARC_SWEEP);

  dial.fillSprite(TFT_BLACK);

  // Full track first, then the active portion painted over it. Because the
  // buffer is cleared every frame, nothing can accumulate.
  sprArc(ARC_START, ARC_END, COL_TRACK);

  if (knobs[i].type == KNOB_CENTRE) {
    if (fillTo >= ARC_MID) sprArc(ARC_MID, fillTo,  active);
    else                   sprArc(fillTo,  ARC_MID, active);
    sprCentreTick(COL_TICK);
  } else {
    sprArc(ARC_START, fillTo, active);
  }

  dial.fillSmoothCircle(SPR_C, SPR_C, BODY_R, COL_BODY, TFT_BLACK);
  dial.drawCircle(SPR_C, SPR_C, BODY_R, COL_RIM);

  sprPointer((float)fillTo, active);

  // Reading, remapped onto this slot's display range
  long span  = (long)knobs[i].dispMax - (long)knobs[i].dispMin;
  int  shown = knobs[i].dispMin + (int)lround(frac * span);

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

void renderCurrentPatchPage() {
  tft.fillScreen(TFT_BLACK);
  for (int i = 0; i < NUM_KNOBS; i++) {
    drawKnobLabel(i);
    drawKnobValue(i);
  }

  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TC_DATUM);
  tft.setCursor(250, 76);
  tft.print("LFO 1");
  tft.setCursor(250, 156);
  tft.print("LFO 2");
  tft.setCursor(250, 236);
  tft.print("LFO 3");

  drawLFO1Waveform(panelData[P_LFO1Waveform]);
  drawLFO2Waveform(panelData[P_LFO2Waveform]);
  drawLFO3Waveform(panelData[P_LFO3Waveform]);

}

// ---------------------------------------------------------------------------
//  Setup / MIDI
// ---------------------------------------------------------------------------

void setup() {

  tft.init();
  tft.setRotation(1);

  SetupHardware();

  tft.fillScreen(TFT_BLACK);

  dial.setColorDepth(16);          // 16-bit needed for anti-aliasing
  dial.createSprite(SPR_SIZE, SPR_SIZE);

  layoutKnobs();

  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.setHandleControlChange(myConvertControlChange);
  MIDI.turnThruOn(midi::Thru::Mode::Off);

  renderCurrentPatchPage();
}

void myConvertControlChange(byte channel, byte number, byte value) {

  if (channel == 1) {
    myControlChange(channel, number, value);   // native 0..127, no scaling
  }

  if (channel == 2) {
    myLEDupdate(channel, number, value);
  }
}

void drawLFO1Waveform(uint8_t wave) {
  tft.fillRect(WAVE1_BOX_X, WAVE1_BOX_Y, WAVE1_BOX_W, WAVE1_BOX_H, TFT_BLACK);

  uint8_t type;
  switch (wave) {
    case 0:  type = WAVE_TRI; break;
    case 1:  type = WAVE_SQR; break;
    case 2:  type = WAVE_SAW; break;
    default: return;                  // unknown value, leave the box empty
  }

  drawWave(WAVE1_BOX_X, WAVE1_BOX_Y, WAVE1_BOX_W, WAVE1_BOX_H,
           type, getActiveColor(), 2);
}

void drawLFO2Waveform(uint8_t wave) {
  tft.fillRect(WAVE2_BOX_X, WAVE2_BOX_Y, WAVE2_BOX_W, WAVE2_BOX_H, TFT_BLACK);

  uint8_t type;
  switch (wave) {
    case 0:  type = WAVE_TRI; break;
    case 1:  type = WAVE_SQR; break;
    case 2:  type = WAVE_SAW; break;
    default: return;                  // unknown value, leave the box empty
  }

  drawWave(WAVE2_BOX_X, WAVE2_BOX_Y, WAVE2_BOX_W, WAVE2_BOX_H,
           type, getActiveColor(), 2);
}

// Fixed "random" pattern so the shape is stable every redraw
static const float randLv[9] = { -0.2, 0.8, -0.6, 0.3, 1.0, -0.9, 0.5, -0.4, -0.2 };

// Returns -1.0 .. +1.0 for phase p (0.0 .. 1.0). Curved shapes only.
float waveSample(uint8_t type, float p) {
  switch (type) {
    case WAVE3_SINE:     return sin(TWO_PI * p);
    case WAVE3_SWEEP:    return 2.0 * (2.0 * p - 1.0) * (2.0 * p - 1.0) - 1.0;
    case WAVE3_LUMPS:    return 1.0 - 2.0 * (2.0 * p - 1.0) * (2.0 * p - 1.0);
    case WAVE3_SINE_OCT: return 0.65 * sin(TWO_PI * p) + 0.35 * sin(TWO_PI * 2.0 * p);
    case WAVE3_SINE_3RD: return 0.65 * sin(TWO_PI * p) + 0.35 * sin(TWO_PI * 3.0 * p);
    case WAVE3_SINE_4TH: return 0.65 * sin(TWO_PI * p) + 0.35 * sin(TWO_PI * 4.0 * p);
    case WAVE3_RAMP_OCT: return 0.6 * (2.0 * p - 1.0)
                             + 0.4 * (2.0 * fmod(p * 2.0, 1.0) - 1.0);
  }
  return 0.0;
}

// Plots a curved shape column by column, joining points with short lines.
void wavePlot(int x, int y, int w, int h, uint8_t type, uint16_t colour, int yoff) {
  int top = y + 2;
  int bot = y + h - 3;
  int mid = (top + bot) / 2;
  int amp = (bot - top) / 2;

  int px = x;
  int py = mid - (int)lround(amp * waveSample(type, 0.0));
  for (int i = 1; i < w; i++) {
    int cx = x + i;
    int cy = mid - (int)lround(amp * waveSample(type, i / (float)w));
    tft.drawLine(px, py + yoff, cx, cy + yoff, colour);
    px = cx;
    py = cy;
  }
}

// Draws a waveform inside the box x,y,w,h. Set thick to 2 for a bolder line.
void drawWave3(int x, int y, int w, int h, uint8_t type, uint16_t colour, int thick) {
  int top = y + 2;
  int bot = y + h - 3;
  int mid = (top + bot) / 2;
  int amp = (bot - top) / 2;
  int span = bot - top + 1;

  for (int t = 0; t < thick; t++) {
    switch (type) {

      case WAVE3_RAMP_UP:
        tft.drawLine(x, bot + t, x + w / 2, top + t, colour);
        tft.drawFastVLine(x + w / 2 + t, top, span, colour);
        tft.drawLine(x + w / 2, bot + t, x + w - 1, top + t, colour);
        break;

      case WAVE3_RAMP_DOWN:
        tft.drawLine(x, top + t, x + w / 2, bot + t, colour);
        tft.drawFastVLine(x + w / 2 + t, top, span, colour);
        tft.drawLine(x + w / 2, top + t, x + w - 1, bot + t, colour);
        break;

      case WAVE3_PULSE:
        tft.drawFastVLine(x + t,             top, span, colour);
        tft.drawFastHLine(x,                 top + t, w / 2, colour);
        tft.drawFastVLine(x + w / 2 + t,     top, span, colour);
        tft.drawFastHLine(x + w / 2,         bot + t, w / 2, colour);
        tft.drawFastVLine(x + w - 1 - t,     top, span, colour);
        break;

      case WAVE3_TRI: {
        int q = w / 4;
        tft.drawLine(x,         mid + t, x + q,     top + t, colour);
        tft.drawLine(x + q,     top + t, x + 3 * q, bot + t, colour);
        tft.drawLine(x + 3 * q, bot + t, x + w - 1, mid + t, colour);
        break;
      }

      case WAVE3_QUAD_RAMP: {
        int seg = w / 4;
        for (int k = 0; k < 4; k++) {
          int sx = x + k * seg;
          tft.drawLine(sx, bot + t, sx + seg - 1, top + t, colour);
          if (k < 3) tft.drawFastVLine(sx + seg - 1 + t, top, span, colour);
        }
        break;
      }

      case WAVE3_QUAD_PULSE: {
        int seg = w / 6;
        int pw  = seg / 3; if (pw < 2) pw = 2;
        for (int k = 0; k < 6; k++) {
          int sx = x + k * seg;
          tft.drawFastVLine(sx + t,      top, span, colour);
          tft.drawFastHLine(sx,          top + t, pw, colour);
          tft.drawFastVLine(sx + pw + t, top, span, colour);
          tft.drawFastHLine(sx + pw,     bot + t, seg - pw, colour);
        }
        break;
      }

      case WAVE3_TRI_STEP: {
        const int lv[7] = { 0, 1, 2, 3, 2, 1, 0 };
        int seg = w / 7;
        int prevY = bot;
        for (int k = 0; k < 7; k++) {
          int sx = x + k * seg;
          int ly = bot - (bot - top) * lv[k] / 3;
          tft.drawFastHLine(sx, ly + t, seg, colour);
          if (k > 0) tft.drawFastVLine(sx + t, min(prevY, ly), abs(ly - prevY) + 1, colour);
          prevY = ly;
        }
        break;
      }

      case WAVE3_RAND_LEVELS: {
        int seg = w / 8;
        int prevY = 0;
        for (int k = 0; k < 8; k++) {
          int sx = x + k * seg;
          int ly = mid - (int)lround(amp * randLv[k]);
          tft.drawFastHLine(sx, ly + t, seg, colour);
          if (k > 0) tft.drawFastVLine(sx + t, min(prevY, ly), abs(ly - prevY) + 1, colour);
          prevY = ly;
        }
        break;
      }

      case WAVE3_RAND_SLOPES: {
        int seg = w / 8;
        for (int k = 0; k < 8; k++) {
          int y0 = mid - (int)lround(amp * randLv[k]);
          int y1 = mid - (int)lround(amp * randLv[k + 1]);
          tft.drawLine(x + k * seg, y0 + t, x + (k + 1) * seg, y1 + t, colour);
        }
        break;
      }

      default:   // all the curved shapes
        wavePlot(x, y, w, h, type, colour, t);
        break;
    }
  }
}

void drawLFO3Waveform(uint8_t wave) {
  tft.fillRect(WAVE3_BOX_X, WAVE3_BOX_Y, WAVE3_BOX_W, WAVE3_BOX_H, TFT_BLACK);
  if (wave > 15) return;
  drawWave3(WAVE3_BOX_X, WAVE3_BOX_Y, WAVE3_BOX_W, WAVE3_BOX_H,
           wave, getActiveColor(), 2);
}

void drawWave(int x, int y, int w, int h, uint8_t type, uint16_t colour, int thick) {
  int top = y + 2;
  int bot = y + h - 3;
  int mid = y + h / 2;
  int q   = w / 4;

  for (int t = 0; t < thick; t++) {
    switch (type) {

      case WAVE_TRI:
        tft.drawLine(x,         mid + t, x + q,     top + t, colour);
        tft.drawLine(x + q,     top + t, x + 3 * q, bot + t, colour);
        tft.drawLine(x + 3 * q, bot + t, x + w,     mid + t, colour);
        break;

      case WAVE_SAW:
        tft.drawLine(x,         bot + t, x + w / 2, top + t, colour);
        tft.drawFastVLine(x + w / 2 + t, top, bot - top + 1, colour);
        tft.drawLine(x + w / 2, bot + t, x + w,     top + t, colour);
        break;

      case WAVE_SQR:
        tft.drawFastVLine(x + t,             top, bot - top + 1, colour);
        tft.drawFastHLine(x,                 top + t, w / 2, colour);
        tft.drawFastVLine(x + w / 2 + t,     top, bot - top + 1, colour);
        tft.drawFastHLine(x + w / 2,         bot + t, w / 2, colour);
        tft.drawFastVLine(x + w - 1 - t,     top, bot - top + 1, colour);
        break;
    }
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

    case CCLFO1Waveform:
      drawLFO1Waveform(value);
      break;

    case CCLFO2Waveform:
      drawLFO2Waveform(value);
      break;

    case CCLFO3Waveform:
      drawLFO3Waveform(value);
      break;

  }
}

void loop(void) {
  MIDI.read(MIDI_CHANNEL_OMNI);
}