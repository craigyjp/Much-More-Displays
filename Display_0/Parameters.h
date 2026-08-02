// If your Parameters.h uses different names, change these four lines only.
#define PARP_ONOFF  P_arpStartStop
#define PARP_LATCH  P_arpLatch
#define PARP_RANGE  P_arpRange
#define PARP_MODE   P_arpMode

#define BTN_H       56
#define BTN_R        6
#define LED_SHELF   14   // height of the top LED area; face fills the rest
#define COL_BTN_OFF     0x2124   // unlit fill
#define COL_BTN_OFF_ED  0x8410   // unlit border

#define COL_CAP_FACE   0x0000   // black cap face (was grey 0xC618)
#define COL_CAP_STEP   0x0000   // lower face also black
#define COL_CAP_BORDER 0x8410   // grey outline so the cap reads against black
#define COL_CAP_HI     0xAD55   // step lip - a lighter grey line
#define COL_HDR        0xAD55

#define COL_LED_ON     0xF800   // lit LED (bright red)
#define COL_LED_HALO   0xFB2C   // lit LED highlight dot
#define COL_LED_OFF    0x4000   // unlit LED (dark maroon)
#define COL_LED_OFF_RIM 0x6000  // unlit LED rim

int maxSectionWidth = 0;

char filterDisplay[30];
char lfoDisplay[30];
char buf1[30];
char buf2[30];
char buf3[30];
char buf4[30];
char buf5[30];

int playMode = 0;
int readRes = 1023;
int parameterGroup = 0;

//Values below are just for initialising and will be changed when synth is initialised to current panel controls & EEPROM settings
int i = 0;

int resolutionFrig = 3;
boolean recallPatchFlag = false;
int setCursorPos = 0;

String patchName = INITPATCHNAME;

int NotePriority = 0;
int ClockSource = 0;
int chordHoldSW = 0;
int upperSW = 0;
int lowerSW = 1;

int returnvalue = 0;

int upperData[105];
int lowerData[105];
int panelData[105];

#define P_sysex 0
#define P_LFO2Rate 1
#define P_fmDepth 2
#define P_osc2PW 3
#define P_osc2PWM 4
#define P_osc1PW 5
#define P_osc1PWM 6
#define P_osc1Range 7
#define P_osc2Range 8
#define P_osc2Interval 9
#define P_glideTime 10
#define P_osc2Detune 11
#define P_noiseLevel 12
#define P_osc2SawLevel 13
#define P_osc1SawLevel 14
#define P_osc2PulseLevel 15
#define P_osc1PulseLevel 16
#define P_filterCutoff 17
#define P_filterLFO 18
#define P_filterRes 19
#define P_filterType 20
#define P_modWheelDepth 21
#define P_effectsMix 22
#define P_LFODelayGo 23
#define P_filterEGlevel 24
#define P_LFO1Rate 25
#define P_LFO1Waveform 26
#define P_filterAttack 27
#define P_filterDecay 28
#define P_filterSustain 29
#define P_filterRelease 30
#define P_ampAttack 31
#define P_ampDecay 32
#define P_ampSustain 33
#define P_ampRelease 34
#define P_volumeControl 35
#define P_glideSW 36
#define P_keytrack 37
#define P_filterPoleSW 38
#define P_filterLoop 39
#define P_filterEGinv 40
#define P_filterVel 41
#define P_vcaLoop 42
#define P_vcaVel 43
#define P_vcaGate 44
#define P_lfoAlt 45
#define P_filterLevel1 46
#define P_filterLevel2 47
#define P_monoMulti 48
#define P_modWheelLevel 49
#define P_PitchBendLevel 50
#define P_amDepth 51
#define P_sync 52
#define P_effectPot1 53
#define P_effectPot2 54
#define P_effectPot3 55
#define P_oldampAttack 56
#define P_oldampDecay 57
#define P_oldampSustain 58
#define P_oldampRelease 59
#define P_AfterTouchDest 60
#define P_filterLogLin 61
#define P_ampLogLin 62
#define P_osc2TriangleLevel 63
#define P_osc1SubLevel 64
#define P_keyboardMode 65
#define P_LFO1Delay 66
#define P_effectNum 67
#define P_effectBank 68
#define P_LFO1Slope 69
#define P_LFO3Rate 70
#define P_lfoMultiplier 71
#define P_NotePriority 72
#define P_keytrackSW 73
#define P_ATDepth 74
#define P_pitchAttack 75
#define P_pitchDecay 76
#define P_pitchSustain 77
#define P_pitchRelease 78
#define P_LFO3Delay 79
#define P_osc1sawDetune 80
#define P_osc1sawCount 81
#define P_arpRate 82
#define P_LFO3Waveform 83
#define P_LFO2Waveform 84
#define P_osc2envDepth 85
#define P_noiseSrc 86
#define P_lfo1retrig 87
#define P_osc1envPWM 88
#define P_osc2envPWM 89
#define P_dco_at_SW 90
#define P_filter_at_SW 91
#define P_arpStartStop 92
#define P_arpRange 93
#define P_arpMode 94
#define P_arpLatch 95
#define P_vcfATDepth 96
#define P_fx_Bypass 97
#define P_unisonDetune 98
#define P_dualDetune 99
#define P_env2_env3_adsr 100
#define P_env1_adsr 101
#define P_env1_punch 102
#define P_env2_punch 103
#define P_env3_punch 104
