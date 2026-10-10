/* Persisted settings: last flash page of the STM32F103C8 (64 KB device). */
#include "main.h"
#include "vp.h"
#include <string.h>

#define VP_FLASH_PAGE   0x0800FC00u          /* last 1 KB page of 64 KB     */

vp_settings_t vp_cfg;

void vp_settings_defaults(vp_settings_t *s)
{
  memset(s, 0, sizeof(*s));
  s->magic      = VP_MAGIC;
  s->uv_trip_v  = 180;
  s->ov_trip_v  = 250;
  s->oc_trip_ma = 10000;                     /* 10 A                        */
#if VP_AC_FRONTEND
  /* Mid-rail biased AC front end (build_ac.bat), as fed by the MATLAB
     stimulus in Proteus. Span is twice the peak range: a 4095-count deviation
     maps to the span and a mid-rail signal can only deviate 2047 counts.
     restore_s = 1 so the stimulus events, 2 s apart, are not masked by the
     previous trip. Used whenever flash holds no valid settings, which in
     Proteus is every full simulation restart. */
  s->restore_s  = 1;
  s->v_offset   = 2048;
  s->v_span     = 2 * VP_V_SPAN_DEF;
  s->i_offset   = 2048;
  s->i_span     = 2 * VP_I_SPAN_DEF;
#else
  s->restore_s  = 5;
  s->v_offset   = 0;                         /* DC-proxy default: pot demo  */
  s->v_span     = VP_V_SPAN_DEF;
  s->i_offset   = 0;
  s->i_span     = VP_I_SPAN_DEF;
#endif
}

static uint32_t vp_crc(const vp_settings_t *s)
{
  const uint8_t *p = (const uint8_t *)s;
  uint32_t c = 0xA5A5A5A5u;
  /* everything except the trailing crc field */
  for (uint32_t i = 0; i < sizeof(*s) - sizeof(uint32_t); i++) {
    c = (c << 5) ^ (c >> 27) ^ p[i];
  }
  return c;
}

bool vp_settings_load(void)
{
  const vp_settings_t *f = (const vp_settings_t *)VP_FLASH_PAGE;
  if (f->magic == VP_MAGIC && f->crc == vp_crc(f)) {
    memcpy(&vp_cfg, f, sizeof(vp_cfg));
    return true;
  }
  vp_settings_defaults(&vp_cfg);
  return false;
}

bool vp_settings_save(void)
{
  FLASH_EraseInitTypeDef er = {0};
  uint32_t page_err = 0;
  bool ok = true;

  vp_cfg.magic = VP_MAGIC;
  vp_cfg.crc   = vp_crc(&vp_cfg);

  HAL_FLASH_Unlock();
  er.TypeErase   = FLASH_TYPEERASE_PAGES;
  er.PageAddress = VP_FLASH_PAGE;
  er.NbPages     = 1;
  if (HAL_FLASHEx_Erase(&er, &page_err) != HAL_OK) {
    ok = false;
  } else {
    const uint16_t *src = (const uint16_t *)&vp_cfg;
    for (uint32_t i = 0; i < sizeof(vp_cfg) / 2 && ok; i++) {
      if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                            VP_FLASH_PAGE + i * 2u, src[i]) != HAL_OK) {
        ok = false;
      }
    }
  }
  HAL_FLASH_Lock();
  return ok;
}
