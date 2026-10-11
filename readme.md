# STM32 Voltage Protector

FSMB Recruitment project. Firmware for an STM32F103C8 that monitors line
voltage and load current, disconnects the load on under-voltage, over-voltage or
over-current, reconnects it when the line has recovered, and lets the settings
be changed from a computer (UART) or from the device (two buttons + 16x2 LCD).

Everything here runs in **simulation** (Proteus 9). No hardware was built; per
the brief, nothing in this project is meant to be connected to mains.

## Repository layout

```
src/firmware/        STM32CubeMX/HAL project: sources, HAL/CMSIS drivers, linker script, build scripts
src/pc_app/          protocol.md (host protocol), protector_cli.py (host console, see Known issues)
schematics/          schematic of the device
test_evidence/       screenshots, terminal logs, oscilloscope captures (index in test_evidence/README.md)
project_files/
  proteus/           Proteus 9 projects + prebuilt .elf files
  proteus_stimulus/  MATLAB-generated fault waveforms for the Proteus FILE generators
  matlab/            grid_model.m (offline RMS/trip model), proteus_stimulus.m (waveform generator)
  cubemx/            CubeMX configuration script and notes
design_document.pdf  design document
```

## Running the main simulation (Proteus 9)

To run the final version of the project file, open the `with_matlab_grid.pdsprj` file project from `project_files/proteus/` directory in proteus, double-click the STM32F103C8 (U1)
and point **Program File** at the `VoltageProtector_ac.elf` which is the final firmware file that was used. Check **OSC Frequency = 8M**, **Clock Scale = Off** like in the image below. Other project files that was created along the way is also shared in the directory.

<img width="640" height="646" alt="image" src="https://github.com/user-attachments/assets/0fd1afbc-4bc6-44cb-bf3b-9f45d55249b2" />

| Project | Inputs | Use with |
|---|---|---|
| `proteus_config.pdsprj` | potentiometers on PA0/PA1 | `VoltageProtector.elf` |
| `with_matlab_grid.pdsprj` | FILE generators on PA0/PA1 playing the MATLAB fault scenario | `VoltageProtector_ac.elf` |
| `actual_circuit_with_pulldown_resistors.pdsprj` | push-button circuit with pull-down resistors | `VoltageProtector.elf` |

The Virtual Terminal ("SERIAL MONITOR") is the host. Click inside it to type
commands; the full command set is in `src/pc_app/protocol.md`.

### Potentiometer demo

With the DC defaults (0-3.3 V on PA0 maps to 0-400 V), the healthy band
180-250 V is roughly 45-62 % of the voltage pot. Below it trips under-voltage,
above it over-voltage. The current pot maps 0-3.3 V to 0-20 A; above
half-scale trips over-current (10 A).

### MATLAB fault scenario

The FILE generators need the waveform files. Either keep them at
`D:\VoltageProtector\stimulus\` or edit each generator's **Data File** to point
at `project_files/proteus_stimulus/pa0_voltage.txt` and `pa1_current.txt`.
Enable **Repeat Waveform** on both. To regenerate the files, run
`project_files/matlab/proteus_stimulus.m` in MATLAB (base MATLAB, R2022b), or
`run_stimulus.bat`.

One 13.5 s pass, with the AC build defaults (auto-reconnect after 1 s healthy):

| Scenario time | Event | Expected |
|---|---|---|
| 1.0-2.5 s | sag to 160 V | under-voltage trip, `EVT,...,1,...` |
| 4.5-6.0 s | swell to 270 V | over-voltage trip, `EVT,...,2,...` |
| 8.0-8.06 s | 3-cycle surge to 330 V | over-voltage trip |
| 10.0-11.5 s | load fault, ~14.7 A | over-current trip, `EVT,...,3,...` |

Each trip is followed by `ACK,RESET` about 1 s after the event ends.
Proteus runs slower than real time; read times from the `ms` field of the
terminal lines, not from the wall clock.

### Front panel

MODE steps RUN -> UNDER-VOLT -> OVER-VOLT -> OVER-CURRENT -> AUTO-RESTORE -> RUN.
ADJUST raises the value shown (UV/OV in 5 V steps, OC in 0.5 A, restore in 1 s;
it wraps to the minimum and auto-repeats when held). Leaving setup, by MODE or
after 6 s without a press, saves to flash if a value changed (`ACK,SAVE(UI)`). The run screen shows voltage,
current, relay state, and either the healthy band or the trip cause with the
value measured at the trip.

The same presses can be sent from the terminal with `KEY MODE` and `KEY ADJ`.

## Hardware (pin map)

| Pin | Function | Role |
|---|---|---|
| PA0 | ADC1_IN0 | line voltage sense (conditioned, 0-3.3 V) |
| PA1 | ADC1_IN1 | load current sense (conditioned, 0-3.3 V) |
| PA9 / PA10 | USART1 TX / RX | host link, 115200 8N1 |
| PB1 | GPIO out | relay / switch drive, high = load connected |
| PB6 / PB7 | GPIO (software I2C) | PCF8574 I2C backpack -> LCD1602, address 0x20 |
| PB10 / PB11 | GPIO in | MODE / ADJUST buttons, active high (switch to 3.3 V, 10k pull-down) |
| PC13 | GPIO out | fault LED, active low, lit while the load is disconnected |

PCF8574 to LCD: P0 RS, P1 RW, P2 E, P4-P7 D4-D7.

## Building the firmware

Toolchain: the GNU Arm toolchain bundled with STM32CubeIDE 2.2.0
(`arm-none-eabi-gcc` 14.3.1), on Windows. No IDE project is needed.

```
src\firmware\build.bat      ->  VoltageProtector.elf     (defaults for the potentiometer demo)
src\firmware\build_ac.bat   ->  VoltageProtector_ac.elf  (defaults for the AC waveform demo)
```

Both also write `.hex` and `.bin`. If the toolchain is not at the path in
`build.bat`, set `TC` to its `bin` folder first, e.g.
`set TC=C:\...\tools\bin` then run the script. The linker warning
"LOAD segment with RWX permissions" is expected.

The two variants differ only in the settings used when flash holds no saved
settings (see `src/pc_app/protocol.md`, Calibration presets).

`PROTEUS_SIM=1` is set by `build.bat`: Proteus does not model the STM32F1 PLL
and runs the core directly from the 8 MHz crystal, so this build runs SYSCLK
from HSE at 8 MHz. For real hardware remove `-DPROTEUS_SIM=1`; `main.c` then
configures the 72 MHz PLL.

Prebuilt copies of both `.elf` files are in `project_files/proteus/`.

## Known issues and limitations

- **Physical inputs blank the LCD in Proteus.** With the buttons wired as
  verified by an exported netlist, an edge on a button input often blanks the
  LCD, which recovers on the next screen change. Seen with analog switches and
  with LOGICSTATE sources, on PB10/PB11, PB14/PB15 and PA4/PA5, active-high and
  active-low, with and without internal pulls. `KEY MODE` / `KEY ADJ`, which run
  the same firmware path without a GPIO edge, have not shown it. Root cause not
  identified. The simulation projects use LOGICTOGGLE sources as buttons.
- **LCD over software I2C, write-only in simulation.** The I2C1 peripheral
  timed out in Proteus (log: "Access to I2C module while CR1.SWRST = 1"), so
  PB6/PB7 are driven in software. In Proteus the pin inputs read low even
  while the bus is high, so the firmware detects this at boot and drives the
  LCD without ACK checking. With readable pins (real hardware) ACK and
  stuck-bus checks are active.
- **Saved settings survive an MCU reset but not a full simulation restart.**
  Proteus reloads flash from the `.elf` when a simulation is started.
- **Sampling is paced from the 1 ms HAL tick in the main loop.** Blocking UART
  output (telemetry, command replies) skips samples, so an RMS window can
  cover slightly more than one mains cycle and read a few percent low (seen as
  ~202 V instead of 220 V on single readings). Trips need two consecutive faulty
  cycles and reconnection needs a continuously healthy period, so single
  readings do not trip or reconnect.
- **Relay.** The simulation drives a VSWITCH and LED from PB1 to stand for the
  relay; a real build needs a relay or SSR with a driver transistor.
- **The `with_matlab_grid` current waveform is the prospective load current.**
  It keeps flowing in the file after the relay opens; only trips while
  connected are meaningful.
- **`src/pc_app/protector_cli.py` has not been run against the simulation.**
  The simulation's host interface is the Proteus Virtual Terminal.
- **The CubeMX `.ioc` describes the 72 MHz hardware clock**, not the 8 MHz
  `PROTEUS_SIM` configuration, and `src/firmware` is built from the `.bat`
  scripts, not opened as a CubeIDE project.
