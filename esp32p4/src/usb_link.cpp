// Link to the Octopus-Bridge (ESP32-S3 at the controller) over USB-C "USB 2.0" (J16), or over its
// machine network when the cable is out: display link over TCP, gamepad lines over UDP (see the bridge).
// While the cable is in, nothing is sent over WiFi.
//
// The CDC interfaces are described here instead of with the Arduino USBCDC class: that one always
// announces the high speed bulk size of 512 bytes, which the full speed host of the ESP32-S3 refuses
// ("EP MPS (512) exceeds supported limit"). 64 bytes is the full speed maximum.
//
// All TinyUSB and socket calls are made by one task. The link reaches it through two stream buffers, the
// gamepad is passed through to the D1 Mini UART by the task itself (latest state wins, nothing is queued).

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "USB.h"
#include "esp32-hal-tinyusb.h"
#include "tusb.h"
#include "freertos/stream_buffer.h"
#include "pins.h"
#include "link.h"
#include "wlan.h"
#include "usb_link.h"

#define BRIDGE_LINK_PORT    5000    // TCP
#define BRIDGE_GAMEPAD_PORT 5001    // UDP
#define TCP_RETRY_MS        1000
#define TCP_CONNECT_MS       500
#define TCP_CHECK_MS          50
#define IDLE_MAX_MS            4    // the task waits up to this long while nothing comes or goes

#define CDC_EP_SIZE      64
#define CDC_NOTIF_SIZE    8
#define LINK_RX_BUF   16384     // preview rows arrive in bursts while LVGL is drawing
#define LINK_TX_BUF    4096
#define TX_WAIT_MS       20     // max. wait of link_send() for room

#define CH_LINK          0
#define CH_GAMEPAD       1

static StreamBufferHandle_t rx_buf, tx_buf;
static HardwareSerial &gp_uart = Serial2;
static volatile uint32_t gp_from_d1, gp_to_d1;     // bytes, for usb_link_report()
static volatile link_path_t path = Path_None;
static WiFiClient tcp;
static WiFiUDP udp;

// channel 0 as a Stream for the link
class UsbLinkStream : public Stream {
  public:
    int available (void) override { return (int)xStreamBufferBytesAvailable(rx_buf); }
    int read (void) override
    {
        uint8_t c;
        return xStreamBufferReceive(rx_buf, &c, 1, 0) ? c : -1;
    }
    int peek (void) override { return -1; }     // not used by the link
    size_t write (uint8_t c) override { return write(&c, 1); }
    size_t write (const uint8_t *buf, size_t len) override
    {
        return xStreamBufferSend(tx_buf, buf, len, pdMS_TO_TICKS(TX_WAIT_MS));
    }
};

static UsbLinkStream usb_stream;

// same endpoints as the Arduino core: 0x85 notification, 0x03 out, 0x84 in (reserved for USB_INTERFACE_CDC)
static uint16_t cdc0_descriptor (uint8_t *dst, uint8_t *itf)
{
    uint8_t str = tinyusb_add_string_descriptor("grblHAL link");
    uint8_t desc[TUD_CDC_DESC_LEN] = { TUD_CDC_DESCRIPTOR(*itf, str, 0x85, CDC_NOTIF_SIZE, 0x03, 0x84, CDC_EP_SIZE) };

    *itf += 2;
    memcpy(dst, desc, sizeof(desc));

    return sizeof(desc);
}

static uint16_t cdc1_descriptor (uint8_t *dst, uint8_t *itf)
{
    uint8_t str = tinyusb_add_string_descriptor("grblHAL gamepad");
    uint8_t ep_notif = tinyusb_get_free_in_endpoint();
    uint8_t ep_in = tinyusb_get_free_in_endpoint();
    uint8_t ep_out = tinyusb_get_free_out_endpoint();

    if(!ep_notif || !ep_in || !ep_out)
        return 0;

    uint8_t desc[TUD_CDC_DESC_LEN] = { TUD_CDC_DESCRIPTOR(*itf, str, (uint8_t)(0x80 | ep_notif), CDC_NOTIF_SIZE,
                                                          ep_out, (uint8_t)(0x80 | ep_in), CDC_EP_SIZE) };
    *itf += 2;
    memcpy(dst, desc, sizeof(desc));

    return sizeof(desc);
}

// WiFi path: connect the link to the bridge while it is wanted, else close it. While connected the state is
// asked only every TCP_CHECK_MS (each tcp.connected() is a socket call); a broken connection meanwhile only
// makes read/write fail.
static bool net_update (bool wanted)
{
    static uint32_t last_try = 0, last_check = 0;
    static bool conn = false;

    if(!wanted) {
        if(conn || tcp)
            tcp.stop();
        conn = false;
        return false;
    }
    if(conn && millis() - last_check < TCP_CHECK_MS)
        return true;
    last_check = millis();
    conn = tcp.connected();
    if(!conn && millis() - last_try >= TCP_RETRY_MS) {
        last_try = millis();
        tcp.stop();
        if(tcp.connect(WiFi.gatewayIP(), BRIDGE_LINK_PORT, TCP_CONNECT_MS))
            tcp.setNoDelay(true);
        conn = tcp.connected();
    }

    return conn;
}

static void usb_task (void *arg)
{
    uint8_t buf[256];
    char gp_line[128];
    size_t n, gp_len = 0;
    bool udp_open = false;
    uint32_t idle = 1;

    for(;;) {
        bool moved = false;             // any data this round: then the next round comes after 1 ms
        bool usb = tud_mounted() && tud_cdc_n_connected(CH_LINK);
        bool wifi = !usb && wifi_machine();     // nothing over WiFi while the cable is in
        bool net = net_update(wifi);

        path = usb ? Path_Usb : net ? Path_Wifi : Path_None;

        // link log: every change of the USB connection to the bridge (mounted = enumerated, open = DTR set)
        static int last_usb = -1;
        int now_usb = (tud_mounted() ? 1 : 0) | (tud_cdc_n_connected(CH_LINK) ? 2 : 0) | (net ? 4 : 0);
        if(link_log && now_usb != last_usb)
            Serial.printf("[%lu] usb: %s, link channel %s%s\n", (unsigned long)millis(), now_usb & 1 ? "mounted" : "NOT mounted",
                          now_usb & 2 ? "open" : "closed", now_usb & 4 ? ", WiFi link" : "");
        last_usb = now_usb;

        if(wifi != udp_open) {
            if(wifi)
                udp.begin(BRIDGE_GAMEPAD_PORT);
            else
                udp.stop();
            udp_open = wifi;
        }

        // controller -> display, read only with room: otherwise USB or TCP holds the data back
        if(xStreamBufferSpacesAvailable(rx_buf) >= sizeof(buf)) {
            if(usb)
                n = tud_cdc_n_read(CH_LINK, buf, sizeof(buf));
            else if(net && tcp.available() > 0)
                n = tcp.read(buf, sizeof(buf));
            else
                n = 0;
            if(n > 0 && n <= sizeof(buf)) {
                xStreamBufferSend(rx_buf, buf, n, 0);
                moved = true;
            }
        }

        // display -> controller, dropped while there is no path
        n = usb ? min(sizeof(buf), (size_t)tud_cdc_n_write_available(CH_LINK)) : sizeof(buf);
        if(n && (n = xStreamBufferReceive(tx_buf, buf, n, 0)) > 0) {
            moved = true;
            if(usb) {
                tud_cdc_n_write(CH_LINK, buf, n);
                tud_cdc_n_write_flush(CH_LINK);
            } else if(net)
                tcp.write(buf, n);
        }

        // gamepad: controller -> D1 Mini
        if((n = tud_cdc_n_read(CH_GAMEPAD, buf, sizeof(buf))) > 0 && GAMEPAD) {
            gp_uart.write(buf, n);
            gp_to_d1 += n;
            moved = true;
        }
        if(udp_open && udp.parsePacket() > 0) {
            int r = udp.read(buf, sizeof(buf));
            if(r > 0 && GAMEPAD) {
                gp_uart.write(buf, r);
                gp_to_d1 += r;
            }
            moved = true;
        }

        // gamepad: D1 Mini -> controller, over WiFi one datagram per line
        int avail = GAMEPAD ? gp_uart.available() : 0;
        if(avail > 0) {
            moved = true;
            n = gp_uart.read(buf, min((size_t)avail, sizeof(buf)));
            gp_from_d1 += n;
            if(usb) {
                if(tud_cdc_n_connected(CH_GAMEPAD) && tud_cdc_n_write_available(CH_GAMEPAD) >= n) {
                    tud_cdc_n_write(CH_GAMEPAD, buf, n);
                    tud_cdc_n_write_flush(CH_GAMEPAD);
                }
            } else {
                for(size_t i = 0; i < n; i++) {
                    if(gp_len < sizeof(gp_line))
                        gp_line[gp_len++] = buf[i];
                    if(buf[i] == '\n') {
                        if(udp_open && gp_len < sizeof(gp_line)) {
                            udp.beginPacket(WiFi.gatewayIP(), BRIDGE_GAMEPAD_PORT);
                            udp.write((const uint8_t *)gp_line, gp_len);
                            udp.endPacket();
                        }
                        gp_len = 0;
                    }
                }
            }
        }

        // 1 ms while data flows, then 2 and up to IDLE_MAX_MS: fewer socket calls while nothing happens,
        // a new line waits at most IDLE_MAX_MS (status comes every 100 ms, the gamepad every 50 ms)
        idle = moved ? 1 : min(idle * 2, (uint32_t)IDLE_MAX_MS);
        vTaskDelay(pdMS_TO_TICKS(idle) ? pdMS_TO_TICKS(idle) : 1);
    }
}

void usb_link_begin (void)
{
    rx_buf = xStreamBufferCreate(LINK_RX_BUF, 1);
    tx_buf = xStreamBufferCreate(LINK_TX_BUF, 1);

    if(GAMEPAD) {
        gp_uart.setRxBufferSize(2048);
        gp_uart.begin(GAMEPAD_BAUD, SERIAL_8N1, PIN_GAMEPAD_RX, PIN_GAMEPAD_TX);
    }

    tinyusb_enable_interface(USB_INTERFACE_CDC, TUD_CDC_DESC_LEN, cdc0_descriptor);
    tinyusb_enable_interface(USB_INTERFACE_CDC2, TUD_CDC_DESC_LEN, cdc1_descriptor);
    USB.productName("grblHAL display");
    USB.begin();
    xTaskCreatePinnedToCore(usb_task, "usb_link", 4096, NULL, 5, NULL, 0);

    link_use(&usb_stream);
}

bool usb_link_mounted (void)
{
    return tud_mounted() && tud_cdc_n_connected(CH_LINK);
}

void usb_link_report (void)
{
    static const char *paths[] = { "-", "USB", "WiFi" };

    Serial.printf("link over %s, USB %s, machine network '%s' %s, gamepad channel %s, D1 UART GPIO%d TX GPIO%d RX %d baud: "
                  "%lu bytes from the D1, %lu to it\n",
                  paths[path], tud_mounted() ? "mounted" : "-", wifi_machine_ssid(),
                  wifi_machine() ? "connected" : "-", tud_cdc_n_connected(CH_GAMEPAD) ? "open" : "closed",
                  PIN_GAMEPAD_TX, PIN_GAMEPAD_RX, GAMEPAD_BAUD, (unsigned long)gp_from_d1, (unsigned long)gp_to_d1);
}

link_path_t usb_link_path (void)
{
    return path;
}
