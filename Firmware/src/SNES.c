/**
 * @file     SNES.c
 * @brief    SNES I/O driver
 * @ingroup  Firmware
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
 * bi-directional logic level converter such as the BSS138 by Fairchild
 * Semiconductor: https://www.sparkfun.com/products/12009
 *
 *   +-------------+-----+--------- -+-------+-------+
 *   | Location    | Pin | Desc.     | Conn. | ESP32 |
 *   +-------------+-----+-----------+-------+-------+
 *   | SNES Port0  |  1  | +5V       |  +5V  |       |
 *   | SNES Port0  |  2  | Clock     | LShft | IO 14 |
 *   | SNES Port0  |  3  | Latch     | LShft | IO 15 |
 *   | SNES Port0  |  4  | Data      | LShft | IO 12 |
 *   | SNES Port0  |  6  | IOPort 6  | LShft |       |
 *   +-------------+-----+-----------+-------+-------+
 *   | SNES Port1  |  2  | Clock     | LShft | IO 18 |
 *   | SNES Port1  |  3  | Latch     | LShft | IO  5 |
 *   | SNES Port1  |  4  | Data      | LShft | IO 19 |
 *   | SNES Port1  |  6  | IOPort 7  | LShft |       |
 *   | SNES Port1  |  7  | GND       |  GND  |       |
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
#include "esp_log.h"
#include "driver/rmt.h"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "rom/ets_sys.h"
#include "SNES.h"

/**
 * @typedef  SNESDriver
 * @brief    SNES I/O driver data
 * @struct   SNESDriver_t
 * @brief    SNES I/O driver data structure
 */
typedef struct SNESDriver_t
{
    bool     bIsRunning;    ///< Run condition
    uint16_t u16InputData;  ///< Controller input data
    bool     bIOPortBit6;   ///< Programmable I/O Port bit 6
    bool     bIOPortBit7;   ///< Programmable I/O Port bit 7

    uint32_t                     u32Port0Tx;  ///< Port 0 TX buffer
    spi_bus_config_t             stPort0Bus;  ///< HSPI bus configuration
    spi_slave_interface_config_t stPort0;     ///< HSPI interface configuration

    uint32_t                     u32Port1Tx;  ///< Port 1 TX buffer
    spi_bus_config_t             stPort1Bus;  ///< VSPI bus configuration
    spi_slave_interface_config_t stPort1;     ///< VSPI interface configuration

    rmt_config_t stLatch;          ///< Latch signal configuration
    rmt_item32_t stLatchItem[1];   ///< Latch signal data
    rmt_config_t stClock;          ///< Clock signal configuration
    rmt_item32_t stClockItem[17];  ///< Clock signal data

    rmt_config_t stDebug;
    rmt_item32_t stDebugItem[1];

} SNESDriver;

/**
 * @var    _stDriver
 * @brief  SNES I/O driver private data
 */
static SNESDriver _stDriver;

static void _SNESReadInputThread(void* pArg);
#ifdef DEBUG
static void _SNESDebugThread(void* pArg);
#endif
static void _InitSNESSigGen(void);

static void IRAM_ATTR Port0Setup(spi_slave_transaction_t *stTrans);
static void IRAM_ATTR Port0Trans(spi_slave_transaction_t *stTrans);
static void IRAM_ATTR Port1Setup(spi_slave_transaction_t *stTrans);
static void IRAM_ATTR Port1Trans(spi_slave_transaction_t *stTrans);

/**
 * @fn     void InitSNES(void)
 * @brief  Initialise SNES I/O driver
 */
void InitSNES(void)
{
    gpio_config_t stGPIOConf;

    memset(&_stDriver, 0, sizeof(struct SNESDriver_t));
    _stDriver.bIsRunning   = true;
    _stDriver.u16InputData = 0xffff;
    _stDriver.u32Port0Tx   = 0xffffffff;
    _stDriver.u32Port1Tx   = 0xffffffff;

    // GPIO configuration.
    stGPIOConf.intr_type    = GPIO_PIN_INTR_DISABLE;
    stGPIOConf.mode         = GPIO_MODE_INPUT_OUTPUT;
    stGPIOConf.pin_bit_mask =
        SNES_PORT0_DATA_BIT | SNES_PORT1_DATA_BIT;
    stGPIOConf.pull_down_en = 0;
    stGPIOConf.pull_up_en   = 1;
    ESP_ERROR_CHECK(gpio_config(&stGPIOConf));

    stGPIOConf.intr_type    = GPIO_PIN_INTR_DISABLE;
    stGPIOConf.mode         = GPIO_MODE_INPUT;
    stGPIOConf.pin_bit_mask =
        SNES_INPUT_DATA_BIT  |
        SNES_PORT0_CLOCK_BIT | SNES_PORT1_CLOCK_BIT |
        SNES_PORT0_LATCH_BIT | SNES_PORT1_LATCH_BIT;
    stGPIOConf.pull_down_en = 0;
    stGPIOConf.pull_up_en   = 1;
    ESP_ERROR_CHECK(gpio_config(&stGPIOConf));

    // Controller port 0 (HSPI).
    _stDriver.stPort0Bus.mosi_io_num     = -1;
    _stDriver.stPort0Bus.miso_io_num     = SNES_PORT0_DATA_PIN;
    _stDriver.stPort0Bus.sclk_io_num     = SNES_PORT0_CLOCK_PIN;
    _stDriver.stPort0Bus.quadwp_io_num   = -1;
    _stDriver.stPort0Bus.quadhd_io_num   = -1;
    _stDriver.stPort0Bus.max_transfer_sz =  0;
    _stDriver.stPort0Bus.flags           = SPICOMMON_BUSFLAG_SLAVE;
    _stDriver.stPort0Bus.intr_flags      = ESP_INTR_FLAG_IRAM;

    _stDriver.stPort0.spics_io_num       = SNES_PORT0_LATCH_PIN;
    _stDriver.stPort0.flags              = SPI_SLAVE_BIT_LSBFIRST;
    _stDriver.stPort0.queue_size         = 1;
    _stDriver.stPort0.mode               = 2;
    _stDriver.stPort0.post_setup_cb      = Port0Setup;
    _stDriver.stPort0.post_trans_cb      = Port0Trans;

    ESP_ERROR_CHECK(spi_slave_initialize(HSPI_HOST, &_stDriver.stPort0Bus, &_stDriver.stPort0, 0));

    // Controller port 1 (VSPI).
    _stDriver.stPort1Bus.mosi_io_num     = -1;
    _stDriver.stPort1Bus.miso_io_num     = SNES_PORT1_DATA_PIN;
    _stDriver.stPort1Bus.sclk_io_num     = SNES_PORT1_CLOCK_PIN;
    _stDriver.stPort1Bus.quadwp_io_num   = -1;
    _stDriver.stPort1Bus.quadhd_io_num   = -1;
    _stDriver.stPort1Bus.max_transfer_sz =  0;
    _stDriver.stPort1Bus.flags           = SPICOMMON_BUSFLAG_SLAVE;
    _stDriver.stPort1Bus.intr_flags      = ESP_INTR_FLAG_IRAM;

    _stDriver.stPort1.spics_io_num       = SNES_PORT1_LATCH_PIN;
    _stDriver.stPort1.flags              = SPI_SLAVE_BIT_LSBFIRST;
    _stDriver.stPort1.queue_size         = 1;
    _stDriver.stPort1.mode               = 2;
    _stDriver.stPort1.post_setup_cb      = Port1Setup;
    _stDriver.stPort1.post_trans_cb      = Port1Trans;

    ESP_ERROR_CHECK(spi_slave_initialize(VSPI_HOST, &_stDriver.stPort1Bus, &_stDriver.stPort1, 0));

    // Initialise latch and clock signal generator.
    _InitSNESSigGen();

    xTaskCreate(
        _SNESReadInputThread,
        "SNESReadInputThread",
        2048, NULL, 3, NULL);

    #ifdef DEBUG
    xTaskCreate(
        _SNESDebugThread,
        "SNESDebugThread",
        2048, NULL, 3, NULL);
    #endif
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
 * @fn      void SendClock(void)
 * @brief   Send clock signal.
 */
void SendClock(void)
{
    rmt_write_items(_stDriver.stClock.channel, _stDriver.stClockItem, 17, 0);
}

/**
 * @fn     void SendLatch(void)
 * @brief  Send latch pulse.
 */
void SendLatch(void)
{
    rmt_write_items(_stDriver.stLatch.channel, _stDriver.stLatchItem, 1, 0);
}

/**
 * @fn       void _SNESReadInputThread(void* pArg)
 * @brief    Read SNES controller input
 * @details
 * @code{.unparsed}
 *
 * Most games use the so called auto-joypad mode.  In this mode every
 * 16.67ms or about 60Hz, the SNES CPU sends out a 12µs wide, positive
 * going data latch pulse on pin 3 of the controller port.  This
 * instructs the parallel-in serial-out shift register in the controller
 * to latch the state of all buttons internally.
 *
 * Remark: It is possible to trigger the latch and clock manually to
 * achieve higher transfer rates.
 *
 * 6µs after the fall of the data latch pulse, the CPU sends out 16 data
 * clock pulses on pin 2.  These are 50% duty cycle with 12µs per full
 * cycle.  The controllers serially shift the latched button states out
 * of pin 4 on very rising edge of the clock, and the CPU samples the
 * data on every falling edge.
 *
 * At the end of the 16 cycle sequence, the serial data line is driven
 * low until the next data latch pulse.
 *
 * But because the clock is normally high, the first transition it makes
 * after the latch signal is a high-to-low transition.  Since data for
 * the first button will be latched on this transition, it's data must
 * actually be driven earlier.  The SNES controllers drive data for the
 * first button at the falling edge of the latch.

 * The protocol used looks like SPI.  However, using the SPI slave
 * driver from esp-idf to transmit data to the controller ports requires
 * a little hack.  Here's what happens if you try it:
 *
 * Data latch (used as CS):
 *
 *        12µs
 *     >-------<
 *     +---+---+    6µs
 *     |       |   >---<
 * +---+       +---+---+-------------------+
 *
 * Clock signal:
 *
 * +-----------+---+   +---+   +---+   +---+
 *                 |   |   |   |   |   |   |
 *                 |   |   |   |   |   |   |
 *                 +---+   +---+   +---+   +
 *
 * MISO starts sending the first data bit half a clock cycle too late:
 *
 * +---------------+       +----------------+
 *                 |       |
 *                 |       |
 *                 +---+---+
 *
 * To solve this problem a XNOR-gate between the latch and the clock
 * signal is used to generate a new SPI clock:
 *
 *     +---+---+   +---+   +---+   +---+   +
 *     |       |   |   |   |   |   |   |   |
 *     |       |   |   |   |   |   |   |   |
 * +---+       +---+   +---+   +---+   +---+
 *
 * (Thanks to Stéphane Marty (https://www.youtube.com/user/dexsilicium)
 * for the tipp!)
 *
 * But using this new SPI clock results in a different unwanted
 * behaviour; the data is sent one clock cycle too early (first bit on
 * the rising edge of the latch pulse).
 *
 * To compensate, I added a 17th dummy bit at the beginning of the
 * transmission.
 *
 * tbc.
 *
 * @endcode
 *           The data is retrieved three times as often as on a SNES.
 *           Signal fluctuations, probably caused by wrong timing, are
 *           compensated by comparing the results.
 * @param    pArg Unused
 * @todo     Use the RMT module driver to read the data in non-blocking
 *           mode.
 */
static void _SNESReadInputThread(void* pArg)
{
    uint16_t u16Temp[3] = { 0xffff, 0xffff, 0xffff };
    uint8_t  u8Attempt = 0;
    spi_slave_transaction_t stTrans0;
    spi_slave_transaction_t stTrans1;

    memset(&stTrans0, 0, sizeof(stTrans0));
    stTrans0.length    = 17;
    stTrans0.trans_len = 17;
    stTrans0.tx_buffer = &_stDriver.u32Port0Tx;

    while (_stDriver.bIsRunning)
    {
        SendLatch();
        SendClock();

        ets_delay_us(3);
        for (uint8_t u8Bit = 0; u8Bit < 16; u8Bit++)
        {
            ets_delay_us(6);
            if (u8Bit < 12)
            {
                if (gpio_get_level(SNES_INPUT_DATA_PIN))
                {
                    if (gpio_get_level(SNES_INPUT_DATA_PIN))
                    {
                        u16Temp[u8Attempt] |= 1 << u8Bit;
                    }
                }
                else
                {
                    u16Temp[u8Attempt] &= ~(1 << u8Bit);
                }
            }
            ets_delay_us(6);
        }
        u8Attempt++;

        // Compensate signal fluctuations.
        if (u8Attempt > 2)
        {
            if (u16Temp[0] == u16Temp[1] && u16Temp[1] == u16Temp[2])
            {
                _stDriver.u16InputData = u16Temp[0];
            }
            _stDriver.u32Port0Tx = _stDriver.u16InputData;
            _stDriver.u32Port0Tx = _stDriver.u32Port0Tx << 1;
            _stDriver.u32Port0Tx &= ~(1 << 0);
            spi_slave_queue_trans(VSPI_HOST, &stTrans0, 0);

            u8Attempt = 0;
        }

        vTaskDelay(5 / portTICK_PERIOD_MS);
    }

    vTaskDelete(NULL);
}

#ifdef DEBUG
/**
 * @fn     _SNESDebugThread(void* pArg)
 * @brief  Debug thread
 */
static void _SNESDebugThread(void* pArg)
{
    char     acDebug[13] = { 0 };
    uint16_t u16Temp     = 0xffff;
    uint16_t u16Prev     = 0xffff;

    while (_stDriver.bIsRunning)
    {
        u16Temp = _stDriver.u16InputData;

        for (uint8_t u8Index = 0; u8Index < 12; u8Index++)
        {
            if ((u16Temp >> u8Index) & 1)
            {
                acDebug[u8Index] = '0';
            }
            else
            {
                acDebug[u8Index] = '1';
            }
        }
        acDebug[12] = '\0';
        if (u16Temp != u16Prev)
        {
            ESP_LOGI("SNES", "%s", acDebug);
            u16Prev = u16Temp;
        }

        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}
#endif

/**
 * @fn       void _InitSNESSigGen(void)
 * @brief    SNES signal generator
 * @details  This function initialises the latch and clock pins to use
 *           the RMT (Remote Control) module driver.
 */
static void _InitSNESSigGen(void)
{
    // Initialise latch signal.
    _stDriver.stLatch.rmt_mode      = RMT_MODE_TX;
    _stDriver.stLatch.channel       = RMT_CHANNEL_0;
    _stDriver.stLatch.clk_div       = 80;
    _stDriver.stLatch.gpio_num      = SNES_INPUT_LATCH_PIN;
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
    _stDriver.stClock.gpio_num      = SNES_INPUT_CLOCK_PIN;
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
}

static void IRAM_ATTR Port0Setup(spi_slave_transaction_t *stTrans)
{
    (void)stTrans;
}

static void IRAM_ATTR Port0Trans(spi_slave_transaction_t *stTrans)
{
    (void)stTrans;
}

static void IRAM_ATTR Port1Setup(spi_slave_transaction_t *stTrans)
{
    (void)stTrans;
}

static void IRAM_ATTR Port1Trans(spi_slave_transaction_t *stTrans)
{
    (void)stTrans;
}
