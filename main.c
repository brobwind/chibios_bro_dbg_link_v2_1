
#include <stdio.h>
#include <string.h>

#include "ch.h"
#include "hal.h"
#include "chprintf.h"

#include "usbcfg.h"
#include "bro_aes.h"


#define MIN(a, b) ((a) <= (b) ? (a) : (b))


/*===========================================================================*/
/* On-chip Flash operation                                                   */
/*===========================================================================*/

static void setupFlash(void) {
    /* Configure the HSI oscillator */
    RCC->CR |= RCC_CR_HSION;

    /* Wait for it to come on */
    while (!(RCC->CR & RCC_CR_HSIRDY)) {}
}

static bool flashErasePage(uint32_t pageAddr) {
    FLASH->CR = FLASH_CR_PER;

    while (FLASH->SR & FLASH_SR_BSY) {}
    FLASH->AR = pageAddr;
    FLASH->CR = FLASH_CR_STRT | FLASH_CR_PER;
    while (FLASH->SR & FLASH_SR_BSY) {}

    /* TODO: verify the page was erased */

    FLASH->CR = 0x00;

    return TRUE;
}

static void flashLock(void) {
    /* take down the HSI oscillator? it may be in use elsewhere */

    /* Ensure all FPEC functions disabled and lock the FPEC */
    FLASH->CR = FLASH_CR_LOCK;
}

static void flashUnlock(void) {
    /* Unlock the flash */
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

static bool flashWriteWord(uint32_t addr, uint32_t word) {
    uint32_t rwmVal = FLASH->CR;

    FLASH->CR = FLASH_CR_PG;

    /* Apparently we need not write to FLASH_AR and can
       simply do a native write of a half word */
    while (FLASH->SR & FLASH_SR_BSY) {}
    *(uint16_t *)(addr + 0) = (word >>  0) & 0xffff;
    while (FLASH->SR & FLASH_SR_BSY) {}
    *(uint16_t *)(addr + 2) = (word >> 16) & 0xffff;
    while (FLASH->SR & FLASH_SR_BSY) {}

    FLASH->CR = rwmVal & 0xfffffffe;

    /* Verify the write */
    if (*(uint32_t *)addr != word) {
        return FALSE;
    }

    return TRUE;
}

/*===========================================================================*/
/* USB DFU                                                                   */
/*===========================================================================*/

#define DFU_STATE_RDY            0x00
#define DFU_STATE_RUN            0x01
#define DFU_STATE_STP            0x02
#define DFU_STATE_ART            0x04
#define DFU_STATE_BZY            0x10
#define DFU_STATE_ERR            0x20

uint8_t dfu_state = DFU_STATE_RDY;
uint8_t dfu_command[16 + 1024];

static SEMAPHORE_DECL(dfu_cmd_sem, 0);
static SEMAPHORE_DECL(dfu_cmd_sem_action, 0);
static MUTEX_DECL(dfu_cmd_mtx);


static THD_WORKING_AREA(waDfuCmd, 2048);
static __attribute__((noreturn)) THD_FUNCTION(DfuCmd, arg) {
  (void)arg;
  uint8_t rxbuf[16], txbuf[32];

  chRegSetThreadName("DfuCmd");
  while (true) {
    msg_t msg = usbReceive(&USBD1, USBD1_STLINK_RX_EP, rxbuf, sizeof(rxbuf));
    if (msg == MSG_RESET) {
        chThdSleepMilliseconds(500);
        chMtxLock(&dfu_cmd_mtx);
        dfu_state = DFU_STATE_RDY;
        chMtxUnlock(&dfu_cmd_mtx);
        continue;
    }

    /* Notify blink thread in data transmition */
    chSemSignal(&dfu_cmd_sem_action);

    if (!memcmp(rxbuf, "\xf1\x80\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 16)) {
        volatile uint8_t *p = (volatile uint8_t *)(FLASH_BASE + 16 * 1024 - 2);

        txbuf[0] = p[0];
        txbuf[1] = p[1];

        // Fill with VID
        txbuf[2] = (BDLINK_VID >> 0) & 0xff;
        txbuf[3] = (BDLINK_VID >> 8) & 0xff;
        // Fill with PID
        txbuf[4] = (BDLINK_PID >> 0) & 0xff;
        txbuf[5] = (BDLINK_PID >> 8) & 0xff;

        usbTransmit(&USBD1, USBD1_STLINK_TX_EP, txbuf, 6);
    } else if (!memcmp(rxbuf, "\xf5\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 16)) {
        txbuf[0] = 0x00;
        txbuf[1] = 0x02;

        usbTransmit(&USBD1, USBD1_STLINK_TX_EP, txbuf, 2);
    } else if (!memcmp(rxbuf, "\xf3\x08\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 16)) {
        volatile uint32_t *p;

        txbuf[0] = 0x80; txbuf[1] = 0x00; txbuf[2] = 0xff; txbuf[3] = 0xff;

        // blank configure area: 06 40 05 49
        // stm32 only:           4a 06 40 05
        // stm32 msd + vcp:      42 06 40 05
        txbuf[4] = 0x42; txbuf[5] = 0x06; txbuf[6] = 0x40; txbuf[7] = 0x05;

        p = (volatile uint32_t *)0x1FFFF7E8;
        *(uint32_t *)(txbuf +  8) = p[0];
        *(uint32_t *)(txbuf + 12) = p[1];
        *(uint32_t *)(txbuf + 16) = p[2];

        usbTransmit(&USBD1, USBD1_STLINK_TX_EP, txbuf, 20);
    } else if (!memcmp(rxbuf, "\xf3\x0a\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 16)) {
        memset(txbuf, 0x00, 16);
        // For ST-LINK/V2 or ST-LINK/V2-1:
        // - PC13 Pull down with 10K Resistor
        // - PC14 Floating
        if (palReadPad(GPIOC, 13) == 0 && palReadPad(GPIOC, 14) == 1) {
            txbuf[3] = 0x21;
        }

        usbTransmit(&USBD1, USBD1_STLINK_TX_EP, txbuf, 16);
    } else if (!memcmp(rxbuf, "\xf3\x03\x00\x00", 4) && msg == 16) {
        chMtxLock(&dfu_cmd_mtx);
        switch (dfu_state) {
        case DFU_STATE_RDY:
            memcpy(txbuf, "\x00\x00\x00\x00\x02\x00", 6);
            break;
        case DFU_STATE_STP | DFU_STATE_BZY:
            dfu_state &= 0x0F;
        case DFU_STATE_RUN:
        case DFU_STATE_ART:
            memcpy(txbuf, "\x00\x50\x00\x00\x04\x00", 6);
            break;
        case DFU_STATE_STP:
            memcpy(txbuf, "\x00\x00\x00\x00\x05\x00", 6);
            break;
        default: // DFU_STATE_ERR
            memcpy(txbuf, "\x00\x50\x00\x00\x04\x00", 6);
            break;
        }
        chMtxUnlock(&dfu_cmd_mtx);

        usbTransmit(&USBD1, USBD1_STLINK_TX_EP, txbuf, 6);
    } else if (rxbuf[0] == 0xf3 && rxbuf[1] == 0x01) {
        uint16_t len = rxbuf[6] | rxbuf[7] << 8;
        uint8_t *p;
        uint16_t size;

        if ((dfu_state & DFU_STATE_BZY) == 0 && len <= sizeof(dfu_command) - 16) {
            memcpy(dfu_command, rxbuf, 16);
            p       = dfu_command + 16;
            size    = sizeof(dfu_command) - 16;
        } else {
            chMtxLock(&dfu_cmd_mtx);
            dfu_state |= DFU_STATE_ERR;
            chMtxUnlock(&dfu_cmd_mtx);

            p       = rxbuf;
            size    = sizeof(rxbuf);
        }
        do {
            msg = usbReceive(&USBD1, USBD1_STLINK_RX_EP, p, MIN(size, len));
            if (msg <= 0) {
                chMtxLock(&dfu_cmd_mtx);
                dfu_state = DFU_STATE_RDY;
                chMtxUnlock(&dfu_cmd_mtx);
                break;
            }

            p       += MIN(size, len);
            len     -= (uint16_t)msg;
        } while (len > 0);

        if (dfu_state == DFU_STATE_RDY && len > 0) continue;

        chMtxLock(&dfu_cmd_mtx);
        dfu_state = DFU_STATE_RUN;
        chMtxUnlock(&dfu_cmd_mtx);

        chSemSignal(&dfu_cmd_sem);
    } else if (!memcmp(rxbuf, "\xf3\x09\x16\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00", 16)) {
        volatile uint8_t *p = (volatile uint8_t *)(FLASH_BASE + 15 * 1024 + 0x30);
        uint8_t len = rxbuf[2];
        memcpy(txbuf, (void *)p, len);
        usbTransmit(&USBD1, USBD1_STLINK_TX_EP, txbuf, len);
    } else if (!memcmp(rxbuf, "\xf3\x07\x00\x00", 4) && msg == 16) {
        // Exit DFU mode
        usbDisconnectBus(&USBD1);
        chThdSleepMilliseconds(1500);

        BKP->DR1 = 0xfeed;

        NVIC_SystemReset();
    }
  }
}

static THD_WORKING_AREA(waDfuWorker, 2048);
static __attribute__((noreturn)) THD_FUNCTION(DfuWorker, arg) {
    (void)arg;
    uint32_t location;

    chRegSetThreadName("DfuWorker");
    while (true) {
        uint16_t seq, checksum, len, idx, tmp;

        chSemWait(&dfu_cmd_sem);

        if (dfu_command[0] == 0xf3 && dfu_command[1] == 0x01) {
            seq         = dfu_command[2] | dfu_command[3] << 8;
            checksum    = dfu_command[4] | dfu_command[5] << 8;
            len         = dfu_command[6] | dfu_command[7] << 8;
            if ((seq & 0x06) != 0) {
                const uint8_t salt[] = {
                    0x29, 0xf1, 0x95, 0x64, 0xcc, 0xdb, 0xde, 0xf9,
                    0x3b, 0xd1, 0xe7, 0x7d, 0x8a, 0x89, 0xb9, 0xbf
                };
                uint8_t devuid[16] = {
                    0x80, 0x00, 0xff, 0xff,
                    'b', 'r', 'o', 'b',
                    'w', 'i', 'n', 'd',
                    '.', 'c', 'o', 'm'
                };

                uint8_t deckey[16];
                AES_KEY aes_key;

                AES_set_decrypt_key(devuid, 128, &aes_key);
                AES_decrypt(salt, deckey, &aes_key);

                AES_set_encrypt_key(deckey, 128, &aes_key);
                memcpy(devuid + 4, (void *)0x1FFFF7E8, 12);
                AES_encrypt(devuid, deckey, &aes_key);
                AES_set_decrypt_key(deckey, 128, &aes_key);

                for (idx = 0; idx < len; idx += 16) {
                    AES_decrypt(dfu_command + 16 + idx, dfu_command + 16 + idx, &aes_key);
                }
            }
            for (idx = 0, tmp = 0; idx < len; idx++) {
                tmp += dfu_command[16 + idx];
            }

            if (tmp != checksum) { // Checksum mismatch
                chMtxLock(&dfu_cmd_mtx);
                dfu_state = DFU_STATE_ART;
                chMtxUnlock(&dfu_cmd_mtx);
                continue;
            }

            if (seq == 0x0000 && len == 0x0005) { // Location or erase command
                if (dfu_command[16] == 0x21 || dfu_command[16] == 0x41) {
                    location = dfu_command[16 + 1] <<  0 |
                            dfu_command[16 + 2] <<  8 |
                            dfu_command[16 + 3] << 16 |
                            dfu_command[16 + 4] << 24;
                }
                if (dfu_command[16] == 0x41) { // Erase flash block
                    // 1. Setup flash clock
                    setupFlash();
                    // 2. Unlock flash
                    flashUnlock();
                    // 3. Erase flash block
                    flashErasePage(location);
                    // 4. Lock flash
                    flashLock();
                }
            }
            else if ((seq & 0x06) != 0) {
                // 1. Unlock flash
                flashUnlock();

                // 2. Write data to flash
                for (idx = 0; idx < len; idx += 4) {
                    uint32_t word, read;
                    word = dfu_command[16 + idx + 0] <<  0 |
                        dfu_command[16 + idx + 1] <<  8 |
                        dfu_command[16 + idx + 2] << 16 |
                        dfu_command[16 + idx + 3] << 24;
                    read = flashWriteWord(location + idx, word);
                    if (read != TRUE) {
                    }
                }

                // 3. Lock flash
                flashLock();
            }
        }

        chMtxLock(&dfu_cmd_mtx);
        dfu_state = DFU_STATE_STP | DFU_STATE_BZY;
        chMtxUnlock(&dfu_cmd_mtx);
    }
}

/*===========================================================================*/
/* Generic code.                                                             */
/*===========================================================================*/

/*
 * Blinker thread, times are in milliseconds.
 */
static THD_WORKING_AREA(waLedBlinker, 32);
static __attribute__((noreturn)) THD_FUNCTION(LedBlinker, arg) {
  (void)arg;
  chRegSetThreadName("blinker");
  while (true) {
    systime_t time = USBD1.state == USB_ACTIVE ? 250 : 500;
    chSemWaitTimeout(&dfu_cmd_sem_action, MS2ST(time));
    palTogglePad(GPIOA, GPIOA_LED);
  }
}

void JumpToUserApp(uint32_t pAppAddr) {
  volatile uint32_t *pMspAddr;
  volatile uint32_t *pJmpAddr;

  /* Get main stack address from the application vector table */
  pMspAddr = (volatile uint32_t *)(pAppAddr + 0);
  /* Get jump address from application vector table */
  pJmpAddr = (volatile uint32_t *)(pAppAddr + 4);

  /* Set stack pointer as in application's vector table */
  __set_MSP(*pMspAddr);

  /* Privileged and using main stack */
  __set_CONTROL(0);

  /* Jump to the new application */
  (*(void (*)(void))*pJmpAddr)();
}

/*
 * Application entry point.
 */
int __attribute__((noreturn)) main(void) {
  /*
   * System initializations.
   * - HAL initialization, this also initializes the configured device drivers
   *   and performs the board-specific initializations.
   */
  halInit();

  do {
    uint32_t flashSize, magicValue;

    /* Check the firmware intergrity */
    flashSize = (*(volatile uint32_t *)0x1FFFF7E0 & 0xffff) << 10;
    /* Magic value locate at the last 4 bytes in the flash */
    magicValue = *(volatile uint32_t *)(flashSize - 4);
    if (magicValue != 0xa50027d3) {
      break;
    }

    /* Check power on reason */
    if ((RCC->CSR & RCC_CSR_SFTRSTF) != 0 && BKP->DR1 != 0xfeed) {
      /* Software reset occurred */
      break;
    }

    if (BKP->DR1 == 0xfeed) BKP->DR1 = 0x0000;

    /* Clear reset flag */
    RCC->CSR |= RCC_CSR_RMVF;

    JumpToUserApp(0x08004000);
  } while (0);

  /*
   * System initializations.
   * - Kernel initialization, the main() function becomes a thread and the
   *   RTOS is active.
   */
  chSysInit();

  /*
   * Activates the serial driver 2 using the driver default configuration.
   */
  sdStart(&SD2, NULL);

  /*
   * Activates the USB driver and then the USB bus pull-up on D+.
   * Note, a delay is inserted in order to not have to disconnect the cable
   * after a reset.
   */
  usbDisconnectBus(&USBD1);
  chThdSleepMilliseconds(1500);
  usbStart(&USBD1, &usbcfg);
  usbConnectBus(&USBD1);

  /*
   * Creates the blinker thread.
   */
  chThdCreateStatic(waLedBlinker, sizeof(waLedBlinker), NORMALPRIO, LedBlinker, NULL);

  chSemObjectInit(&dfu_cmd_sem, 0);
  chSemObjectInit(&dfu_cmd_sem_action, 0);
  chMtxObjectInit(&dfu_cmd_mtx);

  /*
   * Starting threads.
   */
  chThdCreateStatic(waDfuCmd, sizeof(waDfuCmd), NORMALPRIO, DfuCmd, NULL);
  chThdCreateStatic(waDfuWorker, sizeof(waDfuWorker), NORMALPRIO, DfuWorker, NULL);

  /*
   * Normal main() thread activity, in this demo it does nothing except
   * sleeping in a loop and check the button state.
   */
  while (true) {
    chThdSleepMilliseconds(1000);
  }
}
