
#ifndef _USBCFG_H_
#define _USBCFG_H_


// ST-LINK/V2 VID and PID
#define BDLINK_VID                      0x0483
#define BDLINK_PID                      0x3748

/*
 * Endpoints to be used for USBD1.
 */
#define USBD1_STLINK_TX_EP              1
#define USBD1_STLINK_RX_EP              2
#define USBD1_STLINK_TRACE_EP           3

extern const USBConfig usbcfg;

#endif  /* _USBCFG_H_ */

/** @} */
