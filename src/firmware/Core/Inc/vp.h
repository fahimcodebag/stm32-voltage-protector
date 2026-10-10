/* Voltage Protector - shared declarations.
 * FSMB Phase 1. Target STM32F103C8, CubeMX/HAL project.
 */
#ifndef VP_H
#define VP_H

#include <stdint.h>
#include <stdbool.h>

/* ---- measurement geometry -------------------------------------------- */
#define VP_LINE_HZ        50u                 /* Bangladesh grid            */
#define VP_SAMPLE_HZ      1000u               /* ADC sampling rate          */
#define VP_SAMPLES_CYCLE  (VP_SAMPLE_HZ / VP_LINE_HZ)   /* 20 per cycle     */
#define VP_ADC_FULL       4095u

/* Front end: resistive divider + mid-rail bias maps +/-VP_V_SPAN volts peak
 * onto 0..3V3. Current sense is a Hall sensor with the same bias. These are
 * the nominal design values; they are overridable at runtime (see settings). */
#define VP_V_SPAN_DEF     400                 /* volts peak, full scale     */
#define VP_I_SPAN_DEF     20                  /* amps peak, full scale      */

/* Build variant: 1 = defaults for the mid-rail biased AC front end
   (build_ac.bat), 0 = DC potentiometer demo. See vp_settings_defaults(). */
#ifndef VP_AC_FRONTEND
#define VP_AC_FRONTEND 0
#endif

/* ---- faults ----------------------------------------------------------- */
typedef enum {
  VP_FAULT_NONE = 0,
  VP_FAULT_UNDERVOLT = 1,
  VP_FAULT_OVERVOLT = 2,
  VP_FAULT_OVERCURRENT = 3
} vp_fault_t;

/* ---- persisted settings ----------------------------------------------- */
typedef struct {
  uint32_t magic;
  uint16_t uv_trip_v;        /* under-voltage threshold, Vrms              */
  uint16_t ov_trip_v;        /* over-voltage threshold, Vrms               */
  uint16_t oc_trip_ma;       /* over-current threshold, mA rms             */
  uint16_t restore_s;        /* auto-reconnect delay, seconds (0 = manual) */
  uint16_t v_offset;         /* ADC counts at 0 V  (2048 for AC, 0 for DC) */
  uint16_t v_span;           /* volts at full deflection from offset       */
  uint16_t i_offset;
  uint16_t i_span;           /* amps at full deflection                    */
  uint32_t crc;
} vp_settings_t;

#define VP_MAGIC 0x56503031u   /* "VP01" */

extern vp_settings_t vp_cfg;

void vp_settings_defaults(vp_settings_t *s);
bool vp_settings_load(void);        /* true if valid config came from flash */
bool vp_settings_save(void);

/* ---- live measurement state ------------------------------------------- */
typedef struct {
  uint32_t   v_mv;           /* line voltage, mV rms                        */
  uint32_t   i_ma;           /* load current, mA rms                        */
  bool       connected;      /* relay closed                                */
  vp_fault_t fault;          /* latched fault cause                         */
  uint32_t   trip_ms;        /* HAL tick at which the relay opened          */
  uint32_t   detect_ms;      /* measured detect->open latency, ms           */
  uint32_t   trip_v;         /* V rms when the fault was first seen         */
  uint32_t   trip_ma;        /* mA rms when the fault was first seen        */
} vp_state_t;

extern vp_state_t vp_state;

void vp_protection_init(void);
void vp_protection_sample(void);    /* call at VP_SAMPLE_HZ                 */
bool vp_protection_cycle_ready(void);
void vp_protection_evaluate(void);  /* call once per mains cycle            */
void vp_protection_reset(void);     /* clear latched fault, re-close relay  */

/* ---- host link --------------------------------------------------------- */
void vp_comms_init(void);
void vp_comms_poll(void);           /* dispatch any complete command line   */
void vp_comms_telemetry(void);
void vp_comms_send_cfg(void);
void vp_comms_event(vp_fault_t f, uint32_t rms_v);
void vp_comms_printf(const char *fmt, ...);

/* ---- on-device UI: MODE/ADJUST buttons + I2C LCD1602 ------------------ */
void vp_ui_init(void);
void vp_ui_poll(uint32_t now);      /* call every main-loop pass           */
void vp_ui_key(bool mode);          /* host-injected MODE / ADJUST press    */

#endif /* VP_H */
