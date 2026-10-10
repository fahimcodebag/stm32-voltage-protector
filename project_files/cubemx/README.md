# CubeMX configuration

`cubemx_config.script` is the exact CubeMX command script for this project's
pinout and peripheral configuration. Every `set mode` / `set pin` line below was
verified to return `OK` under CubeMX 6.18.1 headless mode:

    STM32CubeMX.exe -q <path-to-this-script>

## Known limitation

Peripheral **configuration** scripts headlessly without problems, but
`project generate` deadlocks in CubeMX 6.18.1 (it spins in the `ProjectChoiceTab`
UI component and never returns, with or without the FW_F1 package installed).
Code generation therefore has to be done once from the GUI. Open CubeMX, apply the
pinout below, and press **Project > Generate Code**.

## Pin assignment

| Pin   | Function       | Role                        |
|-------|----------------|-----------------------------|
| PA0   | ADC1_IN0       | line voltage sense          |
| PA1   | ADC1_IN1       | load current sense          |
| PA9   | USART1_TX      | host link                   |
| PA10  | USART1_RX      | host link                   |
| PB1   | GPIO_Output    | relay drive (disconnect)    |
| PB6   | I2C1_SCL       | LCD1602                     |
| PB7   | I2C1_SDA       | LCD1602                     |
| PB10  | GPIO_Input     | MODE button                 |
| PB11  | GPIO_Input     | ADJUST button               |
| PC13  | GPIO_Output    | fault LED                   |

Toolchain: STM32CubeIDE. Copy setting: "copy only".
