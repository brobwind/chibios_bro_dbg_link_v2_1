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

static void __attribute__((unused)) setupFlash(void) {
    /* Configure the HSI oscillator */
    RCC->CR |= RCC_CR_HSION;

    /* Wait for it to come on */
    while (!(RCC->CR & RCC_CR_HSIRDY)) {}
}

static bool __attribute__((unused)) flashErasePage(uint32_t pageAddr) {
    FLASH->CR = FLASH_CR_PER;

    while (FLASH->SR & FLASH_SR_BSY) {}
    FLASH->AR = pageAddr;
    FLASH->CR = FLASH_CR_STRT | FLASH_CR_PER;
    while (FLASH->SR & FLASH_SR_BSY) {}

    /* TODO: verify the page was erased */

    FLASH->CR = 0x00;

    return TRUE;
}

static void __attribute__((unused)) flashLock(void) {
    /* take down the HSI oscillator? it may be in use elsewhere */

    /* Ensure all FPEC functions disabled and lock the FPEC */
    FLASH->CR = FLASH_CR_LOCK;
}

static void __attribute__((unused)) flashUnlock(void) {
    /* Unlock the flash */
    if (FLASH->CR & FLASH_CR_LOCK) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

static bool __attribute__((unused)) flashWriteWord(uint32_t addr, uint32_t word) {
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
  uint8_t rxbuf[16];

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

    if (!memcmp(rxbuf, "\xf3\x07\x00\x00", 4) && msg == 16) {
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

    chRegSetThreadName("DfuWorker");
    while (true) {
        chSemWait(&dfu_cmd_sem);

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

#ifdef ST_LINK_V2_1
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
#else
  JumpToUserApp(0x08004000);
#endif

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
