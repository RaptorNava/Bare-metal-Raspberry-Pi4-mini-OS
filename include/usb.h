/**
 * usb.h — USB HID драйвер для Raspberry Pi 4 (DWC2 OTG, bare-metal)
 *
 * Поддерживается только USB HID клавиатура (Boot Protocol),
 * подключённая к нижнему USB 2.0 порту (DWC2 OTG).
 *
 * Аппаратная база:
 *   DWC2 base:  0xFE980000  (mapped в адресном пространстве RPi4)
 *   Используется polling-режим (без прерываний) для простоты.
 */

#ifndef USB_H
#define USB_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Базовый адрес DWC2 OTG контроллера на RPi4
 * Периферийная база RPi4: 0xFE000000
 * DWC2 смещение:          0x00980000
 * ----------------------------------------------------------------------- */
#define DWC2_BASE           0xFE980000UL

/* -----------------------------------------------------------------------
 * Глобальные регистры DWC2 (смещения от DWC2_BASE)
 * ----------------------------------------------------------------------- */
#define DWC2_GOTGCTL        0x000   /* OTG Control and Status */
#define DWC2_GOTGINT        0x004   /* OTG Interrupt */
#define DWC2_GAHBCFG        0x008   /* AHB Configuration */
#define DWC2_GUSBCFG        0x00C   /* USB Configuration */
#define DWC2_GRSTCTL        0x010   /* Reset Control */
#define DWC2_GINTSTS        0x014   /* Interrupt Status */
#define DWC2_GINTMSK        0x018   /* Interrupt Mask */
#define DWC2_GRXSTSR        0x01C   /* Receive Status Debug Read */
#define DWC2_GRXSTSP        0x020   /* Receive Status Read/Pop */
#define DWC2_GRXFSIZ        0x024   /* Receive FIFO Size */
#define DWC2_GNPTXFSIZ      0x028   /* Non-Periodic TX FIFO Size */
#define DWC2_GNPTXSTS       0x02C   /* Non-Periodic TX FIFO/Queue Status */
#define DWC2_HPTXFSIZ       0x100   /* Host Periodic TX FIFO Size */
#define DWC2_HCFG           0x400   /* Host Configuration */
#define DWC2_HFIR           0x404   /* Host Frame Interval */
#define DWC2_HFNUM          0x408   /* Host Frame Number/Frame Time Remaining */
#define DWC2_HPTXSTS        0x410   /* Host Periodic TX FIFO/Queue Status */
#define DWC2_HAINT          0x414   /* Host All Channels Interrupt */
#define DWC2_HAINTMSK       0x418   /* Host All Channels Interrupt Mask */
#define DWC2_HPRT           0x440   /* Host Port Control and Status */

/* Канальные регистры (Channel 0) — каждый канал занимает 0x20 байт */
#define DWC2_HCBASE         0x500
#define DWC2_HCCHAR(ch)     (DWC2_HCBASE + (ch)*0x20 + 0x00)  /* Channel Characteristics */
#define DWC2_HCSPLT(ch)     (DWC2_HCBASE + (ch)*0x20 + 0x04)  /* Channel Split Control */
#define DWC2_HCINT(ch)      (DWC2_HCBASE + (ch)*0x20 + 0x08)  /* Channel Interrupt */
#define DWC2_HCINTMSK(ch)   (DWC2_HCBASE + (ch)*0x20 + 0x0C)  /* Channel Interrupt Mask */
#define DWC2_HCTSIZ(ch)     (DWC2_HCBASE + (ch)*0x20 + 0x10)  /* Channel Transfer Size */
#define DWC2_HCDMA(ch)      (DWC2_HCBASE + (ch)*0x20 + 0x14)  /* Channel DMA Address */

/* ----------------------------------------------------------------------- 
 * Биты регистров
 * ----------------------------------------------------------------------- */

/* GAHBCFG */
#define GAHBCFG_GLBL_INTR_EN    (1 << 0)
#define GAHBCFG_DMA_EN          (1 << 5)

/* GUSBCFG */
#define GUSBCFG_TOUTCAL_MASK    (0x7 << 0)
#define GUSBCFG_PHYIF_16BIT     (1 << 3)
#define GUSBCFG_SRPCAP          (1 << 8)
#define GUSBCFG_HNPCAP          (1 << 9)
#define GUSBCFG_TRDT_MASK       (0xF << 10)
#define GUSBCFG_TRDT_VAL        (5 << 10)    /* USB turnaround time */
#define GUSBCFG_FHMOD           (1 << 29)    /* Force Host Mode */

/* GRSTCTL */
#define GRSTCTL_CSRST           (1 << 0)     /* Core Soft Reset */
#define GRSTCTL_RXFFLSH         (1 << 4)     /* RX FIFO Flush */
#define GRSTCTL_TXFFLSH         (1 << 5)     /* TX FIFO Flush */
#define GRSTCTL_TXFNUM_MASK     (0x1F << 6)
#define GRSTCTL_TXFNUM_ALL      (0x10 << 6)  /* Flush all TX FIFOs */
#define GRSTCTL_AHBIDLE         (1 << 31)    /* AHB Master Idle */

/* GINTSTS / GINTMSK биты */
#define GINT_CURMODE_HOST       (1 << 0)     /* Current Mode: 1=Host */
#define GINT_SOF                (1 << 3)     /* Start of Frame */
#define GINT_RXFLVL             (1 << 4)     /* RX FIFO Non-Empty */
#define GINT_HPRTINT            (1 << 24)    /* Host Port Interrupt */
#define GINT_HCINT              (1 << 25)    /* Host Channels Interrupt */

/* HPRT биты */
#define HPRT_PRTCONNSTS         (1 << 0)     /* Port Connect Status */
#define HPRT_PRTCONNDET         (1 << 1)     /* Port Connect Detected */
#define HPRT_PRTENA             (1 << 2)     /* Port Enable */
#define HPRT_PRTENCHNG          (1 << 3)     /* Port Enable/Disable Change */
#define HPRT_PRTOVRCURRACT      (1 << 4)     /* Port Overcurrent Active */
#define HPRT_PRTOVRCURRCHNG     (1 << 5)     /* Port Overcurrent Change */
#define HPRT_PRTRES             (1 << 6)     /* Port Resume */
#define HPRT_PRTSUSP            (1 << 7)     /* Port Suspend */
#define HPRT_PRTRST             (1 << 8)     /* Port Reset */
#define HPRT_PRTLNSTS_MASK      (0x3 << 10)  /* Port Line Status */
#define HPRT_PRTSPD_MASK        (0x3 << 17)  /* Port Speed */
#define HPRT_PRTSPD_HS          (0 << 17)
#define HPRT_PRTSPD_FS          (1 << 17)
#define HPRT_PRTSPD_LS          (2 << 17)
#define HPRT_PRTPWR             (1 << 12)    /* Port Power */

/* HCCHAR биты */
#define HCCHAR_MPS_MASK         (0x7FF << 0) /* Max Packet Size */
#define HCCHAR_EPNUM_SHIFT      11
#define HCCHAR_EPDIR_IN         (1 << 15)    /* Endpoint Direction: IN */
#define HCCHAR_LSPDDEV          (1 << 17)    /* Low-Speed Device */
#define HCCHAR_EPTYPE_SHIFT     18
#define HCCHAR_EPTYPE_CTRL      (0 << 18)
#define HCCHAR_EPTYPE_ISOC      (1 << 18)
#define HCCHAR_EPTYPE_BULK      (2 << 18)
#define HCCHAR_EPTYPE_INTR      (3 << 18)
#define HCCHAR_DEVADDR_SHIFT    22
#define HCCHAR_ODDFRM           (1 << 29)    /* Odd Frame */
#define HCCHAR_CHDIS            (1 << 30)    /* Channel Disable */
#define HCCHAR_CHENA            (1 << 31)    /* Channel Enable */

/* HCTSIZ биты */
#define HCTSIZ_XFERSIZE_MASK    (0x7FFFF << 0)
#define HCTSIZ_PKTCNT_SHIFT     19
#define HCTSIZ_PID_SHIFT        29
#define HCTSIZ_PID_DATA0        (0 << 29)
#define HCTSIZ_PID_DATA2        (1 << 29)
#define HCTSIZ_PID_DATA1        (2 << 29)
#define HCTSIZ_PID_MDATA        (3 << 29)
#define HCTSIZ_PID_SETUP        (3 << 29)

/* HCINT биты */
#define HCINT_XFERCOMPL         (1 << 0)    /* Transfer Complete */
#define HCINT_CHHLTD            (1 << 1)    /* Channel Halted */
#define HCINT_AHBERR            (1 << 2)    /* AHB Error */
#define HCINT_STALL             (1 << 3)
#define HCINT_NAK               (1 << 4)
#define HCINT_ACK               (1 << 5)
#define HCINT_NYET              (1 << 6)
#define HCINT_XACTERR           (1 << 7)
#define HCINT_BBLERR            (1 << 8)
#define HCINT_FRMOVRUN          (1 << 9)
#define HCINT_DATATGLERR        (1 << 10)

/* -----------------------------------------------------------------------
 * USB стандартные дескрипторы и запросы
 * ----------------------------------------------------------------------- */

/* bmRequestType */
#define USB_REQ_DIR_OUT         0x00
#define USB_REQ_DIR_IN          0x80
#define USB_REQ_TYPE_STD        0x00
#define USB_REQ_TYPE_CLASS      0x20
#define USB_REQ_TYPE_VENDOR     0x40
#define USB_REQ_RECIP_DEV       0x00
#define USB_REQ_RECIP_IFACE     0x01
#define USB_REQ_RECIP_EP        0x02

/* bRequest стандартные */
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_CONFIG      0x09

/* HID class запросы */
#define HID_REQ_SET_PROTOCOL    0x0B
#define HID_REQ_SET_IDLE        0x0A

/* Типы дескрипторов */
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05
#define USB_DESC_HID            0x21
#define USB_DESC_REPORT         0x22

/* Endpoint типы */
#define USB_EP_TYPE_CTRL        0x00
#define USB_EP_TYPE_ISOC        0x01
#define USB_EP_TYPE_BULK        0x02
#define USB_EP_TYPE_INTR        0x03

/* HID протоколы */
#define HID_PROTOCOL_BOOT       0
#define HID_PROTOCOL_REPORT     1

/* HID Boot Protocol — размер пакета клавиатуры */
#define HID_KBD_BOOT_REPORT_SIZE  8

/* -----------------------------------------------------------------------
 * USB Setup пакет (8 байт)
 * ----------------------------------------------------------------------- */
typedef struct {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed)) usb_setup_t;

/* -----------------------------------------------------------------------
 * USB Device Descriptor (18 байт)
 * ----------------------------------------------------------------------- */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;   /* Макс. размер пакета EP0 */
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed)) usb_device_desc_t;

/* -----------------------------------------------------------------------
 * USB Configuration Descriptor (9 байт заголовок)
 * ----------------------------------------------------------------------- */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed)) usb_config_desc_t;

/* -----------------------------------------------------------------------
 * USB Interface Descriptor (9 байт)
 * ----------------------------------------------------------------------- */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed)) usb_iface_desc_t;

/* -----------------------------------------------------------------------
 * USB Endpoint Descriptor (7 байт)
 * ----------------------------------------------------------------------- */
typedef struct {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;  /* бит 7: 1=IN, биты [3:0]: номер EP */
    uint8_t  bmAttributes;      /* биты [1:0]: тип передачи */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;         /* Интервал polling в мс (для Interrupt EP) */
} __attribute__((packed)) usb_ep_desc_t;

/* -----------------------------------------------------------------------
 * Состояние USB клавиатуры
 * ----------------------------------------------------------------------- */
typedef struct {
    uint8_t  initialized;       /* 1 = устройство найдено и сконфигурировано */
    uint8_t  dev_addr;          /* Присвоенный USB адрес (1..127) */
    uint8_t  ep_intr;           /* Номер Interrupt IN endpoint */
    uint8_t  ep_mps;            /* Max Packet Size Interrupt EP */
    uint8_t  toggle;            /* DATA0/DATA1 toggle бит */
    uint8_t  speed;             /* 0=HS, 1=FS, 2=LS */
    uint8_t  report[HID_KBD_BOOT_REPORT_SIZE]; /* Последний HID отчёт */
    uint8_t  prev_keys[6];      /* Предыдущие нажатые клавиши */
} usb_kbd_t;

/* -----------------------------------------------------------------------
 * HID Boot Protocol Report (8 байт):
 *   [0] modifier (Ctrl/Shift/Alt/GUI маски)
 *   [1] reserved
 *   [2..7] keycodes (до 6 одновременно нажатых клавиш)
 * ----------------------------------------------------------------------- */
#define HID_MOD_LEFT_CTRL   (1 << 0)
#define HID_MOD_LEFT_SHIFT  (1 << 1)
#define HID_MOD_LEFT_ALT    (1 << 2)
#define HID_MOD_LEFT_GUI    (1 << 3)
#define HID_MOD_RIGHT_CTRL  (1 << 4)
#define HID_MOD_RIGHT_SHIFT (1 << 5)
#define HID_MOD_RIGHT_ALT   (1 << 6)
#define HID_MOD_RIGHT_GUI   (1 << 7)

/* -----------------------------------------------------------------------
 * Публичный API
 * ----------------------------------------------------------------------- */

/**
 * usb_init() — инициализация DWC2 контроллера в host-режиме.
 * Вызывать один раз из kernel_main() после инициализации UART.
 * Возвращает 0 при успехе, -1 при ошибке.
 */
int usb_init(void);

/**
 * usb_kbd_poll() — опрос клавиатуры (вызывать в главном цикле).
 * Возвращает keycode последней нажатой клавиши или 0 если ничего.
 * Символ уже переведён в ASCII (для печатаемых символов).
 */
char usb_kbd_poll(void);

/**
 * usb_kbd_get_state() — возвращает указатель на внутреннее состояние.
 */
const usb_kbd_t* usb_kbd_get_state(void);

#endif /* USB_H */
