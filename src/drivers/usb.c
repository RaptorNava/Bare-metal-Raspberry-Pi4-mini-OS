/**
 * usb.c — Bare-metal USB HID клавиатура для Raspberry Pi 4
 *
 * Реализует минимальный USB стек для работы HID клавиатуры:
 *   1. Инициализация DWC2 OTG контроллера в Host-режиме
 *   2. Обнаружение подключённого устройства
 *   3. USB Enumeration (адресация, чтение дескрипторов, конфигурация)
 *   4. Установка HID Boot Protocol
 *   5. Polling Interrupt IN endpoint
 *   6. Декодирование HID Boot Report → ASCII
 *
 * Ограничения (намеренно упрощено для bare-metal):
 *   - Только один порт, одно устройство
 *   - Только HID класс (клавиатура), Boot Protocol
 *   - Polling без прерываний
 *   - Только USB Full-Speed (12 Mbit/s) и Low-Speed устройства
 *   - DMA отключён (PIO режим)
 *
 * Аппаратура: DWC2 OTG USB 2.0, базовый адрес 0xFE980000
 */

#include "usb.h"
#include <stddef.h>
/* -----------------------------------------------------------------------
 * Вспомогательные функции: MMIO доступ
 * ----------------------------------------------------------------------- */

static inline void mmio_write(uint64_t addr, uint32_t val) {
    *(volatile uint32_t*)addr = val;
}

static inline uint32_t mmio_read(uint64_t addr) {
    return *(volatile uint32_t*)addr;
}

/* Простая задержка через счётчик (без таймера) */
static void delay_us(uint32_t us) {
    /* На cortex-a72 ~1.5 GHz: примерно 1500 итераций = 1 мкс
     * Используем ~1000 для запаса (задержка будет чуть длиннее — это OK) */
    volatile uint32_t i = us * 1000;
    while (i--) {
        __asm__ volatile("nop");
    }
}

static void delay_ms(uint32_t ms) {
    delay_us(ms * 1000);
}

/* -----------------------------------------------------------------------
 * Глобальное состояние клавиатуры
 * ----------------------------------------------------------------------- */
static usb_kbd_t kbd;

/* Буфер для DMA/FIFO операций — выровнен по 4 байта */
static uint8_t  xfer_buf[256] __attribute__((aligned(4)));

/* -----------------------------------------------------------------------
 * Низкоуровневые DWC2 операции
 * ----------------------------------------------------------------------- */

/**
 * dwc2_reg_read / dwc2_reg_write — чтение/запись регистров DWC2
 */
static inline uint32_t dwc2_reg_read(uint32_t offset) {
    return mmio_read(DWC2_BASE + offset);
}

static inline void dwc2_reg_write(uint32_t offset, uint32_t val) {
    mmio_write(DWC2_BASE + offset, val);
}

/**
 * dwc2_wait_bit — ждёт пока бит станет нужным значением
 * Возвращает 0 при успехе, -1 при таймауте
 */
static int dwc2_wait_bit(uint32_t offset, uint32_t mask, uint32_t expected,
                          uint32_t timeout_ms) {
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        if ((dwc2_reg_read(offset) & mask) == expected)
            return 0;
        delay_ms(1);
        elapsed++;
    }
    return -1; /* Таймаут */
}

/**
 * dwc2_flush_fifos — сброс RX и TX FIFO
 */
static int dwc2_flush_fifos(void) {
    uint32_t val;

    /* Flush TX FIFO (все) */
    val = GRSTCTL_TXFFLSH | GRSTCTL_TXFNUM_ALL;
    dwc2_reg_write(DWC2_GRSTCTL, val);
    if (dwc2_wait_bit(DWC2_GRSTCTL, GRSTCTL_TXFFLSH, 0, 1000) != 0)
        return -1;

    /* Flush RX FIFO */
    dwc2_reg_write(DWC2_GRSTCTL, GRSTCTL_RXFFLSH);
    if (dwc2_wait_bit(DWC2_GRSTCTL, GRSTCTL_RXFFLSH, 0, 1000) != 0)
        return -1;

    delay_us(10);
    return 0;
}

/**
 * dwc2_core_reset — программный сброс ядра DWC2
 */
static int dwc2_core_reset(void) {
    /* Ждём IDLE шины AHB */
    if (dwc2_wait_bit(DWC2_GRSTCTL, GRSTCTL_AHBIDLE, GRSTCTL_AHBIDLE, 1000) != 0)
        return -1;

    /* Программный сброс */
    dwc2_reg_write(DWC2_GRSTCTL, GRSTCTL_CSRST);

    /* Ждём снятия бита сброса */
    if (dwc2_wait_bit(DWC2_GRSTCTL, GRSTCTL_CSRST, 0, 1000) != 0)
        return -1;

    /* После сброса — 3 мс задержка по спецификации */
    delay_ms(3);
    return 0;
}

/* -----------------------------------------------------------------------
 * Управление USB каналами (Host Channels)
 *
 * DWC2 имеет несколько каналов для одновременных транзакций.
 * Мы используем только Channel 0 (для Control EP0) и Channel 1 (Interrupt).
 * ----------------------------------------------------------------------- */

/**
 * dwc2_channel_halt — принудительно останавливает канал
 */
static void dwc2_channel_halt(int ch) {
    uint32_t hcchar = dwc2_reg_read(DWC2_HCCHAR(ch));
    hcchar |= HCCHAR_CHDIS;
    hcchar &= ~HCCHAR_CHENA;
    dwc2_reg_write(DWC2_HCCHAR(ch), hcchar);
    dwc2_wait_bit(DWC2_HCINT(ch), HCINT_CHHLTD, HCINT_CHHLTD, 100);
}

/**
 * dwc2_transfer — выполняет одну USB транзакцию через канал ch.
 *
 * @param ch        Номер канала (0 или 1)
 * @param dev_addr  USB адрес устройства
 * @param ep        Номер endpoint
 * @param ep_type   Тип EP: HCCHAR_EPTYPE_CTRL / INTR / BULK
 * @param ep_mps    Max Packet Size
 * @param is_in     1 = IN транзакция, 0 = OUT
 * @param pid       Тип PID: HCTSIZ_PID_DATA0/1/SETUP
 * @param buf       Буфер данных
 * @param len       Количество байт
 * @param speed     Скорость устройства (0=HS, 1=FS, 2=LS)
 * @return          Количество принятых байт при IN, 0 при OUT, -1 при ошибке
 */
static int dwc2_transfer(int ch, uint8_t dev_addr, uint8_t ep,
                          uint32_t ep_type, uint16_t ep_mps,
                          int is_in, uint32_t pid,
                          void *buf, uint32_t len, uint8_t speed) {
    /* Очищаем прерывания канала */
    dwc2_reg_write(DWC2_HCINT(ch), 0xFFFFFFFF);

    /* Настройка характеристик канала */
    uint32_t hcchar = 0;
    hcchar |= (ep_mps & 0x7FF);                        /* Max Packet Size */
    hcchar |= ((uint32_t)ep << HCCHAR_EPNUM_SHIFT);    /* EP номер */
    hcchar |= ep_type;                                  /* Тип EP */
    hcchar |= ((uint32_t)dev_addr << HCCHAR_DEVADDR_SHIFT); /* Адрес устр. */
    if (is_in)
        hcchar |= HCCHAR_EPDIR_IN;
    if (speed == 2) /* Low-Speed */
        hcchar |= HCCHAR_LSPDDEV;
    /* Нечётный фрейм для Interrupt EP */
    if (ep_type == HCCHAR_EPTYPE_INTR) {
        uint32_t frnum = dwc2_reg_read(DWC2_HFNUM) & 0xFFFF;
        if (frnum & 1)
            hcchar |= HCCHAR_ODDFRM;
    }
    dwc2_reg_write(DWC2_HCCHAR(ch), hcchar);

    /* Размер передачи */
    uint32_t pktcnt = (len + ep_mps - 1) / ep_mps;
    if (pktcnt == 0) pktcnt = 1; /* Минимум 1 пакет (ZLP) */
    uint32_t hctsiz = (len & 0x7FFFF)           /* Transfer Size */
                    | (pktcnt << HCTSIZ_PKTCNT_SHIFT)
                    | pid;                        /* PID */
    dwc2_reg_write(DWC2_HCTSIZ(ch), hctsiz);

    /* Для OUT: записываем данные в FIFO */
    if (!is_in && len > 0) {
        uint32_t *p32 = (uint32_t*)buf;
        uint32_t words = (len + 3) / 4;
        /* TX FIFO для канала ch — смещение 0x1000 + ch*0x1000 от DWC2_BASE */
        uint32_t fifo_offset = 0x1000 + ch * 0x1000;
        for (uint32_t i = 0; i < words; i++) {
            mmio_write(DWC2_BASE + fifo_offset, p32[i]);
        }
    }

    /* Запускаем канал */
    hcchar = dwc2_reg_read(DWC2_HCCHAR(ch));
    hcchar |= HCCHAR_CHENA;
    hcchar &= ~HCCHAR_CHDIS;
    dwc2_reg_write(DWC2_HCCHAR(ch), hcchar);

    /* Ожидаем завершения — polling до 500 мс */
    int timeout = 500;
    while (timeout-- > 0) {
        uint32_t hcint = dwc2_reg_read(DWC2_HCINT(ch));

        if (hcint & HCINT_XFERCOMPL) {
            dwc2_reg_write(DWC2_HCINT(ch), HCINT_XFERCOMPL | HCINT_CHHLTD);

            /* Для IN: читаем данные из RX FIFO */
            if (is_in && len > 0) {
                /* Смотрим сколько байт реально пришло */
                uint32_t remain = dwc2_reg_read(DWC2_HCTSIZ(ch)) & 0x7FFFF;
                uint32_t received = len - remain;

                /* Читаем из RX FIFO */
                uint32_t *p32 = (uint32_t*)buf;
                uint32_t words = (received + 3) / 4;
                uint32_t fifo_offset = 0x1000; /* RX FIFO всегда по смещению 0x1000 */
                for (uint32_t i = 0; i < words; i++) {
                    p32[i] = mmio_read(DWC2_BASE + fifo_offset);
                }
                return (int)received;
            }
            return 0;
        }

        /* NAK — устройство не готово, повторяем */
        if (hcint & HCINT_NAK) {
            dwc2_reg_write(DWC2_HCINT(ch), HCINT_NAK);
            /* Перезапускаем канал */
            dwc2_reg_write(DWC2_HCCHAR(ch), dwc2_reg_read(DWC2_HCCHAR(ch)) | HCCHAR_CHENA);
            delay_ms(1);
            continue;
        }

        /* NYET — только для High-Speed split транзакций */
        if (hcint & HCINT_NYET) {
            dwc2_reg_write(DWC2_HCINT(ch), HCINT_NYET);
            delay_ms(1);
            continue;
        }

        /* Ошибки */
        if (hcint & (HCINT_STALL | HCINT_XACTERR | HCINT_BBLERR |
                     HCINT_AHBERR | HCINT_FRMOVRUN | HCINT_DATATGLERR)) {
            dwc2_channel_halt(ch);
            dwc2_reg_write(DWC2_HCINT(ch), 0xFFFFFFFF);
            return -1;
        }

        delay_ms(1);
    }

    /* Таймаут */
    dwc2_channel_halt(ch);
    return -1;
}

/* -----------------------------------------------------------------------
 * USB Control Transfer — стандартная 3-фазная транзакция:
 *   SETUP фаза → DATA фаза (IN или OUT) → STATUS фаза
 * ----------------------------------------------------------------------- */

/**
 * usb_control_transfer — выполняет полный Control Transfer.
 *
 * @param dev_addr  USB адрес устройства
 * @param ep_mps    Max Packet Size EP0
 * @param setup     Указатель на 8-байтный Setup пакет
 * @param buf       Буфер данных (может быть NULL если wLength=0)
 * @param speed     Скорость устройства
 * @return          Количество принятых байт, или -1 при ошибке
 */
static int usb_control_transfer(uint8_t dev_addr, uint8_t ep_mps,
                                 usb_setup_t *setup, void *buf, uint8_t speed) {
    int ret;
    int is_in = (setup->bmRequestType & USB_REQ_DIR_IN) != 0;

    /* === ФАЗА 1: SETUP пакет (всегда OUT, PID=SETUP, DATA0) === */
    ret = dwc2_transfer(0, dev_addr, 0,
                        HCCHAR_EPTYPE_CTRL, ep_mps,
                        0,                       /* OUT */
                        HCTSIZ_PID_SETUP,
                        setup, 8, speed);
    if (ret < 0)
        return -1;

    delay_us(100);

    /* === ФАЗА 2: DATA фаза (если wLength > 0) === */
    int received = 0;
    if (setup->wLength > 0) {
        ret = dwc2_transfer(0, dev_addr, 0,
                            HCCHAR_EPTYPE_CTRL, ep_mps,
                            is_in,
                            HCTSIZ_PID_DATA1,
                            buf, setup->wLength, speed);
        if (ret < 0)
            return -1;
        received = is_in ? ret : (int)setup->wLength;
    }

    delay_us(100);

    /* === ФАЗА 3: STATUS фаза (направление противоположное DATA) === */
    /* STATUS всегда DATA1, нулевой длины */
    ret = dwc2_transfer(0, dev_addr, 0,
                        HCCHAR_EPTYPE_CTRL, ep_mps,
                        !is_in,              /* Противоположное направление */
                        HCTSIZ_PID_DATA1,
                        NULL, 0, speed);
    /* STATUS NAK не считается ошибкой для нас */

    return received;
}

/* -----------------------------------------------------------------------
 * USB Enumeration — процесс распознавания устройства
 * ----------------------------------------------------------------------- */

/**
 * usb_get_descriptor — читает дескриптор через Control Transfer
 */
static int usb_get_descriptor(uint8_t dev_addr, uint8_t ep_mps,
                               uint8_t desc_type, uint8_t desc_idx,
                               void *buf, uint16_t len, uint8_t speed) {
    usb_setup_t setup = {
        .bmRequestType = USB_REQ_DIR_IN | USB_REQ_TYPE_STD | USB_REQ_RECIP_DEV,
        .bRequest      = USB_REQ_GET_DESCRIPTOR,
        .wValue        = (uint16_t)((desc_type << 8) | desc_idx),
        .wIndex        = 0,
        .wLength       = len
    };
    return usb_control_transfer(dev_addr, ep_mps, &setup, buf, speed);
}

/**
 * usb_set_address — устанавливает USB адрес устройства
 */
static int usb_set_address(uint8_t new_addr, uint8_t ep_mps, uint8_t speed) {
    usb_setup_t setup = {
        .bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RECIP_DEV,
        .bRequest      = USB_REQ_SET_ADDRESS,
        .wValue        = new_addr,
        .wIndex        = 0,
        .wLength       = 0
    };
    int ret = usb_control_transfer(0, ep_mps, &setup, NULL, speed);
    if (ret < 0)
        return -1;
    delay_ms(2); /* Устройству нужно время на смену адреса */
    return 0;
}

/**
 * usb_set_configuration — активирует конфигурацию
 */
static int usb_set_configuration(uint8_t dev_addr, uint8_t cfg_val,
                                  uint8_t ep_mps, uint8_t speed) {
    usb_setup_t setup = {
        .bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_STD | USB_REQ_RECIP_DEV,
        .bRequest      = USB_REQ_SET_CONFIG,
        .wValue        = cfg_val,
        .wIndex        = 0,
        .wLength       = 0
    };
    return usb_control_transfer(dev_addr, ep_mps, &setup, NULL, speed);
}

/**
 * hid_set_protocol — устанавливает Boot Protocol (0) для HID
 */
static int hid_set_protocol(uint8_t dev_addr, uint8_t iface,
                              uint8_t protocol, uint8_t ep_mps, uint8_t speed) {
    usb_setup_t setup = {
        .bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_IFACE,
        .bRequest      = HID_REQ_SET_PROTOCOL,
        .wValue        = protocol,
        .wIndex        = iface,
        .wLength       = 0
    };
    return usb_control_transfer(dev_addr, ep_mps, &setup, NULL, speed);
}

/**
 * hid_set_idle — устанавливает интервал отправки данных (0 = только по изменению)
 */
static int hid_set_idle(uint8_t dev_addr, uint8_t iface,
                         uint8_t ep_mps, uint8_t speed) {
    usb_setup_t setup = {
        .bmRequestType = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS | USB_REQ_RECIP_IFACE,
        .bRequest      = HID_REQ_SET_IDLE,
        .wValue        = 0,  /* Duration=0: отправка только при изменении */
        .wIndex        = iface,
        .wLength       = 0
    };
    return usb_control_transfer(dev_addr, ep_mps, &setup, NULL, speed);
}

/**
 * usb_enumerate — полный процесс перечисления устройства.
 * Возвращает 0 если нашли HID клавиатуру, -1 иначе.
 */
static int usb_enumerate(void) {
    uint8_t speed = 1; /* Предполагаем Full-Speed */

    /* Читаем порт для определения скорости */
    uint32_t hprt = dwc2_reg_read(DWC2_HPRT);
    if (hprt & HPRT_PRTSPD_MASK) {
        uint32_t spd = (hprt & HPRT_PRTSPD_MASK) >> 17;
        speed = (uint8_t)spd;
    }

    /* --- Шаг 1: Читаем первые 8 байт Device Descriptor (EP0 MPS=8 изначально) --- */
    usb_device_desc_t dev_desc;
    int ret = usb_get_descriptor(0, 8, USB_DESC_DEVICE, 0,
                                  &dev_desc, 8, speed);
    if (ret < 0)
        return -1;

    uint8_t ep0_mps = dev_desc.bMaxPacketSize0;
    if (ep0_mps == 0) ep0_mps = 8;

    /* --- Шаг 2: Присваиваем адрес --- */
    ret = usb_set_address(1, ep0_mps, speed);
    if (ret < 0)
        return -1;
    kbd.dev_addr = 1;

    /* --- Шаг 3: Читаем полный Device Descriptor --- */
    ret = usb_get_descriptor(1, ep0_mps, USB_DESC_DEVICE, 0,
                              &dev_desc, sizeof(dev_desc), speed);
    if (ret < 0)
        return -1;

    /* --- Шаг 4: Читаем Configuration Descriptor (сначала заголовок 9 байт) --- */
    usb_config_desc_t cfg_hdr;
    ret = usb_get_descriptor(1, ep0_mps, USB_DESC_CONFIG, 0,
                              &cfg_hdr, sizeof(cfg_hdr), speed);
    if (ret < 0)
        return -1;

    uint16_t total_len = cfg_hdr.wTotalLength;
    if (total_len > sizeof(xfer_buf))
        total_len = sizeof(xfer_buf);

    /* Читаем полный конфигурационный дескриптор */
    ret = usb_get_descriptor(1, ep0_mps, USB_DESC_CONFIG, 0,
                              xfer_buf, total_len, speed);
    if (ret < 0)
        return -1;

    /* --- Шаг 5: Парсим дескрипторы в поисках HID клавиатуры --- */
    uint8_t found_kbd = 0;
    uint8_t kbd_iface = 0;
    uint8_t kbd_ep    = 0;
    uint16_t kbd_ep_mps = 8;

    uint8_t *p = xfer_buf;
    uint8_t *end = xfer_buf + ret;

    while (p < end) {
        uint8_t bLen  = p[0];
        uint8_t bType = p[1];

        if (bLen < 2) break;

        if (bType == USB_DESC_INTERFACE) {
            usb_iface_desc_t *iface = (usb_iface_desc_t*)p;
            /* HID класс = 0x03, подкласс Boot = 0x01, протокол Keyboard = 0x01 */
            if (iface->bInterfaceClass    == 0x03 &&
                iface->bInterfaceSubClass == 0x01 &&
                iface->bInterfaceProtocol == 0x01) {
                kbd_iface = iface->bInterfaceNumber;
                found_kbd = 1;
            }
        }

        if (bType == USB_DESC_ENDPOINT && found_kbd) {
            usb_ep_desc_t *ep = (usb_ep_desc_t*)p;
            /* Ищем Interrupt IN endpoint */
            if ((ep->bEndpointAddress & 0x80) &&           /* IN */
                (ep->bmAttributes & 0x03) == USB_EP_TYPE_INTR) { /* Interrupt */
                kbd_ep     = ep->bEndpointAddress & 0x0F;
                kbd_ep_mps = ep->wMaxPacketSize;
                break;
            }
        }

        p += bLen;
    }

    if (!found_kbd || kbd_ep == 0)
        return -1; /* Не нашли HID клавиатуру */

    /* --- Шаг 6: Активируем конфигурацию --- */
    ret = usb_set_configuration(1, cfg_hdr.bConfigurationValue, ep0_mps, speed);
    if (ret < 0)
        return -1;

    delay_ms(5);

    /* --- Шаг 7: Устанавливаем Boot Protocol --- */
    hid_set_protocol(1, kbd_iface, HID_PROTOCOL_BOOT, ep0_mps, speed);
    delay_ms(2);

    /* --- Шаг 8: SET_IDLE (не присылать повторяющиеся репорты) --- */
    hid_set_idle(1, kbd_iface, ep0_mps, speed);
    delay_ms(2);

    /* Сохраняем параметры клавиатуры */
    kbd.ep_intr  = kbd_ep;
    kbd.ep_mps   = (kbd_ep_mps > 8) ? 8 : (uint8_t)kbd_ep_mps;
    kbd.speed    = speed;
    kbd.toggle   = 0; /* DATA0 */
    kbd.initialized = 1;

    return 0;
}

/* -----------------------------------------------------------------------
 * Инициализация DWC2 контроллера
 * ----------------------------------------------------------------------- */

int usb_init(void) {
    /* Обнуляем структуру клавиатуры */
    for (int i = 0; i < (int)sizeof(kbd); i++)
        ((uint8_t*)&kbd)[i] = 0;

    /* --- Шаг 1: Форсируем Host Mode через GUSBCFG --- */
    uint32_t gusbcfg = dwc2_reg_read(DWC2_GUSBCFG);
    gusbcfg &= ~GUSBCFG_HNPCAP;
    gusbcfg &= ~GUSBCFG_SRPCAP;
    gusbcfg |=  GUSBCFG_FHMOD;     /* Force Host Mode */
    gusbcfg &= ~GUSBCFG_TRDT_MASK;
    gusbcfg |=  GUSBCFG_TRDT_VAL;
    dwc2_reg_write(DWC2_GUSBCFG, gusbcfg);

    /* Ждём переключения в host mode (до 25 мс по спецификации) */
    delay_ms(25);

    /* Проверяем что мы в host mode */
    if (!(dwc2_reg_read(DWC2_GINTSTS) & GINT_CURMODE_HOST))
        return -1;

    /* --- Шаг 2: Сброс ядра --- */
    if (dwc2_core_reset() != 0)
        return -1;

    /* --- Шаг 3: Настройка AHB (без DMA, без прерываний глобально) --- */
    uint32_t gahbcfg = dwc2_reg_read(DWC2_GAHBCFG);
    gahbcfg &= ~GAHBCFG_DMA_EN;        /* PIO режим */
    gahbcfg &= ~GAHBCFG_GLBL_INTR_EN;  /* Без глобальных прерываний */
    dwc2_reg_write(DWC2_GAHBCFG, gahbcfg);

    /* Маскируем все прерывания (polling режим) */
    dwc2_reg_write(DWC2_GINTMSK, 0);
    dwc2_reg_write(DWC2_GINTSTS, 0xFFFFFFFF); /* Сбрасываем флаги */

    /* --- Шаг 4: Настройка FIFO размеров ---
     * RPi4 DWC2 имеет 4096 слов (16 KB) FIFO памяти.
     * Распределяем: RX=1024 слова, TX Non-Periodic=512, TX Periodic=512
     */
    dwc2_reg_write(DWC2_GRXFSIZ,   1024);              /* RX FIFO: 1024 слова */
    dwc2_reg_write(DWC2_GNPTXFSIZ, (512 << 16) | 1024);/* NP TX: start=1024, size=512 */
    dwc2_reg_write(DWC2_HPTXFSIZ,  (512 << 16) | 1536);/* P TX: start=1536, size=512 */

    /* Сброс FIFO */
    if (dwc2_flush_fifos() != 0)
        return -1;

    /* --- Шаг 5: Настройка Host Configuration ---
     * HCFG: FS/LS PHY Clock = 48 MHz (биты [1:0] = 01)
     */
    uint32_t hcfg = dwc2_reg_read(DWC2_HCFG);
    hcfg &= ~0x3;
    hcfg |= 0x1; /* 48 MHz для FS */
    dwc2_reg_write(DWC2_HCFG, hcfg);

    /* --- Шаг 6: Включаем питание порта --- */
    uint32_t hprt = dwc2_reg_read(DWC2_HPRT);
    /* Очищаем W1C биты чтобы не сбросить их случайно */
    hprt &= ~(HPRT_PRTENA | HPRT_PRTCONNDET | HPRT_PRTENCHNG | HPRT_PRTOVRCURRCHNG);
    hprt |= HPRT_PRTPWR; /* Включаем питание */
    dwc2_reg_write(DWC2_HPRT, hprt);

    delay_ms(20); /* Время на стабилизацию питания */

    /* --- Шаг 7: Ждём подключения устройства (до 3 секунд) --- */
    int connected = 0;
    for (int i = 0; i < 3000; i++) {
        hprt = dwc2_reg_read(DWC2_HPRT);
        if (hprt & HPRT_PRTCONNSTS) {
            connected = 1;
            break;
        }
        delay_ms(1);
    }

    if (!connected)
        return -1; /* Устройство не подключено */

    /* Сбрасываем флаг подключения (W1C) */
    hprt = dwc2_reg_read(DWC2_HPRT);
    hprt &= ~(HPRT_PRTENA | HPRT_PRTENCHNG | HPRT_PRTOVRCURRCHNG);
    hprt |= HPRT_PRTCONNDET;
    dwc2_reg_write(DWC2_HPRT, hprt);

    delay_ms(100); /* Задержка после обнаружения */

    /* --- Шаг 8: USB Bus Reset (50 мс по спецификации) --- */
    hprt = dwc2_reg_read(DWC2_HPRT);
    hprt &= ~(HPRT_PRTENA | HPRT_PRTCONNDET | HPRT_PRTENCHNG | HPRT_PRTOVRCURRCHNG);
    hprt |= HPRT_PRTRST;
    dwc2_reg_write(DWC2_HPRT, hprt);

    delay_ms(60); /* 50-60 мс Reset */

    /* Снимаем Reset */
    hprt = dwc2_reg_read(DWC2_HPRT);
    hprt &= ~(HPRT_PRTENA | HPRT_PRTCONNDET | HPRT_PRTENCHNG | HPRT_PRTOVRCURRCHNG);
    hprt &= ~HPRT_PRTRST;
    dwc2_reg_write(DWC2_HPRT, hprt);

    delay_ms(20); /* Устройство стабилизируется после Reset */

    /* --- Шаг 9: Enumeration --- */
    return usb_enumerate();
}

/* -----------------------------------------------------------------------
 * HID Keycode → ASCII таблица (Boot Protocol)
 *
 * Индекс = HID Usage ID клавиши
 * Значение = ASCII символ (0 = нет символа)
 *
 * Охватывает: буквы, цифры, базовую пунктуацию, Enter, Backspace
 * ----------------------------------------------------------------------- */

/* Без Shift */
static const char hid_to_ascii[128] = {
    /* 0x00 */ 0,    /* No event */
    /* 0x01 */ 0,    /* Keyboard ErrorRollOver */
    /* 0x02 */ 0,    /* Keyboard POSTFail */
    /* 0x03 */ 0,    /* Keyboard ErrorUndefined */
    /* 0x04 */ 'a',  /* a */
    /* 0x05 */ 'b',
    /* 0x06 */ 'c',
    /* 0x07 */ 'd',
    /* 0x08 */ 'e',
    /* 0x09 */ 'f',
    /* 0x0A */ 'g',
    /* 0x0B */ 'h',
    /* 0x0C */ 'i',
    /* 0x0D */ 'j',
    /* 0x0E */ 'k',
    /* 0x0F */ 'l',
    /* 0x10 */ 'm',
    /* 0x11 */ 'n',
    /* 0x12 */ 'o',
    /* 0x13 */ 'p',
    /* 0x14 */ 'q',
    /* 0x15 */ 'r',
    /* 0x16 */ 's',
    /* 0x17 */ 't',
    /* 0x18 */ 'u',
    /* 0x19 */ 'v',
    /* 0x1A */ 'w',
    /* 0x1B */ 'x',
    /* 0x1C */ 'y',
    /* 0x1D */ 'z',
    /* 0x1E */ '1',
    /* 0x1F */ '2',
    /* 0x20 */ '3',
    /* 0x21 */ '4',
    /* 0x22 */ '5',
    /* 0x23 */ '6',
    /* 0x24 */ '7',
    /* 0x25 */ '8',
    /* 0x26 */ '9',
    /* 0x27 */ '0',
    /* 0x28 */ '\n', /* Enter */
    /* 0x29 */ 0x1B, /* Escape */
    /* 0x2A */ '\b', /* Backspace */
    /* 0x2B */ '\t', /* Tab */
    /* 0x2C */ ' ',  /* Space */
    /* 0x2D */ '-',
    /* 0x2E */ '=',
    /* 0x2F */ '[',
    /* 0x30 */ ']',
    /* 0x31 */ '\\',
    /* 0x32 */ 0,
    /* 0x33 */ ';',
    /* 0x34 */ '\'',
    /* 0x35 */ '`',
    /* 0x36 */ ',',
    /* 0x37 */ '.',
    /* 0x38 */ '/',
    /* 0x39 */ 0,    /* Caps Lock */
    /* 0x3A–0x45: F1–F12 */ 0,0,0,0,0,0,0,0,0,0,0,0,
    /* 0x46 */ 0,    /* Print Screen */
    /* 0x47 */ 0,    /* Scroll Lock */
    /* 0x48 */ 0,    /* Pause */
    /* 0x49 */ 0,    /* Insert */
    /* 0x4A */ 0,    /* Home */
    /* 0x4B */ 0,    /* Page Up */
    /* 0x4C */ 0x7F, /* Delete */
    /* 0x4D */ 0,    /* End */
    /* 0x4E */ 0,    /* Page Down */
    /* 0x4F */ 0,    /* Right Arrow */
    /* 0x50 */ 0,    /* Left Arrow */
    /* 0x51 */ 0,    /* Down Arrow */
    /* 0x52 */ 0,    /* Up Arrow */
    /* 0x53–0x63: NumLock, Keypad... */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

/* С Shift */
static const char hid_to_ascii_shift[128] = {
    0,0,0,0,
    'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n', 0x1B, '\b', '\t', ' ',
    '_','+','{','}','|',
    0,':','"','~','<','>','?',
    0,
    0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0x7F,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

/* -----------------------------------------------------------------------
 * Polling и декодирование
 * ----------------------------------------------------------------------- */

/**
 * usb_kbd_poll — опрашивает Interrupt IN endpoint клавиатуры.
 *
 * Использует Channel 1 для Interrupt транзакции.
 * Возвращает ASCII символ или 0 если нет новых нажатий.
 */
char usb_kbd_poll(void) {
    if (!kbd.initialized)
        return 0;

    /* Interrupt IN транзакция */
    uint32_t pid = kbd.toggle ? HCTSIZ_PID_DATA1 : HCTSIZ_PID_DATA0;

    int ret = dwc2_transfer(1,              /* Channel 1 */
                             kbd.dev_addr,
                             kbd.ep_intr,
                             HCCHAR_EPTYPE_INTR,
                             kbd.ep_mps,
                             1,             /* IN */
                             pid,
                             kbd.report,
                             HID_KBD_BOOT_REPORT_SIZE,
                             kbd.speed);

    if (ret <= 0)
        return 0; /* NAK или ошибка — нет данных */

    /* Переключаем DATA toggle */
    kbd.toggle ^= 1;

    /* Декодируем HID Boot Report:
     * [0] = modifier
     * [1] = reserved
     * [2..7] = keycodes
     */
    uint8_t modifier = kbd.report[0];
    int shift = (modifier & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0;

    /* Ищем новое нажатие (не было в предыдущем репорте) */
    for (int i = 2; i < HID_KBD_BOOT_REPORT_SIZE; i++) {
        uint8_t key = kbd.report[i];
        if (key == 0) continue;

        /* Проверяем, была ли эта клавиша нажата в прошлый раз */
        int was_pressed = 0;
        for (int j = 0; j < 6; j++) {
            if (kbd.prev_keys[j] == key) {
                was_pressed = 1;
                break;
            }
        }

        if (!was_pressed && key < 128) {
            /* Обновляем prev_keys */
            for (int j = 0; j < 6; j++)
                kbd.prev_keys[j] = kbd.report[j + 2];

            char c = shift ? hid_to_ascii_shift[key] : hid_to_ascii[key];
            return c;
        }
    }

    /* Обновляем prev_keys даже если нет новых клавиш */
    for (int j = 0; j < 6; j++)
        kbd.prev_keys[j] = kbd.report[j + 2];

    return 0;
}

const usb_kbd_t* usb_kbd_get_state(void) {
    return &kbd;
}
