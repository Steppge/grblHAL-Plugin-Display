#pragma once

#include <Arduino.h>

// grblHAL system state bits (grbl/system.h)
#define STATE_IDLE          0
#define STATE_ALARM         (1 << 0)
#define STATE_CHECK_MODE    (1 << 1)
#define STATE_HOMING        (1 << 2)
#define STATE_CYCLE         (1 << 3)
#define STATE_HOLD          (1 << 4)
#define STATE_JOG           (1 << 5)
#define STATE_SAFETY_DOOR   (1 << 6)
#define STATE_SLEEP         (1 << 7)
#define STATE_ESTOP         (1 << 8)
#define STATE_TOOL_CHANGE   (1 << 9)

// flags in $DS, see display_link.c
#define DL_FLAG_BLOCKING     0x01
#define DL_FLAG_HOLD_DONE    0x02
#define DL_FLAG_SPINDLE_ON   0x04
#define DL_FLAG_LIMIT_ON     0x08
#define DL_FLAG_HOMING_LOCK  0x10
#define DL_FLAG_FORCE_UNLOCK 0x20
#define DL_FLAG_SD_JOB       0x40
#define DL_FLAG_PROBE_ON     0x80
#define DL_FLAG_PROBE_OK    0x100
#define DL_FLAG_LASER_TEST  0x200

// grblHAL alarm codes
#define ALARM_HARD_LIMIT        1
#define ALARM_SOFT_LIMIT        2
#define ALARM_ABORT_CYCLE       3
#define ALARM_PROBE_INITIAL     4
#define ALARM_PROBE_CONTACT     5
#define ALARM_HOMING_RESET      6
#define ALARM_HOMING_DOOR       7
#define ALARM_HOMING_PULLOFF    8
#define ALARM_HOMING_APPROACH   9
#define ALARM_ESTOP            10
#define ALARM_HOMING_REQUIRED  11
#define ALARM_LIMITS_ENGAGED   12

// grblHAL realtime commands
#define CMD_RESET           0x18
#define CMD_FEED_HOLD       '!'
#define CMD_CYCLE_START     '~'
#define CMD_JOG_CANCEL      0x85
#define CMD_OVR_FEED_RESET  0x90
#define CMD_OVR_FEED_PLUS   0x91
#define CMD_OVR_FEED_MINUS  0x92
#define CMD_OVR_SPIN_RESET  0x99
#define CMD_OVR_SPIN_PLUS   0x9A
#define CMD_OVR_SPIN_MINUS  0x9B
#define CMD_OVR_SPIN_STOP   0x9E

#define LINK_FILES_MAX       100

struct machine_t {
    bool online;            // valid status lines arrive
    uint32_t state;
    uint16_t alarm;
    uint16_t flags;
    float wpos[3], mpos[3];
    float feed, s;
    uint16_t ov_feed, ov_rapid, ov_spindle;
    int16_t pct;            // SD job progress in 0.1 %, -1 = none
    uint8_t mode;           // $32: 0 spindle, 1 laser, 2 lathe
    uint8_t wcs;            // 0 = G54
    float travel[3];        // machine size (mm)
    bool env_valid;         // work envelope known (homed)
    float env_min[3], env_max[3];
    float s_max;            // spindle speed / laser power at 100 %
    char job[48];           // name of the running SD job
    uint32_t updated;       // millis() of the last status line
    bool info;              // $DI has arrived
    char msg[64];           // last message from the controller
    uint16_t msg_seq;       // counts messages
    uint16_t err_code;      // last error (status code) from the controller
    bool err_job;           // it has ended an SD job
    uint16_t err_seq;       // counts errors
    char refused[48];       // command line the controller did not accept
    uint16_t refused_seq;   // counts refused command lines
    int8_t gp_key;          // gamepad button pressed in learn mode (setting value), -1 = no gamepad
    uint16_t gp_key_seq;    // counts reported buttons
    uint8_t gp_flags;       // gamepad state GP_xxx (0 = unknown or no gamepad plugin)
    uint8_t gp_pair;        // seconds left in the pairing window
};

struct file_list_t {
    char name[LINK_FILES_MAX][64];
    int count;              // -1 = no SD card
    bool done;              // list complete
    uint16_t seq;           // counts completed lists
};

// G-code preview of an SD file, see display_link.c (PB/PR/PE)
#define PREVIEW_W_MAX       240
#define PREVIEW_H_MAX       160

struct preview_t {
    bool valid;             // complete bitmap of the requested file
    bool loading;
    char name[64];
    float x0, y0, x1, y1;   // bounds of the burned/cut moves, work coordinates (mm)
    uint16_t w, h;          // bitmap size, row 0 = top (y1)
    uint16_t seq;           // counts completed previews
    uint8_t bits[PREVIEW_W_MAX / 8 * PREVIEW_H_MAX];
};

// grblHAL settings, see display_link.c (SV/SE)
#define SETTINGS_MAX        220

// gamepad plugin: $450 + function holds the button of the function, settings group "Gamepad"
#define GP_SETTING_FIRST    450
#define GP_FUNCTIONS          8
#define GP_GROUP            "Gamepad"

// gamepad state from $GS (display_link.c v1.1)
#define GP_PLUGIN           0x01    // gamepad plugin in the controller firmware
#define GP_ESP32            0x02    // the ESP32 with the Bluetooth receiver answers
#define GP_CONNECTED        0x04    // a controller is connected
#define GP_PAIRING          0x08    // the pairing window is open

enum { Fmt_Bool = 0, Fmt_Bitfield, Fmt_XBitfield, Fmt_RadioButtons, Fmt_AxisMask, Fmt_Integer, Fmt_Decimal };

struct setting_t {
    uint16_t id;
    bool editable;
    uint8_t type;           // grblHAL setting_datatype_t
    char group[24];
    char value[26];
    char unit[12];
    char min[12], max[12];
    char name[42];
    char format[52];        // bit or option names, comma separated
};

struct settings_list_t {
    setting_t *item;        // SETTINGS_MAX entries in PSRAM
    int count;
    bool loading;
    uint16_t seq;           // counts completed lists and single updates
};

extern machine_t mc;
extern file_list_t files;
extern preview_t preview;
extern settings_list_t setlist;

void link_settings (void);              // request all settings
void link_setting (uint16_t id);        // request one setting again

void link_preview (const char *name);   // request the preview of an SD file, "" clears it

void link_begin (void);
void link_poll (void);
void link_rt (uint8_t cmd);
bool link_cmd (const char *line);   // queued, sent again until the controller has accepted it
bool link_idle (void);              // all queued command lines have been accepted
void link_send (const char *body);  // other requests: "FL", "LF,<pct>,<ms>"
