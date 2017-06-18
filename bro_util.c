/*
 * Copyright (C) 2017 https://www.brobwind.com
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "ch.h"
#include "hal.h"
#include "chprintf.h"

#include "bro_util.h"


int bdprintf(mutex_t *mtx, SerialDriver *sd, const char *fmt, ...)
{
  va_list ap;
  int formatted_bytes;

  chMtxLock(mtx);
  va_start(ap, fmt);
  formatted_bytes = chvprintf((BaseSequentialStream *)sd, fmt, ap);
  va_end(ap);
  chMtxUnlock(mtx);

  return formatted_bytes;
}

// https://android.googlesource.com/platform/art/+/android-7.0.0_r7/runtime/base/hex_dump.cc
void hexdump(mutex_t *mtx, SerialDriver *chp, void *address_, int byte_count_, int show_actual_addresses_, const char *prefix_)
{
#define min(a, b) (a) > (b) ? (b) : (a)
    if (byte_count_ == 0) {
        return;
    }

    if (address_ == NULL) {
        bdprintf(mtx, chp, "00000000:");
        return;
    }

    static const char gHexDigit[] = "0123456789abcdef";
    const unsigned char* addr =(const unsigned char*)(address_);
    const int kBitsPerWord = 32;
    // 01234560: 00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff    0123456789abcdef
    char out[(kBitsPerWord / 4) + /* offset */
                     1 + /* colon */
                     (16 * 3) + /* 16 hex digits and space */
                     2 + /* white space */
                     16 + /* 16 characters*/
                     1 /* \0 */ ];
    size_t offset;        /* offset to show while printing */

    if (show_actual_addresses_) {
        offset =(size_t)(addr);
    } else {
        offset = 0;
    }
    memset(out, ' ', sizeof(out)-1);
    out[kBitsPerWord / 4] = ':';
    out[sizeof(out)-1] = '\0';

    size_t byte_count = byte_count_;
    size_t gap = offset & 0x0f;
    while (byte_count > 0) {
        size_t line_offset = offset & ~0x0f;

        char* hex = out;
        char* asc = out + (kBitsPerWord / 4) + /* offset */ 1 + /* colon */
                (16 * 3) + /* 16 hex digits and space */ 2 /* white space */;

        size_t i;
        for (i = 0; i < (kBitsPerWord / 4); i++) {
            *hex++ = gHexDigit[(line_offset >> (kBitsPerWord - 4)) & 0x0f];
            line_offset <<= 4;
        }
        hex++;
        hex++;

        size_t count = min(byte_count, 16 - gap);
        // CHECK_NE(count, 0U);
        // CHECK_LE(count + gap, 16U);

        if (gap) {
            /* only on first line */
            hex += gap * 3;
            asc += gap;
        }

        for (i = gap ; i < count + gap; i++) {
            *hex++ = gHexDigit[*addr >> 4];
            *hex++ = gHexDigit[*addr & 0x0f];
            hex++;
            if (*addr >= 0x20 && *addr < 0x7f /*isprint(*addr)*/) {
                *asc++ = *addr;
            } else {
                *asc++ = '.';
            }
            addr++;
        }
        for (; i < 16; i++) {
            /* erase extra stuff; only happens on last line */
            *hex++ = ' ';
            *hex++ = ' ';
            hex++;
            *asc++ = ' ';
        }

        prefix_ != NULL ? bdprintf(mtx, chp, "%s %s\r\n", prefix_, out): bdprintf(mtx, chp, "%s\r\n", out);

        gap = 0;
        byte_count -= count;
        offset += count;
    }
}
