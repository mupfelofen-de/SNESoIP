/**
 * @file       SNES.c
 * @brief      SNES I/O driver
 * @ingroup    SNESIO SNES I/O driver
 * @defgroup   SNESIO SNES I/O driver
 * @details
 * @code{.unparsed}
 *
 * Wiring diagram (default configuration):
 *   +-------------------+--------------+,
 *   |                   |                \
 *   | [VCC] [2] [3] [4] | [NC] [6] [GND] |
 *   |                   |                /
 *   +-------------------+--------------+'
 *
 * Important: The logic levels need to be converted using a
 * bidirectional logic level shifter such as the TXS0108E by Texas
 * Instruments.
 *
 *   +-------------+-----+--------- -+-------+-------+
 *   | Location    | Pin | Desc.     | Conn. | ESP32 |
 *   +-------------+-----+-----------+-------+-------+
 *   | SNES Port1  |  1  | +5V       |  +5V  |       |
 *   | SNES Port1  |  2  | Clock     | LShft |       |
 *   | SNES Port1  |  3  | Latch     | LShft |       |
 *   | SNES Port1  |  4  | Data      | LShft |       |
 *   | SNES Port1  |  6  | IOPort 6  | LShft |       |
 *   +-------------+-----+-----------+-------+-------+
 *   | SNES Port2  |  2  | Clock     | LShft |       |
 *   | SNES Port2  |  3  | Latch     | LShft |       |
 *   | SNES Port2  |  4  | Data      | LShft |       |
 *   | SNES Port2  |  6  | IOPort 7  | LShft |       |
 *   | SNES Port2  |  7  | GND       |  GND  |       |
 *   +-------------+-----+-----------+-------+-------+
 *   | SNES Input  |  1  | +5V       |  +5V  |       |
 *   | SNES Input  |  2  | Clock     | LShft | IO 25 |
 *   | SNES Input  |  3  | Latch     | LShft | IO 26 |
 *   | SNES Input  |  4  | Data      | LShft | IO 27 |
 *   | SNES Input  |  7  | GND       |  GND  |       |
 *   +-------------+-----+-----------+-------+-------+
 *
 * The IOPort in the wiring diagram above can be accessed trough bit 6
 * and 5 of the Joypad Programmable I/O Port.
 *
 * The SNESoIP uses the SNES controller ports as a power supply and
 * because VCC and GND is connected on both sides, can you save at least
 * one pin on each cable.  These pins can be used to gain access to the
 * usually unconnected pin 6 used by the IOPort.
 *
 * The SNESoIP uses the IOPort to establish bidirectional
 * communication.
 *
 * But because SNES controller connectors aren't designed to be reopened
 * again, I wrote some simple instructions how they can be modified to
 * fit the requirements.  See the main page of the documentation for
 * details.
 *
 * 4201h WRIO (Open-Collector Output) (W)
 *
 *   IOPort6 Port1 Pin 6
 *   IOPort7 Port2 Pin 6
 *
 *   Note: Due to the weak high-level, the raising "edge" is raising
 *   rather slowly, for sharper transitions one may need external
 *   pull-up resistors. Source: fullsnes by nocash
 *
 * 4213h RDIO (Input) (R)
 *
 *   When used as Input via 4213h, set the corresponding bits in 4201h
 *   to high.
 *
 * @endcode
 * @author     Michael Fitzmayer
 * @copyright  "THE BEER-WARE LICENCE" (Revision 42)
 * @todo       Determine if the IOPort needs to be connected to the
 *             logic level shifter due it's weak high-level.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/rmt.h"
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "SNES.h"

/**
 * @struct  SNESDriver
 * @brief   SNES I/O driver data
 */
typedef struct SNESDriver_t
{
    bool     bIsRunning;
    uint16_t u16InputData;
    bool     bIOPortBit6;
    bool     bIOPortBit7;

    rmt_config_t stLatch;
    rmt_item32_t stLatchItem[1];

    rmt_config_t stClock;
    rmt_item32_t stClockItem[17];

} SNESDriver;

/**
 * @var    _stDriver
 * @brief  SNES I/O driver private data
 */
static SNESDriver _stDriver;

static void _SNESReadInputThread(void* pArg);
static void _SendClock(void);
static void _SendLatch(void);

/**
 * @fn     void InitSNES(void)
 * @brief  Initialise SNES I/O driver
 */
void InitSNES(void)
{
    gpio_config_t stGPIOConf;

    memset(&_stDriver, 0, sizeof(struct SNESDriver_t));
    _stDriver.u16InputData = 0xffff;

    stGPIOConf.intr_type    = GPIO_PIN_INTR_DISABLE;
    stGPIOConf.mode         = GPIO_MODE_INPUT;
    stGPIOConf.pin_bit_mask = GPIO_SNES_INPUT_DATA_BIT;
    stGPIOConf.pull_up_en   = 1;
    ESP_ERROR_CHECK(gpio_config(&stGPIOConf));

    // Initialise latch signal.
    _stDriver.stLatch.rmt_mode      = RMT_MODE_TX;
    _stDriver.stLatch.channel       = RMT_CHANNEL_0;
    _stDriver.stLatch.clk_div       = 80;
    _stDriver.stLatch.gpio_num      = GPIO_SNES_INPUT_LATCH_PIN;
    _stDriver.stLatch.mem_block_num = 1;

    _stDriver.stLatch.tx_config.loop_en        = 0;
    _stDriver.stLatch.tx_config.idle_level     = RMT_IDLE_LEVEL_LOW;
    _stDriver.stLatch.tx_config.idle_output_en = 1;

    rmt_config(&_stDriver.stLatch);
    rmt_driver_install(_stDriver.stLatch.channel, 0, 0);

    _stDriver.stLatchItem[0].duration0 = 12;
    _stDriver.stLatchItem[0].level0    = 1;
    _stDriver.stLatchItem[0].duration1 = 0;
    _stDriver.stLatchItem[0].level1    = 0;

    // Initialise clock signal.
    _stDriver.stClock.rmt_mode      = RMT_MODE_TX;
    _stDriver.stClock.channel       = RMT_CHANNEL_1;
    _stDriver.stClock.clk_div       = 80;
    _stDriver.stClock.gpio_num      = GPIO_SNES_INPUT_CLOCK_PIN;
    _stDriver.stClock.mem_block_num = 1;

    _stDriver.stClock.tx_config.loop_en        = 0;
    _stDriver.stClock.tx_config.idle_level     = RMT_IDLE_LEVEL_HIGH;
    _stDriver.stClock.tx_config.idle_output_en = 1;

    rmt_config(&_stDriver.stClock);
    rmt_driver_install(_stDriver.stClock.channel, 0, 0);

    _stDriver.stClockItem[0].duration0 = 6;
    _stDriver.stClockItem[0].level0    = 1;
    _stDriver.stClockItem[0].duration1 = 5;
    _stDriver.stClockItem[0].level1    = 1;

    for (uint8_t u8Index = 1; u8Index < 17; u8Index++)
    {
        _stDriver.stClockItem[u8Index].duration0 = 6;
        _stDriver.stClockItem[u8Index].level0    = 0;
        _stDriver.stClockItem[u8Index].duration1 = 6;
        _stDriver.stClockItem[u8Index].level1    = 1;
    }

    xTaskCreate(
        _SNESReadInputThread,
        "SNESReadInputThread",
        4096, NULL, 3, NULL);
}

/**
 * @fn     void DeInitSNES(void)
 * @brief  De-initialise/stop SNES I/O driver
 */
void DeInitSNES(void)
{
    _stDriver.bIsRunning = false;
}

/**
 * @fn     uint16_t GetSNESInputData(void)
 * @brief  Get current SNES controller input data
 */
uint16_t GetSNESInputData(void)
{
    return _stDriver.u16InputData;
}

/**
 * @fn       void _SNESReadInputThread(void* pArg)
 * @brief    Read SNES controller input
 * @param    pArg
 *           Unused
 * @details
 * @code{.unparsed}
 *
 * Every 16.67ms / (60Hz), the SNES CPU sends out a 12µs wide, positive
 * going data latch pulse on pin 3.  This instructs the parallel-in
 * serial-out shift register in the controller to latch the state of all
 * buttons internally.
 *
 * 6µs after the fall of the data latch pulse, the CPU sends out 16 data
 * clock pulses on pin 2.  These are 50% duty cycle with 12µs per full
 * cycle.  The controllers serially shift the latched button states out
 * of pin 4 on very rising edge of the clock, and the CPU samples the
 * data on every falling edge.
 *
 * At the end of the 16 cycle sequence, the serial data line is driven
 * low until the next data latch pulse.  Only 4 of the 16 clock cycles
 * are shown for brevity:
 *
 *                    |<------------16.67ms------------>|
 *
 *                    12µs
 *                 -->|   |<--
 *
 *                     ---                               ---
 *                    |   |                             |   |
 * Data Latch      ---     -----------------/ /----------    --------...
 *
 * Data clock      ----------   -   -   -  -/ /----------------   -  ...
 *                           | | | | | | | |                   | | | |
 *                            -   -   -   -                     -   -
 *                            1   2   3   4                     1   2
 *
 * Serial Data         ----     ---     ----/ /           ---
 *                    |    |   |   |   |                 |
 * (Buttons B      ---      ---     ---        ----------
 *  & Select       norm      B      SEL           norm
 *  pressed).      low                            low
 *                         12µs
 *                      -->|   |<--
 *
 * Source: repairfaq.org
 *
 * @endcode
 */
static void _SNESReadInputThread(void* pArg)
{
    _stDriver.bIsRunning = true;

    while (_stDriver.bIsRunning)
    {
        _SendLatch();
        _SendClock();
        vTaskDelay(16 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

static void _SendClock(void)
{
    rmt_write_items(_stDriver.stClock.channel, _stDriver.stClockItem, 16, 1);
}

static void _SendLatch(void)
{
    rmt_write_items(_stDriver.stLatch.channel, _stDriver.stLatchItem, 1, 0);
}
