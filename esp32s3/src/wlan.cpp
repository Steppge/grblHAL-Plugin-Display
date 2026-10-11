// WiFi, firmware updates over the air (ArduinoOTA and upload page)

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <Update.h>
#include <Preferences.h>
#include "wlan.h"
#include "pins.h"
#include "link.h"

wifi_cfg_t wifi_cfg = {};

static Preferences prefs;
static WebServer server(80);
static HTTPUpdateServer updater;
static wifi_state_t state = Wifi_Off;
static bool services = false;           // OTA, upload page and mDNS are running
static char ip[16] = "";
static void (*on_progress)(unsigned pct) = NULL;
static bool scanning = false;
static int scan_n = 0;
static char scan_ssid[WIFI_SCAN_MAX][33];
static char host[33] = WIFI_HOST_DEFAULT;

// sender bridge: TCP connection <-> second UART to the controller
static WiFiServer sender_server(SENDER_PORT);
static WiFiClient sender_client;
static HardwareSerial &sender_uart = Serial2;
static bool sender_on = false;

static void sender_close (void)
{
    if(sender_client)
        sender_client.stop();
    if(sender_on)
        link_send("TC,0");
    sender_on = false;
}

// One sender at a time, bytes are passed through unchanged in both directions.
static void sender_poll (void)
{
    if(sender_server.hasClient()) {
        WiFiClient c = sender_server.accept();
        if(sender_client && sender_client.connected())
            c.stop();               // busy
        else {
            sender_client = c;
            sender_client.setNoDelay(true);
            while(sender_uart.available())
                sender_uart.read();
            sender_on = true;
            link_send("TC,1");
        }
    }

    if(sender_on && !sender_client.connected()) {
        sender_close();
        return;
    }

    if(sender_on) {
        uint8_t buf[512];
        int n;
        if((n = sender_client.available()) > 0) {
            n = sender_client.read(buf, min(n, (int)sizeof(buf)));
            if(n > 0)
                sender_uart.write(buf, n);
        }
        if((n = sender_uart.available()) > 0) {
            n = sender_uart.read(buf, min(n, (int)sizeof(buf)));
            if(n > 0)
                sender_client.write(buf, n);
        }

        // the controller may have restarted: repeat the connection state once a second
        static uint32_t last = 0;
        if(millis() - last >= 1000) {
            last = millis();
            link_send("TC,1");
        }
    } else {
        while(sender_uart.available())
            sender_uart.read();     // nothing to pass it to
    }
}

static void services_start (void)
{
    MDNS.begin(host);

    sender_server.begin();
    sender_server.setNoDelay(true);
    MDNS.addService("telnet", "tcp", SENDER_PORT);

    if(*wifi_cfg.ota) {
        ArduinoOTA.setHostname(host);
        ArduinoOTA.setPassword(wifi_cfg.ota);
        ArduinoOTA.setMdnsEnabled(false);   // MDNS is started above
        ArduinoOTA.begin();
        MDNS.enableArduino(3232, true);

        updater.setup(&server, "/update", "admin", wifi_cfg.ota);
        server.on("/", []() {
            server.sendHeader("Location", "/update");
            server.send(302, "text/plain", "");
        });
        server.begin();
        MDNS.addService("http", "tcp", 80);
    }

    services = true;
}

static void services_stop (void)
{
    if(services) {
        sender_close();
        sender_server.end();
        ArduinoOTA.end();
        server.stop();
        MDNS.end();
        services = false;
    }
}

static void start (void)
{
    services_stop();
    WiFi.disconnect(true);

    if(!wifi_cfg.enabled || !*wifi_cfg.ssid) {
        WiFi.mode(WIFI_OFF);
        state = Wifi_Off;
        return;
    }

    WiFi.mode(WIFI_STA);
    WiFi.setHostname(host);
    WiFi.setSleep(false);       // short response times for the sender
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifi_cfg.ssid, wifi_cfg.pass);
    state = Wifi_Connecting;
}

void wifi_begin (void (*progress)(unsigned pct))
{
    on_progress = progress;

    prefs.begin("wifi", false);
    if(prefs.getBytesLength("cfg") == sizeof(wifi_cfg))
        prefs.getBytes("cfg", &wifi_cfg, sizeof(wifi_cfg));
    prefs.getString("host", host, sizeof(host));
    if(!*host)
        strcpy(host, WIFI_HOST_DEFAULT);

    sender_uart.setRxBufferSize(4096);
    sender_uart.setTxBufferSize(4096);
    sender_uart.begin(SENDER_BAUD, SERIAL_8N1, PIN_SENDER_RX, PIN_SENDER_TX);

    // progress of both update paths, the display shows it
    Update.onProgress([](size_t done, size_t total) {
        if(on_progress && total)
            on_progress((unsigned)(done * 100 / total));
    });

    start();
}

void wifi_apply (void)
{
    prefs.putBytes("cfg", &wifi_cfg, sizeof(wifi_cfg));
    start();
}

void wifi_poll (bool updates_allowed)
{
    if(state == Wifi_Off)
        return;

    if(WiFi.status() == WL_CONNECTED) {
        if(state != Wifi_Connected) {
            state = Wifi_Connected;
            strlcpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip));
            services_start();
        }
    } else if(state == Wifi_Connected) {
        state = Wifi_Connecting;    // reconnects by itself
        services_stop();
    }

    if(state == Wifi_Connected && services)
        sender_poll();
    else if(sender_on)
        sender_close();

    // while a job runs update requests are not answered at all
    if(state == Wifi_Connected && services && updates_allowed && *wifi_cfg.ota) {
        ArduinoOTA.handle();
        server.handleClient();
    }
}

void wifi_scan_start (void)
{
    if(scanning)
        return;

    if(WiFi.getMode() == WIFI_OFF)
        WiFi.mode(WIFI_STA);    // scanning needs the station mode, switched off again afterwards

    WiFi.scanDelete();
    scanning = WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING;
    scan_n = 0;
}

int wifi_scan_result (void)
{
    if(!scanning)
        return scan_n;

    int n = WiFi.scanComplete();
    if(n == WIFI_SCAN_RUNNING)
        return -1;

    scanning = false;
    scan_n = 0;

    // the results come sorted by signal strength, the same network may show up more than once
    for(int i = 0; i < n && scan_n < WIFI_SCAN_MAX; i++) {
        String ssid = WiFi.SSID(i);
        bool dup = ssid.length() == 0;
        for(int j = 0; j < scan_n && !dup; j++)
            dup = ssid == scan_ssid[j];
        if(!dup)
            strlcpy(scan_ssid[scan_n++], ssid.c_str(), sizeof(scan_ssid[0]));
    }
    WiFi.scanDelete();

    if(state == Wifi_Off)
        WiFi.mode(WIFI_OFF);

    return scan_n;
}

const char *wifi_scan_ssid (int i)
{
    return i >= 0 && i < scan_n ? scan_ssid[i] : "";
}

const char *wifi_host (void)
{
    return host;
}

void wifi_set_host (const char *name)
{
    char h[33];
    size_t n = 0;

    for(; *name && n < sizeof(h) - 1; name++) {
        char c = *name >= 'A' && *name <= 'Z' ? *name + 32 : *name;
        if((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && n))
            h[n++] = c;
    }
    h[n] = '\0';

    strlcpy(host, n ? h : WIFI_HOST_DEFAULT, sizeof(host));
    prefs.putString("host", host);
}

bool wifi_sender (void)
{
    return sender_on;
}

wifi_state_t wifi_state (void)
{
    return state;
}

const char *wifi_ip (void)
{
    return state == Wifi_Connected ? ip : "";
}
