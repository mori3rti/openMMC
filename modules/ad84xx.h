/*
 *   openMMC -- Open Source modular IPM Controller firmware
 *
 *   Copyright (C) 2015  Henrique Silva <henrique.silva@lnls.br>
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

#ifndef AD84XX_H_
#define AD84XX_H_

/**
 * @brief Initializes AD84XX DAC interface
 *
 * @note This module uses legacy SPI and controls SSEL as a GPIO
 */
void dac_vadj_init( void );

/**
 * @brief Configure the output value of AD84XX
 *
 * @param addr Selects which potentiometer will be set
 * @param val Value to be set
 */
void dac_vadj_config( uint8_t addr, uint8_t val );

#endif