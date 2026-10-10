# Host <-> Device Serial Protocol

USART1 (PA9 TX / PA10 RX), 115200 8N1, ASCII lines terminated by `\r\n` (the
device accepts `\r` or `\n`). Human-readable on purpose, so any serial terminal
(the Proteus Virtual Terminal in the simulation) works as the host.
Backspace / DEL edit the line being typed.

## Device -> Host

| Frame | Meaning |
|---|---|
| `T,<ms>,<v_mV>,<i_mA>,<connected>,<fault>` | telemetry, 10 Hz. `ms` = HAL tick since boot |
| `CFG,<uv_V>,<ov_V>,<oc_mA>,<restore_s>` | protection settings; on `GET`, after any `SET`, and after each front-panel adjustment |
| `CAL,<v_offset>,<v_span>,<i_offset>,<i_span>` | front-end calibration; on `GET` |
| `EVT,<ms>,<fault>,<rms_V>,detect=<n>ms` | emitted as the relay opens. `rms_V` = reading when the fault was first seen, `detect` = time from first faulty cycle to relay open |
| `ACK,<cmd ...>` | command accepted, e.g. `ACK,SET VSPAN 800` echoes the parsed value |
| `ACK,RESET` | relay re-closed (by `RESET` or by auto-reconnect) |
| `ACK,SAVE` / `ACK,SAVE(UI)` | settings written to flash (host command / front panel) |
| `VERIFY,magic=<hex>,expect=<hex>,uv=<n>` | flash page read back after `SAVE` |
| `ERR,BADSET` / `ERR,BADKEY` / `ERR,UNKNOWN` / `ERR,FLASH` | command rejected / flash write failed |
| `UI,MODE,<n>` | front panel moved to screen `n` (0 run, 1 UV, 2 OV, 3 OC, 4 restore) |
| `UI,...` | LCD status messages at boot |

`connected`: `1` relay closed (load connected), `0` open.
`fault`: `0` none, `1` under-voltage, `2` over-voltage, `3` over-current.
The fault code stays latched until the relay reconnects.

## Host -> Device

| Command | Effect |
|---|---|
| `GET` | reply `CFG,...` and `CAL,...` |
| `SET UV <V>` / `SET OV <V>` | under- / over-voltage trip threshold, V rms |
| `SET OC <mA>` | over-current trip threshold, mA rms |
| `SET RESTORE <s>` | auto-reconnect after the line has been healthy for `s` seconds; `0` = manual (`RESET`) only |
| `SET VOFF <counts>` / `SET VSPAN <V>` | voltage calibration: ADC counts at 0 V, volts per 4095-count deviation |
| `SET IOFF <counts>` / `SET ISPAN <A>` | current calibration, same meaning |
| `SAVE` | write settings to flash (CRC-checked, last 1 KB page) |
| `VERIFY` | read the flash page back |
| `DEFAULTS` | load build defaults (not saved until `SAVE`) |
| `RESET` | clear a latched fault and re-close the relay |
| `KEY MODE` / `KEY ADJ` | press a front-panel button from the host (same code path as the buttons) |
| `HELP` or `?` | command summary |

`SET` changes take effect immediately; they persist only after `SAVE` (or when
leaving the front-panel setup screens, which saves automatically).

## Calibration presets

| Front end | VOFF | VSPAN | IOFF | ISPAN |
|---|---|---|---|---|
| DC potentiometer demo (default of `build.bat`) | 0 | 400 | 0 | 20 |
| Mid-rail biased AC, +/-400 V pk, +/-20 A pk (default of `build_ac.bat`) | 2048 | 800 | 2048 | 40 |

The span is the value for a full 4095-count deviation; a mid-rail signal can
only deviate 2047 counts, hence twice the peak range.
