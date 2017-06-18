#include "hal.h"
#include "usbcfg.h"


/*
 * USB Device Descriptor.
 */
static const uint8_t bdlink_device_descriptor_data[18] = {
  USB_DESC_DEVICE       (0x0200,        /* bcdUSB (2.0).                    */
                         0x00,          /* bDeviceClass.                    */
                         0x00,          /* bDeviceSubClass.                 */
                         0x00,          /* bDeviceProtocol.                 */
                         0x40,          /* bMaxPacketSize.                  */
                         BDLINK_VID,    /* idVendor.                        */
                         BDLINK_PID,    /* idProduct.                       */
                         0x0100,        /* bcdDevice.                       */
                         1,             /* iManufacturer.                   */
                         2,             /* iProduct.                        */
                         3,             /* iSerialNumber.                   */
                         1)             /* bNumConfigurations.              */
};

/*
 * Device Descriptor wrapper.
 */
static const USBDescriptor bdlink_device_descriptor = {
  sizeof bdlink_device_descriptor_data,
  bdlink_device_descriptor_data
};

/* Configuration Descriptor tree for a CDC.*/
static const uint8_t bdlink_configuration_descriptor_data[39] = {
  /* Configuration Descriptor.*/
  USB_DESC_CONFIGURATION(39,            /* wTotalLength.                    */
                         0x01,          /* bNumInterfaces.                  */
                         0x01,          /* bConfigurationValue.             */
                         0,             /* iConfiguration.                  */
                         0x80,          /* bmAttributes (self powered).     */
                         50),           /* bMaxPower (100mA).               */
  /* Interface Descriptor.*/
  USB_DESC_INTERFACE    (0x00,          /* bInterfaceNumber.                */
                         0x00,          /* bAlternateSetting.               */
                         0x03,          /* bNumEndpoints.                   */
                         0xff,          /* bInterfaceClass                  */
                         0xff,          /* bInterfaceSubClass               */
                         0xff,          /* bInterfaceProtocol               */
                         4),            /* iInterface.                      */
  /* Endpoint 1 Descriptor.*/
  USB_DESC_ENDPOINT     (USBD1_STLINK_TX_EP | 0x80,    /* bEndpointAddress. */
                         0x02,          /* bmAttributes (Bulk).             */
                         0x0040,        /* wMaxPacketSize.                  */
                         0x00),         /* bInterval.                       */
  /* Endpoint 2 Descriptor.*/
  USB_DESC_ENDPOINT     (USBD1_STLINK_RX_EP,           /* bEndpointAddress. */
                         0x02,          /* bmAttributes (Bulk).             */
                         0x0040,        /* wMaxPacketSize.                  */
                         0x00),         /* bInterval.                       */
  /* Endpoint 3 Descriptor.*/
  USB_DESC_ENDPOINT     (USBD1_STLINK_TRACE_EP | 0x80, /* bEndpointAddress. */
                         0x02,          /* bmAttributes (Bulk).             */
                         0x0040,        /* wMaxPacketSize.                  */
                         0x00)          /* bInterval.                       */
};

/*
 * Configuration Descriptor wrapper.
 */
static const USBDescriptor bdlink_configuration_descriptor = {
  sizeof bdlink_configuration_descriptor_data,
  bdlink_configuration_descriptor_data
};

/*
 * U.S. English language identifier.
 */
static const uint8_t bdlink_string0[] = {
  USB_DESC_BYTE(4),                     /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  USB_DESC_WORD(0x0409)                 /* wLANGID (U.S. English).          */
};

/*
 * Vendor string.
 */
static const uint8_t bdlink_string1[] = {
  USB_DESC_BYTE(26),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  'b', 0, 'r', 0, 'o', 0, 'b', 0, 'w', 0, 'i', 0, 'n', 0, 'd', 0,
  '.', 0, 'c', 0, 'o', 0, 'm', 0
};

/*
 * Device Description string.
 */
static const uint8_t bdlink_string2[] = {
  USB_DESC_BYTE(60),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  'B', 0, 'R', 0, 'O', 0, '-', 0, 'D', 0, 'B', 0, 'G', 0, '-', 0,
  'L', 0, 'I', 0, 'N', 0, 'K', 0, ' ', 0, '-', 0, ' ', 0, 'V', 0,
  '2', 0, '.', 0, '1', 0, '+', 0, 'B', 0, 'L', 0, '-', 0, '1', 0,
  '6', 0, '1', 0, '1', 0, '2', 0, '1', 0
};

/*
 * Serial Number string.
 */
static uint8_t bdlink_string3[2 + 16 * 3] = {
  USB_DESC_BYTE(50),                     /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
};

/*
 * Interface string.
 */
static const uint8_t bdlink_string4[] = {
  USB_DESC_BYTE(16),                    /* bLength.                         */
  USB_DESC_BYTE(USB_DESCRIPTOR_STRING), /* bDescriptorType.                 */
  'S', 0, 'T', 0, ' ', 0, 'L', 0, 'i', 0, 'n', 0, 'k', 0
};

/*
 * Strings wrappers array.
 */
static const USBDescriptor bdlink_strings[] = {
  {sizeof bdlink_string0, bdlink_string0},
  {sizeof bdlink_string1, bdlink_string1},
  {sizeof bdlink_string2, bdlink_string2},
  {sizeof bdlink_string3, bdlink_string3},
  {sizeof bdlink_string4, bdlink_string4}
};

/*
 * Handles the GET_DESCRIPTOR callback. All required descriptors must be
 * handled here.
 */
static const USBDescriptor *get_descriptor(USBDriver *usbp,
                                           uint8_t dtype,
                                           uint8_t dindex,
                                           uint16_t lang) {

  (void)usbp;
  (void)lang;
  switch (dtype) {
  case USB_DESCRIPTOR_DEVICE:
    return &bdlink_device_descriptor;
  case USB_DESCRIPTOR_CONFIGURATION:
    return &bdlink_configuration_descriptor;
  case USB_DESCRIPTOR_STRING:
    if (dindex < 5)
      return &bdlink_strings[dindex];
  }
  return NULL;
}

/**
 * @brief   IN EP1 state.
 */
static USBInEndpointState ep1instate;

/**
 * @brief   EP1 initialization structure (IN only)
 */
static const USBEndpointConfig ep1config = {
  USB_EP_MODE_TYPE_BULK,
  NULL,
  NULL,
  NULL,
  0x0040,
  0x0000,
  &ep1instate,
  NULL,
  1,
  NULL
};

/**
 * @brief   OUT EP2 state.
 */
static USBOutEndpointState ep2outstate;

/**
 * @brief   EP2 initialization structure (OUT only).
 */
static const USBEndpointConfig ep2config = {
  USB_EP_MODE_TYPE_BULK,
  NULL,
  NULL,
  NULL,
  0x0000,
  0x0040,
  NULL,
  &ep2outstate,
  1,
  NULL
};

/**
 * @brief   IN EP3 state.
 */
static USBInEndpointState ep3instate;

/**
 * @brief   EP3 initialization structure (IN only)
 */
static const USBEndpointConfig ep3config = {
  USB_EP_MODE_TYPE_BULK,
  NULL,
  NULL,
  NULL,
  0x0040,
  0x0000,
  &ep3instate,
  NULL,
  1,
  NULL
};

/*
 * Handles the USB driver global events.
 */
static void usb_event(USBDriver *usbp, usbevent_t event) {

  switch (event) {
  case USB_EVENT_RESET:
    {
        const uint8_t HEX[] = {
            '0', '1', '2', '3', '4', '5', '6', '7',
            '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
        };
        volatile uint32_t *p = (volatile uint32_t *)0x1FFFF7E8;
        int8_t i;

        for (i = 0; i < 24; i++) {
            bdlink_string3[(i + 1) * 2 + 0] = HEX[p[i / 8] >> (7 - i % 8) * 4 & 0x0f];
            bdlink_string3[(i + 1) * 2 + 1] = 0;
        }
    }
    return;
  case USB_EVENT_ADDRESS:
    return;
  case USB_EVENT_CONFIGURED:
    chSysLockFromISR();

    /* Enables the endpoints specified into the configuration.
       Note, this callback is invoked from an ISR so I-Class functions
       must be used.*/
    usbInitEndpointI(usbp, USBD1_STLINK_TX_EP, &ep1config);
    usbInitEndpointI(usbp, USBD1_STLINK_RX_EP, &ep2config);
    usbInitEndpointI(usbp, USBD1_STLINK_TRACE_EP, &ep3config);

    chSysUnlockFromISR();
    return;
  case USB_EVENT_SUSPEND:
    return;
  case USB_EVENT_WAKEUP:
    return;
  case USB_EVENT_STALLED:
    return;
  case USB_EVENT_UNCONFIGURED:
    return;
  }
  return;
}

/*
 * USB driver configuration.
 */
const USBConfig usbcfg = {
  usb_event,
  get_descriptor,
  NULL,
  NULL
};
