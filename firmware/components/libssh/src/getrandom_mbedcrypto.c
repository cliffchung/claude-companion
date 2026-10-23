/*
 * This file is part of the SSH Library
 *
 * Copyright (c) 2017 Sartura d.o.o.
 *
 * Author: Juraj Vijtiuk <juraj.vijtiuk@sartura.hr>
 *
 * The SSH Library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or (at your
 * option) any later version.
 *
 * The SSH Library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with the SSH Library; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330, Boston,
 * MA 02111-1307, USA.
 */

#include "libssh_esp32_config.h"

#include "libssh/crypto.h"
#include "mbedcrypto-compat.h"

mbedtls_ctr_drbg_context ssh_mbedtls_ctr_drbg;

/* Tab5 port: the DRBG is shared by sessions on different tasks. mbedtls_ctr_drbg_random()
 * takes the context's own mutex (MBEDTLS_THREADING_C); the strong path flips the
 * prediction-resistance flag, so it holds that same mutex across flip, draw and flip back
 * and draws with mbedtls_ctr_drbg_random_with_add(), which does not lock. */
#if !defined(MBEDTLS_THREADING_C)
#error "libssh on the Tab5 needs CONFIG_MBEDTLS_THREADING_C: its DRBG is shared across tasks"
#endif
#include <mbedtls/threading.h>

int
ssh_mbedtls_random(void *where, int len, int strong)
{
    int rc = 0;
    if (strong) {
        if (mbedtls_mutex_lock(&ssh_mbedtls_ctr_drbg.MBEDTLS_PRIVATE(mutex)) != 0) {
            return 0;
        }
        mbedtls_ctr_drbg_set_prediction_resistance(&ssh_mbedtls_ctr_drbg,
                                                   MBEDTLS_CTR_DRBG_PR_ON);
        rc = mbedtls_ctr_drbg_random_with_add(&ssh_mbedtls_ctr_drbg, where, len, NULL, 0);
        mbedtls_ctr_drbg_set_prediction_resistance(&ssh_mbedtls_ctr_drbg,
                                                   MBEDTLS_CTR_DRBG_PR_OFF);
        mbedtls_mutex_unlock(&ssh_mbedtls_ctr_drbg.MBEDTLS_PRIVATE(mutex));
    } else {
        rc = mbedtls_ctr_drbg_random(&ssh_mbedtls_ctr_drbg, where, len);
    }

    return !rc;
}

int
ssh_get_random(void *where, int len, int strong)
{
    return ssh_mbedtls_random(where, len, strong);
}
