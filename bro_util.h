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

#ifndef __BRO_UTIL_H__
#define __BRO_UTIL_H__

#include "ch.h"
#include "hal.h"

int bdprintf(mutex_t *mtx, SerialDriver *sd, const char *fmt, ...);
void hexdump(mutex_t *mtx, SerialDriver *chp, void *address_, int byte_count_, int show_actual_addresses_, const char *prefix_);

#endif
