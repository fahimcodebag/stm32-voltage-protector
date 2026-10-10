@echo off
if not defined TC set TC=D:\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740\tools\bin
set GCC=%TC%\arm-none-eabi-gcc.exe
set SZ=%TC%\arm-none-eabi-size.exe
cd /d "%~dp0"
if not defined VP_OUT set VP_OUT=VoltageProtector

set CFLAGS=-mcpu=cortex-m3 -mthumb -DUSE_HAL_DRIVER -DSTM32F103xB -DPROTEUS_SIM=1 %VP_EXTRA% -O1 -g3 -Wall -ffunction-sections -fdata-sections
set INC=-ICore/Inc -IDrivers/STM32F1xx_HAL_Driver/Inc -IDrivers/CMSIS/Device/ST/STM32F1xx/Include -IDrivers/CMSIS/Include
set LDFLAGS=-T STM32F103C8TX_FLASH.ld --specs=nano.specs --specs=nosys.specs -Wl,--gc-sections -Wl,-Map=build.map

set SRC=Core/Src/main.c Core/Src/adc.c Core/Src/gpio.c Core/Src/i2c.c Core/Src/usart.c Core/Src/stm32f1xx_it.c Core/Src/stm32f1xx_hal_msp.c Core/Src/syscalls.c Core/Src/vp_settings.c Core/Src/vp_protection.c Core/Src/vp_comms.c Core/Src/vp_ui.c
set SRC=%SRC% Drivers/CMSIS/Device/ST/STM32F1xx/Source/Templates/system_stm32f1xx.c
set SRC=%SRC% Core/Startup/startup_stm32f103xb.s
set HAL=Drivers/STM32F1xx_HAL_Driver/Src
set SRC=%SRC% %HAL%/stm32f1xx_hal.c %HAL%/stm32f1xx_hal_cortex.c %HAL%/stm32f1xx_hal_rcc.c %HAL%/stm32f1xx_hal_rcc_ex.c %HAL%/stm32f1xx_hal_gpio.c %HAL%/stm32f1xx_hal_gpio_ex.c %HAL%/stm32f1xx_hal_adc.c %HAL%/stm32f1xx_hal_adc_ex.c %HAL%/stm32f1xx_hal_uart.c %HAL%/stm32f1xx_hal_i2c.c %HAL%/stm32f1xx_hal_dma.c %HAL%/stm32f1xx_hal_flash.c %HAL%/stm32f1xx_hal_flash_ex.c %HAL%/stm32f1xx_hal_pwr.c %HAL%/stm32f1xx_hal_exti.c

"%GCC%" %CFLAGS% %INC% %SRC% %LDFLAGS% -o %VP_OUT%.elf
echo BUILD_EXIT=%ERRORLEVEL%
if exist %VP_OUT%.elf "%SZ%" %VP_OUT%.elf
if exist %VP_OUT%.elf "%TC%\arm-none-eabi-objcopy.exe" -O ihex %VP_OUT%.elf %VP_OUT%.hex
if exist %VP_OUT%.elf "%TC%\arm-none-eabi-objcopy.exe" -O binary %VP_OUT%.elf %VP_OUT%.bin
echo HEX/BIN generated for Proteus
