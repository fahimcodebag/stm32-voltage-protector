/* Host link: line-oriented ASCII over USART1 at 115200 8N1.
 * Deliberately human-readable so the Proteus virtual terminal doubles as a
 * debug console during the demo. Protocol is documented in src/pc_app/protocol.md.
 */
#include "main.h"
#include "usart.h"
#include "vp.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RX_MAX 64

static char     rx_line[RX_MAX];
static volatile uint16_t rx_len;
static volatile bool     rx_ready;
static uint8_t  rx_byte;

void vp_comms_printf(const char *fmt, ...)
{
  char buf[96];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n > 0) {
    HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n, HAL_MAX_DELAY);
  }
}

void vp_comms_init(void)
{
  rx_len = 0;
  rx_ready = false;
  HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
}

/* USART1 RX interrupt: assemble one newline-terminated line */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1) {
    char c = (char)rx_byte;
    if (c == '\n' || c == '\r') {
      if (rx_len) { rx_line[rx_len] = '\0'; rx_ready = true; }
    } else if (c == '\b' || c == 0x7F) {
      /* terminals show a corrected line but still send the backspace */
      if (rx_len) rx_len--;
    } else if (rx_len < RX_MAX - 1) {
      rx_line[rx_len++] = c;
    }
    HAL_UART_Receive_IT(&huart1, &rx_byte, 1);
  }
}

void vp_comms_send_cfg(void)
{
  vp_comms_printf("CFG,%u,%u,%u,%u\r\n",
                  vp_cfg.uv_trip_v, vp_cfg.ov_trip_v,
                  vp_cfg.oc_trip_ma, vp_cfg.restore_s);
}

static void send_cal(void)
{
  vp_comms_printf("CAL,%u,%u,%u,%u\r\n",
                  vp_cfg.v_offset, vp_cfg.v_span,
                  vp_cfg.i_offset, vp_cfg.i_span);
}

void vp_comms_telemetry(void)
{
  vp_comms_printf("T,%lu,%lu,%lu,%u,%u\r\n",
                  (unsigned long)HAL_GetTick(),
                  (unsigned long)vp_state.v_mv,
                  (unsigned long)vp_state.i_ma,
                  vp_state.connected ? 1u : 0u,
                  (unsigned)vp_state.fault);
}

void vp_comms_event(vp_fault_t f, uint32_t rms_v)
{
  vp_comms_printf("EVT,%lu,%u,%lu,detect=%lums\r\n",
                  (unsigned long)HAL_GetTick(), (unsigned)f,
                  (unsigned long)rms_v, (unsigned long)vp_state.detect_ms);
}

static bool set_param(const char *key, long val)
{
  if      (!strcmp(key, "UV")) vp_cfg.uv_trip_v  = (uint16_t)val;
  else if (!strcmp(key, "OV")) vp_cfg.ov_trip_v  = (uint16_t)val;
  else if (!strcmp(key, "OC")) vp_cfg.oc_trip_ma = (uint16_t)val;
  else if (!strcmp(key, "RESTORE")) vp_cfg.restore_s = (uint16_t)val;
  else if (!strcmp(key, "VSPAN"))   vp_cfg.v_span   = (uint16_t)val;
  else if (!strcmp(key, "VOFF"))    vp_cfg.v_offset = (uint16_t)val;
  else if (!strcmp(key, "ISPAN"))   vp_cfg.i_span   = (uint16_t)val;
  else if (!strcmp(key, "IOFF"))    vp_cfg.i_offset = (uint16_t)val;
  else return false;
  return true;
}

static void verify_flash(void)
{
  const volatile uint32_t *pg = (const volatile uint32_t *)0x0800FC00u;
  vp_comms_printf("VERIFY,magic=%08lX,expect=%08lX,uv=%u\r\n",
                  (unsigned long)pg[0], (unsigned long)VP_MAGIC,
                  (unsigned)(*(const volatile uint16_t *)0x0800FC04u));
}

static void dispatch(char *line)
{
  char *cmd = strtok(line, " ");
  if (!cmd) return;

  if (!strcmp(cmd, "GET")) {
    vp_comms_send_cfg();
    send_cal();
  } else if (!strcmp(cmd, "RESET")) {
    vp_protection_reset();
  } else if (!strcmp(cmd, "SAVE")) {
    bool ok = vp_settings_save();
    /* Read the page straight back. This separates "the flash write failed"
       from "the write worked but the simulator reloads flash on restart". */
    vp_comms_printf(ok ? "ACK,SAVE\r\n" : "ERR,FLASH\r\n");
    verify_flash();
  } else if (!strcmp(cmd, "VERIFY")) {
    verify_flash();
  } else if (!strcmp(cmd, "HELP") || !strcmp(cmd, "?")) {
    vp_comms_printf("GET (CFG + CAL) | SET <UV|OV|OC|RESTORE> <n> | SAVE | VERIFY | RESET | DEFAULTS\r\n");
    vp_comms_printf("KEY <MODE|ADJ>: press a front-panel button from the host\r\n");
    vp_comms_printf("cal: SET <VSPAN|VOFF|ISPAN|IOFF> <n>   units: V, V, mA, s\r\n");
    vp_comms_printf("T,ms,v_mV,i_mA,connected,fault  fault:1=UV 2=OV 3=OC\r\n");
  } else if (!strcmp(cmd, "DEFAULTS")) {
    vp_settings_defaults(&vp_cfg);
    vp_comms_printf("ACK,DEFAULTS\r\n");
    vp_comms_send_cfg();
  } else if (!strcmp(cmd, "KEY")) {
    char *k = strtok(NULL, " ");
    if (k && !strcmp(k, "MODE"))     vp_ui_key(true);
    else if (k && !strcmp(k, "ADJ")) vp_ui_key(false);
    else vp_comms_printf("ERR,BADKEY\r\n");
  } else if (!strcmp(cmd, "SET")) {
    char *key = strtok(NULL, " ");
    char *val = strtok(NULL, " ");
    long n = val ? strtol(val, NULL, 10) : 0;
    if (key && val && set_param(key, n)) {
      vp_comms_printf("ACK,SET %s %ld\r\n", key, n);   /* echo the parsed value */
      vp_comms_send_cfg();
    } else {
      vp_comms_printf("ERR,BADSET\r\n");
    }
  } else {
    vp_comms_printf("ERR,UNKNOWN\r\n");
  }
}

void vp_comms_poll(void)
{
  if (!rx_ready) return;
  dispatch(rx_line);
  rx_len = 0;
  rx_ready = false;
}
