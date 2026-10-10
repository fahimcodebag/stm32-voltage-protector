/* Sampling, cycle RMS, and the trip state machine.
 *
 * One ADC sample pair is taken every 1 ms. Every VP_SAMPLES_CYCLE samples
 * (one 50 Hz mains cycle) the RMS of each channel is computed and compared
 * against the thresholds. True RMS is used throughout, so a DC input (a
 * potentiometer, for bench testing) is simply the degenerate case of the same
 * maths and needs no separate code path.
 */
#include "main.h"
#include "adc.h"
#include "vp.h"

vp_state_t vp_state;

static uint64_t v_acc, i_acc;        /* sum of squares over the cycle */
static uint16_t n_acc;
static volatile bool cycle_ready;
static uint32_t fault_since_ms;      /* when the fault condition first held */
static uint32_t healthy_since_ms;    /* while tripped: when the line last became
                                        healthy, 0 = not healthy */
static uint32_t v_rms_at_trip, i_ma_at_trip;

/* Confirm a fault persists this long before opening the relay. One mains
 * cycle of confirmation rejects single-sample noise without blunting the
 * response; the spec cares about disconnecting before damage, not instantly. */
#define VP_CONFIRM_MS  20u

static uint16_t adc_read(uint32_t channel)
{
  ADC_ChannelConfTypeDef c = {0};
  uint16_t v = 0;
  c.Channel = channel;
  c.Rank = ADC_REGULAR_RANK_1;
  c.SamplingTime = ADC_SAMPLETIME_55CYCLES_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &c) != HAL_OK) return 0;
  HAL_ADC_Start(&hadc1);
  if (HAL_ADC_PollForConversion(&hadc1, 5) == HAL_OK) {
    v = (uint16_t)HAL_ADC_GetValue(&hadc1);
  }
  HAL_ADC_Stop(&hadc1);
  return v;
}

static void relay_set(bool closed)
{
  HAL_GPIO_WritePin(relay_GPIO_Port, relay_Pin, closed ? GPIO_PIN_SET : GPIO_PIN_RESET);
  /* fault LED is active-low: lit whenever the load is disconnected */
  HAL_GPIO_WritePin(fault_LED_GPIO_Port, fault_LED_Pin,
                    closed ? GPIO_PIN_SET : GPIO_PIN_RESET);
  vp_state.connected = closed;
}

void vp_protection_init(void)
{
  v_acc = i_acc = 0;
  n_acc = 0;
  cycle_ready = false;
  fault_since_ms = 0;
  vp_state.fault = VP_FAULT_NONE;
  vp_state.detect_ms = 0;
  relay_set(true);
}

void vp_protection_sample(void)
{
  int32_t vr = (int32_t)adc_read(ADC_CHANNEL_0) - (int32_t)vp_cfg.v_offset;
  int32_t ir = (int32_t)adc_read(ADC_CHANNEL_1) - (int32_t)vp_cfg.i_offset;

  v_acc += (uint64_t)((int64_t)vr * vr);
  i_acc += (uint64_t)((int64_t)ir * ir);

  if (++n_acc >= VP_SAMPLES_CYCLE) cycle_ready = true;
}

bool vp_protection_cycle_ready(void) { return cycle_ready; }

/* integer sqrt, avoids pulling the soft-float library in for a hot path */
static uint32_t isqrt64(uint64_t x)
{
  uint64_t r = 0, b = 1ull << 62;   /* must start on an EVEN power of two */
  while (b > x) b >>= 2;
  while (b) {
    if (x >= r + b) { x -= r + b; r = (r >> 1) + b; }
    else            { r >>= 1; }
    b >>= 2;
  }
  return (uint32_t)r;
}

void vp_protection_evaluate(void)
{
  uint32_t now = HAL_GetTick();
  uint32_t v_counts, i_counts;
  vp_fault_t f = VP_FAULT_NONE;

  if (!n_acc) return;
  v_counts = isqrt64(v_acc / n_acc);
  i_counts = isqrt64(i_acc / n_acc);
  v_acc = i_acc = 0;
  n_acc = 0;
  cycle_ready = false;

  /* counts -> engineering units (mV rms / mA rms) */
  vp_state.v_mv = (v_counts * (uint32_t)vp_cfg.v_span * 1000u) / VP_ADC_FULL;
  vp_state.i_ma = (i_counts * (uint32_t)vp_cfg.i_span * 1000u) / VP_ADC_FULL;

  if (vp_state.i_ma > vp_cfg.oc_trip_ma)                f = VP_FAULT_OVERCURRENT;
  else if (vp_state.v_mv > vp_cfg.ov_trip_v * 1000u)    f = VP_FAULT_OVERVOLT;
  else if (vp_state.v_mv < vp_cfg.uv_trip_v * 1000u)    f = VP_FAULT_UNDERVOLT;

  if (vp_state.connected) {
    if (f != VP_FAULT_NONE) {
      if (!fault_since_ms) {
        fault_since_ms = now;
        v_rms_at_trip = vp_state.v_mv / 1000u;
        i_ma_at_trip  = vp_state.i_ma;
      }
      if (now - fault_since_ms >= VP_CONFIRM_MS) {
        relay_set(false);
        healthy_since_ms   = 0;
        vp_state.fault     = f;
        vp_state.trip_ms   = now;
        vp_state.detect_ms = now - fault_since_ms;
        vp_state.trip_v    = v_rms_at_trip;   /* kept for the LCD: a fast  */
        vp_state.trip_ma   = i_ma_at_trip;    /* event outlives its reading */
        vp_comms_event(f, v_rms_at_trip);
      }
    } else {
      fault_since_ms = 0;                 /* condition cleared, rearm */
    }
  } else if (f != VP_FAULT_NONE) {
    healthy_since_ms = 0;                 /* still faulty: restart the wait */
  } else if (vp_cfg.restore_s) {
    /* Auto-reconnect only after the line has been CONTINUOUSLY healthy for
       restore_s. Timing from the trip instead let a single cycle that read
       healthy (sampling jitter reads a 270 V swell ~8% low) reconnect the
       load into an ongoing fault; a scope on PB1 caught the relay closing
       for ~25 ms in the middle of a swell. */
    if (!healthy_since_ms) healthy_since_ms = now ? now : 1u;
    if (now - healthy_since_ms >= vp_cfg.restore_s * 1000u) {
      vp_protection_reset();
    }
  }
}

void vp_protection_reset(void)
{
  fault_since_ms = 0;
  healthy_since_ms = 0;
  vp_state.fault = VP_FAULT_NONE;
  relay_set(true);
  vp_comms_printf("ACK,RESET\r\n");
}
