/*
Copyright 2022 Sonya Sireneva

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 2 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include "modem.h"

#define POLYNOMIAL 0x8408

#define ROT0(x) ((((x)) >> 1) ^ ((((x) & 1) ? POLYNOMIAL : 0)))
#define ROT(x) ((uint16_t)ROT0((uint16_t)(x)))
#define ROT8(x) ROT(ROT(ROT(ROT(ROT(ROT(ROT(ROT(x))))))))

#define P1(x) ROT8(x)
#define P4(x) P1(x), P1(x+1), P1(x+2), P1(x+3)
#define P16(x) P4(x), P4(x+4), P4(x+8), P4(x+12)
#define P64(x) P16(x), P16(x+16), P16(x+32), P16(x+48)

const static uint16_t crc_table[256] = {P64(0), P64(64), P64(128), P64(192)};

uint16_t ppp_crc(const char* data, size_t size)
{
    uint16_t crc = -1;
    for(size_t i = 0; i < size; i++)
        crc = (crc >> 8) ^ crc_table[(uint8_t)data[i] ^ (uint8_t)crc];
    return crc;
}
