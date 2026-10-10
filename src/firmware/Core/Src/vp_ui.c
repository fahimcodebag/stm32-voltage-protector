/* On-device user interface: two push buttons and an LCD1602 on an I2C
 * PCF8574 backpack.
 *
 *   MODE   (PB10)  RUN -> UV -> OV -> OC -> RESTORE -> RUN
 *   ADJUST (PB11)  step the selected setting up, wrapping to its
 *                  minimum; hold to auto-repeat
 *
 * Edits apply live, exactly like a host SET command. Leaving setup (MODE back
 * to RUN, or 6 s without a press) saves to flash if anything changed, so the
 * device and the host share one settings path.
 *
 * The LCD is driven from a shadow buffer, one I2C transfer (~0.5 ms at
 * ~100 kHz) per millisecond tick, so redrawing the screen never stalls the
 * 1 kHz sampling for long. If the LCD does not ACK at boot the UI carries on
 * without it and the buttons still work, with feedback over the UART.
 *
 * I2C is bit-banged on PB6/PB7 rather than using the I2C1 peripheral. In
 * Proteus the I2C1 model warned "Access to I2C module while CR1.SWRST = 1"
 * during HAL_I2C_Init and every transfer then timed out (HAL_I2C_ERROR_TIMEOUT)
 * with the bus held low. A software master sidesteps the simulator model and
 * the F1 I2C errata alike, on the same pins and wiring.
 */
#include "main.h"
#include "vp.h"
#include <stdio.h>
#include <string.h>

/* ---- PCF8574 -> HD44780 wiring (common backpack layout) ----------------- */
#define LCD_ADDR     (0x20u << 1)   /* PCF8574, A2..A0 = GND (PCF8574A: 0x38) */
#define LCD_RS       0x01u          /* P0 */
#define LCD_RW       0x02u          /* P1, held low: write only */
#define LCD_EN       0x04u          /* P2 */
#define LCD_BL       0x08u          /* P3, backlight */
                                    /* P4..P7 = D4..D7 */

#define SCL_PIN      GPIO_PIN_6     /* PB6, open drain, external 4k7 pull-up */
#define SDA_PIN      GPIO_PIN_7     /* PB7, open drain, external 4k7 pull-up */

/* Button pins, as assigned in the CubeMX design (.ioc). */
#define BTN_PORT         MODE_GPIO_Port
#define BTN_MODE_PIN     MODE_Pin       /* PB10 */
#define BTN_ADJ_PIN      ADJUST_Pin     /* PB11 */

/* Button polarity. 1: switch to 3V3 with an external pull-down (pressed =
   high). 0: switch to GND with an external pull-up (pressed = low). */
#define BTN_ACTIVE_HIGH  1

/* Internal bias resistor on the button pins. Off: the pins rely on the
   external 10k bias resistor (or a driven source, as in the simulation). */
#define BTN_INTERNAL_PULL 0

/* 10 ms debounce: covers typical tactile-switch bounce (<5 ms), and is short
   enough that a normal click registers in Proteus, which runs ~5x slower than
   real time (a 30 ms filter swallowed ordinary clicks there). */
#define BTN_SCAN_MS      5u
#define BTN_STABLE       2u
#define BTN_REPEAT_DELAY 600u
#define BTN_REPEAT_MS    150u
#define UI_DRAW_MS       200u
#define UI_IDLE_MS       6000u      /* leave setup (and save) after 6 s idle */
#define LCD_RETRY_MS     2000u

typedef enum { F_RUN = 0, F_UV, F_OV, F_OC, F_RESTORE, F_COUNT } ui_field_t;

typedef struct {
  GPIO_TypeDef *port;
  uint16_t pin;
  uint8_t  cnt;
  bool     down;                    /* debounced state */
  uint32_t down_ms;
  uint32_t rep_ms;
} button_t;

static button_t btn_mode   = { BTN_PORT, BTN_MODE_PIN };
static button_t btn_adjust = { BTN_PORT, BTN_ADJ_PIN };

static ui_field_t field;
static bool       dirty;            /* settings changed since entering setup */
static uint32_t   last_press_ms, last_scan_ms, last_draw_ms, last_lcd_ms;
static uint32_t   lcd_retry_ms;

static bool    lcd_ok;
static char    want[2][16], shown[2][16];
static uint8_t cur_row, cur_col;    /* where the LCD cursor actually is */
static bool    cur_valid;

/* ---- software I2C master (open drain: 1 = release, 0 = pull low) ------- */

static uint32_t i2c_delay_loops;
static bool     i2c_blind;          /* pins cannot be read back (see i2c_init) */

static void i2c_delay(void)        /* ~5 us: half a 100 kHz bit period */
{
  for (volatile uint32_t i = 0; i < i2c_delay_loops; i++) { }
}

/* Lines are never driven high. ODR stays 0; "1" switches the pin to a
   floating input and the 4k7 pull-up raises it, "0" switches it to an output
   driving low. Proteus's open-drain mode drove PB7 high against the slave's
   ACK ("logic contention on U1_GPIO_B7"); direction switching cannot.
   CRL nibbles: PB6 = [27:24], PB7 = [31:28]; 0x4 input floating, 0x3 output. */
static inline void pin_release(uint32_t shift, bool hi)
{
  uint32_t crl = GPIOB->CRL & ~(0xFu << shift);
  GPIOB->CRL = crl | ((hi ? 0x4u : 0x3u) << shift);
}
static inline void scl(bool hi) { pin_release(24, hi); }
static inline void sda(bool hi) { pin_release(28, hi); }
static inline bool sda_in(void) { return (GPIOB->IDR & SDA_PIN) != 0; }
static inline bool scl_in(void) { return (GPIOB->IDR & SCL_PIN) != 0; }

/* Fails if the bus is not idle-high. Without this check a line stuck low
   (e.g. a pull-up that has lost its supply) reads as an ACK on every byte. */
static bool i2c_start(void)
{
  uint8_t n = 0;
  sda(1); scl(1); i2c_delay();
  if (i2c_blind) { sda(0); i2c_delay(); scl(0); i2c_delay(); return true; }
  /* allow up to ~20 delay periods (~100 us) for the pull-ups to win */
  while (!(sda_in() && scl_in())) {
    if (++n >= 20u) return false;
    i2c_delay();
  }
  sda(0); i2c_delay();
  scl(0); i2c_delay();
  return true;
}

static void i2c_stop(void)
{
  sda(0); i2c_delay();
  scl(1); i2c_delay();
  sda(1); i2c_delay();
}

static bool i2c_write(uint8_t b)    /* true if the slave ACKed */
{
  bool ack;
  for (uint8_t m = 0x80; m; m >>= 1) {
    sda((b & m) != 0); i2c_delay();
    scl(1); i2c_delay();
    scl(0);
  }
  sda(1); i2c_delay();              /* release SDA for the ACK bit */
  scl(1); i2c_delay();
  ack = i2c_blind || !sda_in();
  scl(0); i2c_delay();
  return ack;
}

static bool i2c_send(const uint8_t *buf, uint8_t n)
{
  bool ok;
  if (!i2c_start()) return false;
  ok = i2c_write(LCD_ADDR);
  for (uint8_t i = 0; ok && i < n; i++) ok = i2c_write(buf[i]);
  i2c_stop();
  return ok;
}

static void i2c_init(void)
{
  GPIO_InitTypeDef g = {0};

  /* main() still runs CubeMX's MX_I2C1_Init(); take the pins back from the
     peripheral and stop its clock so only this master drives the bus. */
  __HAL_RCC_I2C1_FORCE_RESET();     /* clear the peripheral, incl. SWRST */
  __HAL_RCC_I2C1_RELEASE_RESET();
  __HAL_RCC_I2C1_CLK_DISABLE();
  g.Pin   = SCL_PIN | SDA_PIN;
  g.Mode  = GPIO_MODE_INPUT;        /* released: pull-ups hold the bus high */
  g.Pull  = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &g);
  GPIOB->BSRR = (uint32_t)(SCL_PIN | SDA_PIN) << 16;  /* ODR = 0 for "low" */

  /* ~4 CPU cycles per loop pass; scale so the bus stays near 100 kHz at
     8 MHz (Proteus) and 72 MHz (hardware) alike */
  i2c_delay_loops = SystemCoreClock / 1000000u * 5u / 4u;
  if (!i2c_delay_loops) i2c_delay_loops = 1;

  /* With both lines released, the pull-ups must make them read high. If they
     read low, either the bus is shorted or the inputs cannot be read: in
     Proteus PB6/PB7 read 0 here while the nets were visibly high and the LCD
     was receiving data. Fall back to write-only: no ACK or stuck-bus checks. */
  HAL_Delay(1);
  i2c_blind = !scl_in() && !sda_in();
  if (i2c_blind) {
    vp_comms_printf("UI,I2C lines read low when released: LCD write-only, "
                    "no ACK checking\r\n");
    return;
  }

  /* free a slave left mid-byte: up to 9 clocks until SDA is released */
  for (uint8_t i = 0; i < 9 && !sda_in(); i++) {
    scl(0); i2c_delay(); scl(1); i2c_delay();
  }
  i2c_stop();
}

/* ---- LCD low level ----------------------------------------------------- */

static bool lcd_nibble(uint8_t nib, uint8_t rs)
{
  uint8_t b[2];
  b[0] = (uint8_t)((nib << 4) | LCD_BL | LCD_EN | rs);
  b[1] = (uint8_t)((nib << 4) | LCD_BL | rs);
  return i2c_send(b, 2);
}

/* One HD44780 byte as two nibbles, each strobed on EN, in a single transfer */
static bool lcd_byte(uint8_t v, uint8_t rs)
{
  uint8_t b[4];
  b[0] = (uint8_t)((v & 0xF0u) | LCD_BL | LCD_EN | rs);
  b[1] = (uint8_t)((v & 0xF0u) | LCD_BL | rs);
  b[2] = (uint8_t)((v << 4) | LCD_BL | LCD_EN | rs);
  b[3] = (uint8_t)((v << 4) | LCD_BL | rs);
  if (!i2c_send(b, 4)) {
    if (lcd_ok) vp_comms_printf("UI,LCD I2C error, retrying\r\n");
    lcd_ok = false;                 /* vp_ui_poll re-initialises it later */
    return false;
  }
  return true;
}

/* HD44780 4-bit initialisation. The 0x3,0x3,0x3,0x2 nibble sequence also
 * re-synchronises a controller left half way through a byte by a failed
 * transfer, so the same routine recovers the display after a bus fault.
 * Blocks ~60 ms cold (boot) and ~10 ms warm (recovery). */
static bool lcd_start(bool cold)
{
  if (!i2c_send(NULL, 0)) return false;   /* address-only probe */

  lcd_ok = true;                    /* lcd_byte clears this on failure */
  HAL_Delay(cold ? 50u : 5u);
  if (!lcd_nibble(0x3, 0)) return lcd_ok = false;
  HAL_Delay(5);
  if (!lcd_nibble(0x3, 0)) return lcd_ok = false;
  HAL_Delay(1);
  if (!lcd_nibble(0x3, 0)) return lcd_ok = false;
  HAL_Delay(1);
  if (!lcd_nibble(0x2, 0)) return lcd_ok = false;
  HAL_Delay(1);
  lcd_byte(0x28, 0);                /* 4-bit, 2 lines, 5x8 */
  lcd_byte(0x08, 0);                /* display off */
  lcd_byte(0x01, 0); HAL_Delay(2);  /* clear (1.52 ms) */
  lcd_byte(0x06, 0);                /* entry mode: increment */
  lcd_byte(0x0C, 0);                /* display on, no cursor */

  memset(shown, ' ', sizeof shown); /* screen is blank: redraw everything */
  cur_valid = false;
  return lcd_ok;
}

static void lcd_init(void)
{
  i2c_init();
  if (!lcd_start(true)) {
    /* both lines released here; read now and again 1 ms later, to separate
       "line really held low" from "input reads lag the pin" */
    unsigned scl0 = scl_in(), sda0 = sda_in();
    HAL_Delay(1);
    vp_comms_printf("UI,I2C probe 0x%02X failed: SCL=%u SDA=%u, after 1ms SCL=%u SDA=%u, CRL=%08lX\r\n",
                    (unsigned)(LCD_ADDR >> 1), scl0, sda0,
                    (unsigned)scl_in(), (unsigned)sda_in(),
                    (unsigned long)GPIOB->CRL);
  }
}

/* Send at most one transfer: either move the cursor or write one changed
 * character. Called once per millisecond tick. */
static void lcd_step(void)
{
  for (uint8_t i = 0; i < 32; i++) {
    uint8_t r = i / 16u, c = i % 16u;
    if (want[r][c] == shown[r][c]) continue;

    if (!cur_valid || cur_row != r || cur_col != c) {
      if (lcd_byte((uint8_t)(0x80u | (r ? 0x40u : 0x00u) | c), 0)) {
        cur_row = r; cur_col = c; cur_valid = true;
      }
      return;
    }
    if (lcd_byte((uint8_t)want[r][c], LCD_RS)) {
      shown[r][c] = want[r][c];
      if (++cur_col >= 16u) cur_valid = false;   /* no wrap to line 2 */
    }
    return;
  }
}

static void set_line(uint8_t row, const char *s)
{
  size_t n = strlen(s);
  memset(want[row], ' ', 16);
  memcpy(want[row], s, n > 16 ? 16 : n);
}

/* ---- screens ----------------------------------------------------------- */

static void draw(void)
{
  char l[24];
  /* clamp to what fits on 16 columns: 999 V, 99.99 A */
  uint32_t v  = vp_state.v_mv / 1000u;
  uint32_t ia = vp_state.i_ma / 10u;
  if (v > 999u)   v  = 999u;
  if (ia > 9999u) ia = 9999u;

  switch (field) {
  case F_RUN:
    snprintf(l, sizeof l, "%3uV %2u.%02uA %s", (unsigned)v,
             (unsigned)(ia / 100u), (unsigned)(ia % 100u),
             vp_state.connected ? " ON" : "OFF");
    set_line(0, l);
    if (vp_state.fault != VP_FAULT_NONE) {
      /* the reading captured at the trip stays up until reconnect, so even
         a 3-cycle surge leaves its measured value on screen */
      uint32_t tv = vp_state.trip_v  > 999u   ? 999u   : vp_state.trip_v;
      uint32_t ta = vp_state.trip_ma / 10u;
      if (ta > 9999u) ta = 9999u;
      if (vp_state.fault == VP_FAULT_OVERCURRENT)
        snprintf(l, sizeof l, "TRIP OC %u.%02uA", (unsigned)(ta / 100u),
                 (unsigned)(ta % 100u));
      else
        snprintf(l, sizeof l, "TRIP %s %uV",
                 vp_state.fault == VP_FAULT_UNDERVOLT ? "UV" : "OV",
                 (unsigned)tv);
    } else {
      snprintf(l, sizeof l, "OK  %u-%uV", vp_cfg.uv_trip_v, vp_cfg.ov_trip_v);
    }
    set_line(1, l);
    break;
  case F_UV:
    set_line(0, "SET UNDER-VOLT");
    snprintf(l, sizeof l, "> %u V", vp_cfg.uv_trip_v);
    set_line(1, l);
    break;
  case F_OV:
    set_line(0, "SET OVER-VOLT");
    snprintf(l, sizeof l, "> %u V", vp_cfg.ov_trip_v);
    set_line(1, l);
    break;
  case F_OC:
    set_line(0, "SET OVER-CURRENT");
    snprintf(l, sizeof l, "> %u.%u A", vp_cfg.oc_trip_ma / 1000u,
             (vp_cfg.oc_trip_ma % 1000u) / 100u);
    set_line(1, l);
    break;
  case F_RESTORE:
    set_line(0, "SET AUTO-RESTORE");
    if (vp_cfg.restore_s) snprintf(l, sizeof l, "> %u s", vp_cfg.restore_s);
    else                  snprintf(l, sizeof l, "> MANUAL");
    set_line(1, l);
    break;
  default:
    break;
  }
}

/* ---- settings editing -------------------------------------------------- */

static uint16_t step_wrap(uint16_t v, uint16_t step, uint16_t lo, uint16_t hi)
{
  v = (uint16_t)(v + step);
  return (v > hi || v < lo) ? lo : v;
}

/* UV and OV limit each other so the healthy band can never be empty */
static void adjust(void)
{
  switch (field) {
  case F_UV:
    vp_cfg.uv_trip_v = step_wrap(vp_cfg.uv_trip_v, 5, 100,
                                 (uint16_t)(vp_cfg.ov_trip_v - 10u));
    break;
  case F_OV:
    vp_cfg.ov_trip_v = step_wrap(vp_cfg.ov_trip_v, 5,
                                 (uint16_t)(vp_cfg.uv_trip_v + 10u), 300);
    break;
  case F_OC:
    vp_cfg.oc_trip_ma = step_wrap(vp_cfg.oc_trip_ma, 500, 1000, 20000);
    break;
  case F_RESTORE:
    vp_cfg.restore_s = step_wrap(vp_cfg.restore_s, 1, 0, 60);
    break;
  default:
    return;                         /* ADJUST does nothing on the run screen */
  }
  dirty = true;
  vp_comms_send_cfg();              /* keep the host view in step */
}

static void leave_setup(void)
{
  field = F_RUN;
  if (dirty) {
    dirty = false;
    vp_comms_printf(vp_settings_save() ? "ACK,SAVE(UI)\r\n" : "ERR,FLASH\r\n");
  }
}

static void next_field(void)
{
  if (field + 1 >= F_COUNT) { leave_setup(); }
  else                      { field = (ui_field_t)(field + 1); }
}

/* ---- buttons ----------------------------------------------------------- */

/* Returns true on a debounced press, and on auto-repeat while held if
 * `repeat` is set. */
static bool button_scan(button_t *b, uint32_t now, bool repeat)
{
  bool raw = (HAL_GPIO_ReadPin(b->port, b->pin) ==
              (BTN_ACTIVE_HIGH ? GPIO_PIN_SET : GPIO_PIN_RESET));

  if (raw == b->down) { b->cnt = 0; }
  else if (++b->cnt >= BTN_STABLE) {
    b->cnt = 0;
    b->down = raw;
    if (raw) { b->down_ms = b->rep_ms = now; return true; }
    return false;
  }

  if (repeat && b->down && now - b->down_ms >= BTN_REPEAT_DELAY
      && now - b->rep_ms >= BTN_REPEAT_MS) {
    b->rep_ms = now;
    return true;
  }
  return false;
}

/* ---- key actions (physical buttons, or KEY commands from the host) ------ */

static void on_mode(uint32_t now)
{
  last_press_ms = now;
  next_field();
  vp_comms_printf("UI,MODE,%u\r\n", (unsigned)field);
  last_draw_ms = now - UI_DRAW_MS;              /* redraw immediately */
}

static void on_adjust(uint32_t now)
{
  last_press_ms = now;
  adjust();
  last_draw_ms = now - UI_DRAW_MS;
}

/* ---- public ------------------------------------------------------------ */

/* Host-injected key press: same path as the buttons, minus the GPIO. Lets a
   test separate "the UI logic misbehaves" from "the button circuit does". */
void vp_ui_key(bool mode)
{
  if (mode) on_mode(HAL_GetTick());
  else      on_adjust(HAL_GetTick());
}

void vp_ui_init(void)
{
  GPIO_InitTypeDef g = {0};

  /* CubeMX configures PB10/PB11 as floating inputs; re-apply that with the
     chosen bias. The released level comes from the external 10k. */
  g.Pin  = BTN_MODE_PIN | BTN_ADJ_PIN;
  g.Mode = GPIO_MODE_INPUT;
  g.Pull = !BTN_INTERNAL_PULL ? GPIO_NOPULL
         : BTN_ACTIVE_HIGH    ? GPIO_PULLDOWN : GPIO_PULLUP;
  HAL_GPIO_Init(BTN_PORT, &g);

  field = F_RUN;
  dirty = false;
  lcd_init();
  vp_comms_printf("UI: LCD %s\r\n", lcd_ok ? "ok" : "not found, buttons only");
  draw();
}

void vp_ui_poll(uint32_t now)
{
  if (now - last_scan_ms >= BTN_SCAN_MS) {
    last_scan_ms = now;

    if (button_scan(&btn_mode, now, false))  on_mode(now);
    if (button_scan(&btn_adjust, now, true)) on_adjust(now);
    if (field != F_RUN && now - last_press_ms >= UI_IDLE_MS) {
      leave_setup();
      vp_comms_printf("UI,MODE,%u\r\n", (unsigned)field);
    }
  }

  if (now - last_draw_ms >= UI_DRAW_MS) {
    last_draw_ms = now;
    draw();
  }

  if (lcd_ok && now != last_lcd_ms) {
    last_lcd_ms = now;
    lcd_step();
  } else if (!lcd_ok && now - lcd_retry_ms >= LCD_RETRY_MS) {
    lcd_retry_ms = now;             /* probe is ~0.1 ms when nothing answers */
    if (lcd_start(false)) vp_comms_printf("UI,LCD recovered\r\n");
  }
}
