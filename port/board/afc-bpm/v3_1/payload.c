/*
 *   openMMC -- Open Source modular IPM Controller firmware
 *
 *   Copyright (C) 2015  Piotr Miedzik  <P.Miedzik@gsi.de>
 *   Copyright (C) 2015-2016  Henrique Silva <henrique.silva@lnls.br>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *   @license GPL-3.0+ <http://spdx.org/licenses/GPL-3.0+>
 */

/* FreeRTOS Includes */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "event_groups.h"

/* Project Includes */
#include "port.h"
#include "payload.h"
#include "ipmi.h"
#include "task_priorities.h"
#include "adn4604.h"
#include "ad84xx.h"
#include "hotswap.h"
#include "utils.h"
#include "fru.h"
#include "led.h"

/* payload states
 *   0 - no power
 *   1 - power switching on
 *       Power Up sequence
 *
 *   2 - power good wait
 *       Since power supply switching
 *       Until detect power good
 *
 *   3 - power good
 *       Here you can configure devices such as clock crossbar and others
 *       We have to reset pin state program b
 *
 *   4 - fpga booting
 *       Since DCDC converters initialization
 *       Until FPGA DONE signal
 *       about 30 sec
 *
 *   5 - fpga working
 *
 *   6 - power switching off
 *       Power-off sequence
 *
 *   7 - power QUIESCED
 *       It continues until a power outage on the line 12v
 *       or for 30 seconds (???)
 *
 * 255 - power fail
 */

static void check_fpga_reset( void )
{
    static TickType_t edge_time;
    static uint8_t reset_lock;
    static uint8_t last_state = 1;

    TickType_t diff;
    TickType_t cur_time = xTaskGetTickCount();

    uint8_t cur_state = gpio_read_pin( PIN_PORT(GPIO_FRONT_BUTTON), PIN_NUMBER(GPIO_FRONT_BUTTON));

    if ( (cur_state == 0) && (last_state == 1) ) {
        /* Detects the falling edge of the front panel button */
        edge_time = cur_time;
        reset_lock = 0;
    }

    diff = getTickDifference( cur_time, edge_time );

    if ( (diff > pdMS_TO_TICKS(2000)) && (reset_lock == 0) && (cur_state == 0) ) {
        gpio_set_pin_low( PIN_PORT(GPIO_FPGA_RESET), PIN_NUMBER(GPIO_FPGA_RESET) );
        asm("NOP");
        gpio_set_pin_high( PIN_PORT(GPIO_FPGA_RESET), PIN_NUMBER(GPIO_FPGA_RESET) );

        /* If the user continues to press the button after the 2s, prevent this action to be repeated */
        reset_lock = 1;

        /* Blink RED LED to indicate to the user that the Reset was performed */
        LEDUpdate( FRU_AMC, LED1, LEDMODE_LAMPTEST, LEDINIT_ON, 5, 0 );
    }

    last_state = cur_state;
}

/**
 * @brief Set AFC's DCDC Converters state
 *
 * @param on DCDCs state
 *
 * @warning The FMC1_P12V DCDC is not affected by this function since it has to be always on in order to measure the Payload power status on the AFC board.
 */
void setDC_DC_ConvertersON( bool on )
{
    gpio_set_pin_state( PIN_PORT(GPIO_EN_FMC1_PVADJ), PIN_NUMBER(GPIO_EN_FMC1_PVADJ), on );
    //gpio_set_pin_state( PIN_PORT(GPIO_EN_FMC1_P12V), PIN_NUMBER(GPIO_EN_FMC1_P12V), on );
    gpio_set_pin_state( PIN_PORT(GPIO_EN_FMC1_P3V3), PIN_NUMBER(GPIO_EN_FMC1_P3V3), on );

    gpio_set_pin_state( PIN_PORT(GPIO_EN_FMC2_PVADJ), PIN_NUMBER(GPIO_EN_FMC2_PVADJ), on );
    gpio_set_pin_state( PIN_PORT(GPIO_EN_FMC2_P12V), PIN_NUMBER(GPIO_EN_FMC2_P12V), on );
    gpio_set_pin_state( PIN_PORT(GPIO_EN_FMC2_P3V3), PIN_NUMBER(GPIO_EN_FMC2_P3V3), on );

    gpio_set_pin_state( PIN_PORT(GPIO_EN_P1V0), PIN_NUMBER(GPIO_EN_P1V0), on );
    gpio_set_pin_state( PIN_PORT(GPIO_EN_P1V8), PIN_NUMBER(GPIO_EN_P1V8), on ); // <- this one causes problems if not switched off before power loss
    gpio_set_pin_state( PIN_PORT(GPIO_EN_P1V2), PIN_NUMBER(GPIO_EN_P1V2), on );
    gpio_set_pin_state( PIN_PORT(GPIO_EN_P1V5_VTT), PIN_NUMBER(GPIO_EN_P1V5_VTT), on );
    gpio_set_pin_state( PIN_PORT(GPIO_EN_P3V3), PIN_NUMBER(GPIO_EN_P3V3), on );
}

#ifdef MODULE_DAC_AD84XX
void set_vadj_volt( uint8_t fmc_slot, float v )
{
    uint32_t res_total;
    uint32_t res_dac;

    res_total = (uint32_t) (1162.5/(v-0.775)) - 453;
    res_dac = (1800*res_total)/(1800-res_total);

    dac_ad84xx_set_res( fmc_slot, res_dac );
}
#endif

EventGroupHandle_t amc_payload_evt = NULL;
#ifdef MODULE_RTM
EventGroupHandle_t rtm_payload_evt = NULL;
#endif

void payload_send_message( uint8_t fru_id, EventBits_t msg)
{
    if ( (fru_id == FRU_AMC) && amc_payload_evt ) {
        xEventGroupSetBits( amc_payload_evt, msg );
#ifdef MODULE_RTM
    } else if ( (fru_id == FRU_RTM) && rtm_payload_evt ) {
        xEventGroupSetBits( rtm_payload_evt, msg );
#endif
    }
}

TaskHandle_t vTaskPayload_Handle;

void payload_init( void )
{

#ifndef BENCH_TEST
    /* Wait until ENABLE# signal is asserted ( ENABLE == 0) */
    while ( gpio_read_pin( PIN_PORT(GPIO_MMC_ENABLE), PIN_NUMBER(GPIO_MMC_ENABLE) ) == 1 ) {};
#endif

    xTaskCreate( vTaskPayload, "Payload", 120, NULL, tskPAYLOAD_PRIORITY, &vTaskPayload_Handle );

    amc_payload_evt = xEventGroupCreate();
#ifdef MODULE_RTM
    rtm_payload_evt = xEventGroupCreate();
#endif

#ifdef MODULE_DAC_AD84XX
    /* Configure the PVADJ DAC */
    dac_ad84xx_init();
    set_vadj_volt( 0, 2.5 );
    set_vadj_volt( 1, 2.5 );
#endif

    gpio_set_pin_state( PIN_PORT(GPIO_FPGA_RESET), PIN_NUMBER(GPIO_FPGA_RESET), GPIO_LEVEL_HIGH );
}

void vTaskPayload( void *pvParameters )
{
    uint8_t state = PAYLOAD_NO_POWER;
    uint8_t new_state = PAYLOAD_STATE_NO_CHANGE;

    /* Payload power good flag */
    uint8_t PP_good = 0;

    /* Payload DCDCs good flag */
    uint8_t DCDC_good = 0;

    uint8_t QUIESCED_req = 0;
    EventBits_t current_evt;

    extern sensor_t * hotswap_amc_sensor;

    TickType_t xLastWakeTime;
    xLastWakeTime = xTaskGetTickCount();

    gpio_set_pin_state( PIN_PORT(GPIO_FPGA_PROGRAM_B), PIN_NUMBER(GPIO_FPGA_PROGRAM_B), GPIO_LEVEL_HIGH );

    for ( ;; ) {

        check_fpga_reset();

        /* Initialize one of the FMC's DCDC so we can measure when the Payload Power is present */
        gpio_set_pin_state( PIN_PORT(GPIO_EN_FMC1_P12V), PIN_NUMBER(GPIO_EN_FMC1_P12V), GPIO_LEVEL_HIGH );

        new_state = state;

        current_evt = xEventGroupGetBits( amc_payload_evt );

        if ( current_evt & PAYLOAD_MESSAGE_PPGOOD ) {
            PP_good = 1;
            xEventGroupClearBits( amc_payload_evt, PAYLOAD_MESSAGE_PPGOOD );
        }
        if ( current_evt & PAYLOAD_MESSAGE_PPGOODn ) {
            PP_good = 0;
            xEventGroupClearBits( amc_payload_evt, PAYLOAD_MESSAGE_PPGOODn );
        }
        if ( current_evt & PAYLOAD_MESSAGE_DCDC_PGOOD ) {
            DCDC_good = 1;
            xEventGroupClearBits( amc_payload_evt, PAYLOAD_MESSAGE_DCDC_PGOOD );
        }
        if ( current_evt & PAYLOAD_MESSAGE_DCDC_PGOODn ) {
            DCDC_good = 0;
            xEventGroupClearBits( amc_payload_evt, PAYLOAD_MESSAGE_DCDC_PGOODn );
        }
        if ( current_evt & PAYLOAD_MESSAGE_QUIESCED ) {
            QUIESCED_req = 1;
            xEventGroupClearBits( amc_payload_evt, PAYLOAD_MESSAGE_QUIESCED );
        }
        if ( current_evt & PAYLOAD_MESSAGE_COLD_RST ) {
            state = PAYLOAD_SWITCHING_OFF;
            xEventGroupClearBits( amc_payload_evt, PAYLOAD_MESSAGE_COLD_RST );
        }
        if ( current_evt & PAYLOAD_MESSAGE_REBOOT ) {
            gpio_set_pin_low( PIN_PORT(GPIO_FPGA_RESET), PIN_NUMBER(GPIO_FPGA_RESET) );
            asm("NOP");
            gpio_set_pin_high( PIN_PORT(GPIO_FPGA_RESET), PIN_NUMBER(GPIO_FPGA_RESET) );
            xEventGroupClearBits( amc_payload_evt, PAYLOAD_MESSAGE_REBOOT );
        }

        DCDC_good = gpio_read_pin( PIN_PORT(GPIO_DCDC_PGOOD), PIN_NUMBER(GPIO_DCDC_PGOOD) );

        switch(state) {

        case PAYLOAD_NO_POWER:

            if (PP_good) {
                new_state = PAYLOAD_POWER_GOOD_WAIT;
            }
            QUIESCED_req = 0;
            break;

        case PAYLOAD_POWER_GOOD_WAIT:
            /* Turn DDC converters on */
            setDC_DC_ConvertersON( true );

            /* Clear hotswap sensor backend power failure bits */
            hotswap_clear_mask_bit( HOTSWAP_AMC, HOTSWAP_BACKEND_PWR_SHUTDOWN_MASK );
            hotswap_clear_mask_bit( HOTSWAP_AMC, HOTSWAP_BACKEND_PWR_FAILURE_MASK );

            if ( QUIESCED_req || ( PP_good == 0 ) ) {
                new_state = PAYLOAD_SWITCHING_OFF;
            } else if ( DCDC_good == 1 ) {
                new_state = PAYLOAD_STATE_FPGA_SETUP;
            }
            break;

        case PAYLOAD_STATE_FPGA_SETUP:
#ifdef MODULE_ADN4604
            adn4604_init();
#endif
            new_state = PAYLOAD_FPGA_BOOTING;
            break;

        case PAYLOAD_FPGA_BOOTING:
            if ( QUIESCED_req == 1 || PP_good == 0 || DCDC_good == 0 ) {
                new_state = PAYLOAD_SWITCHING_OFF;
            }
            break;

        case PAYLOAD_SWITCHING_OFF:
            setDC_DC_ConvertersON( false );

            if ( QUIESCED_req ) {
                hotswap_set_mask_bit( HOTSWAP_AMC, HOTSWAP_QUIESCED_MASK );
                if ( hotswap_send_event( hotswap_amc_sensor, HOTSWAP_STATE_QUIESCED ) == ipmb_error_success ) {
                    QUIESCED_req = 0;
                    hotswap_clear_mask_bit( HOTSWAP_AMC, HOTSWAP_QUIESCED_MASK );
                    new_state = PAYLOAD_NO_POWER;
                }
            } else {
                new_state = PAYLOAD_NO_POWER;
            }
            /* Reset the power good flags to avoid the state machine to start over without a new read from the sensors */
            PP_good = 0;
            DCDC_good = 0;
            break;

        default:
            break;
        }

        state = new_state;
        vTaskDelayUntil( &xLastWakeTime, PAYLOAD_BASE_DELAY );
    }
}


/* HPM Functions */
#ifdef MODULE_HPM

#include "flash_spi.h"
#include "string.h"

uint8_t hpm_page[256];
uint8_t hpm_pg_index;
uint32_t hpm_page_addr;

uint8_t payload_hpm_prepare_comp( void )
{
    /* Initialize variables */
    memset(hpm_page, 0xFF, sizeof(hpm_page));
    hpm_pg_index = 0;
    hpm_page_addr = 0;

    /* Initialize flash */
    ssp_init( FLASH_SPI, FLASH_SPI_BITRATE, FLASH_SPI_FRAME_SIZE, SSP_MASTER, SSP_INTERRUPT );

    /* Prevent the FPGA from accessing the Flash to configure itself now */
    gpio_set_pin_state( PIN_PORT(GPIO_FPGA_PROGRAM_B), PIN_NUMBER(GPIO_FPGA_PROGRAM_B), GPIO_LEVEL_HIGH );
    gpio_set_pin_state( PIN_PORT(GPIO_FPGA_PROGRAM_B), PIN_NUMBER(GPIO_FPGA_PROGRAM_B), GPIO_LEVEL_LOW );
    gpio_set_pin_state( PIN_PORT(GPIO_FPGA_PROGRAM_B), PIN_NUMBER(GPIO_FPGA_PROGRAM_B), GPIO_LEVEL_HIGH );
    gpio_set_pin_state( PIN_PORT(GPIO_FPGA_PROGRAM_B), PIN_NUMBER(GPIO_FPGA_PROGRAM_B), GPIO_LEVEL_LOW );

    /* Erase FLASH */
    flash_bulk_erase();

    return IPMI_CC_COMMAND_IN_PROGRESS;
}

uint8_t payload_hpm_upload_block( uint8_t * block, uint16_t size )
{
    /* TODO: Check DONE pin before accessing the SPI bus, since the FPGA may be reading it in order to boot */
    uint8_t remaining_bytes_start;

    if ( sizeof(hpm_page) - hpm_pg_index > size ) {
        /* Our page is not full yet, just append the new data */
        memcpy(&hpm_page[hpm_pg_index], block, size);
        hpm_pg_index += size;

        return IPMI_CC_OK;

    } else {
        /* Complete the remaining bytes on the buffer */
        memcpy(&hpm_page[hpm_pg_index], block, (sizeof(hpm_page) - hpm_pg_index));
        remaining_bytes_start = (sizeof(hpm_page) - hpm_pg_index);

        /* Program the complete page in the Flash */
        flash_program_page( hpm_page_addr, &hpm_page[0], sizeof(hpm_page));

        hpm_page_addr += sizeof(hpm_page);

        /* Empty our buffer and reset the index */
        memset(hpm_page, 0xFF, sizeof(hpm_page));
        hpm_pg_index = 0;

        /* Save the trailing bytes */
        memcpy(&hpm_page[hpm_pg_index], block+remaining_bytes_start, size-remaining_bytes_start);

        hpm_pg_index = size-remaining_bytes_start;

        return IPMI_CC_COMMAND_IN_PROGRESS;
    }
}

uint8_t payload_hpm_finish_upload( uint32_t image_size )
{
    /* Check if the last page was already programmed */
    if (!hpm_pg_index) {
        /* Program the complete page in the Flash */
        flash_program_page( hpm_page_addr, &hpm_page[0], (sizeof(hpm_page)-hpm_pg_index));
        hpm_pg_index = 0;
        hpm_page_addr = 0;

        return IPMI_CC_COMMAND_IN_PROGRESS;
    }

    return IPMI_CC_OK;
}

uint8_t payload_hpm_get_upgrade_status( void )
{
    if (is_flash_busy()) {
        return IPMI_CC_COMMAND_IN_PROGRESS;
    } else {
        return IPMI_CC_OK;
    }
}

uint8_t payload_hpm_activate_firmware( void )
{
    /* Reset FPGA - Pulse PROGRAM_B pin */
    gpio_set_pin_state( PIN_PORT(GPIO_FPGA_PROGRAM_B), PIN_NUMBER(GPIO_FPGA_PROGRAM_B), GPIO_LEVEL_LOW);
    gpio_set_pin_state( PIN_PORT(GPIO_FPGA_PROGRAM_B), PIN_NUMBER(GPIO_FPGA_PROGRAM_B), GPIO_LEVEL_HIGH);

    return IPMI_CC_OK;
}
#endif
