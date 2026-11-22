/*
 * "Your Phone Is Talking About You"
 *
 * ESP32-S2 passive Wi-Fi probe-request counter + PIR comparison.
 * The ESP32 never connects to anything. It puts the radio in promiscuous
 * mode, hops channels 1-13, and listens for the Probe Request frames that
 * phones broadcast in the background while looking for known networks.
 *
 * Every REPORT_PERIOD_MS it prints:
 *   - unique MAC addresses heard (upper bound: phones randomise MACs)
 *   - an estimate of real devices (randomised MACs grouped by IE fingerprint)
 *   - SSIDs phones leaked (names of networks they remember)
 *   - PIR motion state
 *   - the real head count you type in, and the ratio
 *
 * Serial commands (type in `idf.py monitor`, then Enter):
 *   <number>     set the real head count, e.g.  18
 *   t<dBm>       set the "in this room" RSSI threshold, e.g.  t-65
 *   w<sec>       set the time window in seconds, e.g.  w120
 *   c            clear all collected devices
 *
 * Wiring (PIR, HC-SR501-style blue module):
 *   PIR VCC -> 5V (VBUS)   PIR GND -> GND   PIR OUT -> GPIO27 (PIR_GPIO)
 *   (OUT is 3.3 V logic, safe for the ESP32-S2.)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#define PIR_GPIO            GPIO_NUM_27
#define MAX_DEVICES         256
#define MAX_SSIDS           32
#define HOP_INTERVAL_MS     250
#define REPORT_PERIOD_MS    5000
#define DEFAULT_WINDOW_S    60
#define DEFAULT_RSSI_MIN    (-70)
#define CONSOLE_UART        UART_NUM_0

static const char *TAG = "probe";

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;          /* strongest RSSI in recent frames (decays) */
    uint8_t  randomized;    /* locally-administered bit set */
    uint32_t fingerprint;   /* hash of capability IEs */
    uint32_t frames;
    int64_t  last_seen_us;
} device_t;

typedef struct {
    char    ssid[33];
    int64_t last_seen_us;
} ssid_t;

static device_t s_dev[MAX_DEVICES];
static int      s_dev_count;
static ssid_t   s_ssid[MAX_SSIDS];
static int      s_ssid_count;
static uint32_t s_total_probes;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static volatile int  s_rssi_min  = DEFAULT_RSSI_MIN;
static volatile int  s_window_s  = DEFAULT_WINDOW_S;
static volatile int  s_headcount = -1;

static int64_t  s_pir_last_motion_us = -1;
static uint32_t s_pir_triggers;
static int      s_pir_level;

/* ---------- helpers ---------- */

static uint32_t fnv1a(uint32_t h, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= d[i];
        h *= 16777619u;
    }
    return h;
}

/* Hash only the IEs that describe the phone's hardware/driver, not the
 * SSID or channel. Randomised MACs from the same phone usually share it. */
static uint32_t ie_fingerprint(const uint8_t *ie, int len)
{
    uint32_t h = 2166136261u;
    int i = 0;
    while (i + 2 <= len) {
        uint8_t id = ie[i], l = ie[i + 1];
        if (i + 2 + l > len) break;
        const uint8_t *v = &ie[i + 2];
        switch (id) {
        case 1:   /* supported rates */
        case 50:  /* extended rates */
        case 45:  /* HT capabilities */
        case 127: /* extended capabilities */
        case 191: /* VHT capabilities */
            h = fnv1a(h, &id, 1);
            h = fnv1a(h, v, l);
            break;
        case 221: /* vendor specific: OUI + type only */
            h = fnv1a(h, &id, 1);
            h = fnv1a(h, v, l < 4 ? l : 4);
            break;
        case 255: /* element extension: ext id (HE caps etc.) */
            if (l >= 1) {
                h = fnv1a(h, &id, 1);
                h = fnv1a(h, v, 1);
            }
            break;
        default:
            break;
        }
        i += 2 + l;
    }
    return h;
}

static void remember_ssid(const uint8_t *ie, int len, int64_t now)
{
    if (len < 2 || ie[0] != 0 || ie[1] == 0 || ie[1] > 32 || ie[1] + 2 > len) return;
    char name[33];
    int n = ie[1];
    for (int i = 0; i < n; i++) {
        char c = (char)ie[2 + i];
        name[i] = isprint((unsigned char)c) ? c : '?';
    }
    name[n] = 0;
    for (int i = 0; i < s_ssid_count; i++) {
        if (strcmp(s_ssid[i].ssid, name) == 0) { s_ssid[i].last_seen_us = now; return; }
    }
    int slot = s_ssid_count < MAX_SSIDS ? s_ssid_count++ : 0;
    if (slot == 0 && s_ssid_count == MAX_SSIDS) {       /* replace oldest */
        for (int i = 1; i < MAX_SSIDS; i++)
            if (s_ssid[i].last_seen_us < s_ssid[slot].last_seen_us) slot = i;
    }
    strcpy(s_ssid[slot].ssid, name);
    s_ssid[slot].last_seen_us = now;
}

/* ---------- Wi-Fi sniffer callback (runs in Wi-Fi task: keep it short) ---------- */

static void sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = p->payload;
    int len = p->rx_ctrl.sig_len - 4;                 /* strip FCS */
    if (len < 24 || f[0] != 0x40) return;             /* 0x40 = Probe Request */

    const uint8_t *src = &f[10];                      /* addr2 = transmitter */
    const uint8_t *ie = &f[24];
    int ie_len = len - 24;
    int8_t rssi = p->rx_ctrl.rssi;
    int64_t now = esp_timer_get_time();
    uint32_t fp = ie_fingerprint(ie, ie_len);

    portENTER_CRITICAL(&s_lock);
    s_total_probes++;
    remember_ssid(ie, ie_len, now);

    device_t *d = NULL;
    for (int i = 0; i < s_dev_count; i++) {
        if (memcmp(s_dev[i].mac, src, 6) == 0) { d = &s_dev[i]; break; }
    }
    if (!d) {
        int slot;
        if (s_dev_count < MAX_DEVICES) {
            slot = s_dev_count++;
        } else {                                       /* evict oldest */
            slot = 0;
            for (int i = 1; i < MAX_DEVICES; i++)
                if (s_dev[i].last_seen_us < s_dev[slot].last_seen_us) slot = i;
        }
        d = &s_dev[slot];
        memset(d, 0, sizeof(*d));
        memcpy(d->mac, src, 6);
        d->randomized = (src[0] & 0x02) ? 1 : 0;
        d->rssi = rssi;
    }
    /* keep a slowly-decaying peak so one weak frame doesn't drop a device */
    if (rssi > d->rssi || now - d->last_seen_us > 10 * 1000000LL) d->rssi = rssi;
    d->fingerprint = fp;
    d->frames++;
    d->last_seen_us = now;
    portEXIT_CRITICAL(&s_lock);
}

/* ---------- tasks ---------- */

static void channel_hop_task(void *arg)
{
    uint8_t ch = 1;
    while (1) {
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
        ch = ch % 13 + 1;
        vTaskDelay(pdMS_TO_TICKS(HOP_INTERVAL_MS));
    }
}

static void pir_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIR_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&io);
    int prev = 0;
    while (1) {
        int lvl = gpio_get_level(PIR_GPIO);
        if (lvl) s_pir_last_motion_us = esp_timer_get_time();
        if (lvl && !prev) s_pir_triggers++;
        s_pir_level = lvl;
        prev = lvl;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void handle_command(char *line)
{
    while (*line == ' ') line++;
    if (!*line) return;
    if (isdigit((unsigned char)line[0])) {
        s_headcount = atoi(line);
        printf(">> real head count set to %d\n", s_headcount);
    } else if (line[0] == 't') {
        s_rssi_min = atoi(line + 1);
        printf(">> RSSI threshold set to %d dBm\n", s_rssi_min);
    } else if (line[0] == 'w') {
        int w = atoi(line + 1);
        if (w > 0) s_window_s = w;
        printf(">> window set to %d s\n", s_window_s);
    } else if (line[0] == 'c') {
        portENTER_CRITICAL(&s_lock);
        s_dev_count = 0; s_ssid_count = 0; s_total_probes = 0;
        portEXIT_CRITICAL(&s_lock);
        s_pir_triggers = 0;
        printf(">> cleared\n");
    } else {
        printf(">> commands: <number>=head count, t-65=RSSI min, w60=window s, c=clear\n");
    }
}

static void console_task(void *arg)
{
    uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0);
    char line[32];
    int n = 0;
    uint8_t c;
    while (1) {
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) == 1) {
            if (c == '\r' || c == '\n') {
                line[n] = 0;
                handle_command(line);
                n = 0;
            } else if (n < (int)sizeof(line) - 1) {
                line[n++] = (char)c;
            }
        }
    }
}

/* ---------- report ---------- */

static device_t s_snap[MAX_DEVICES];

static void print_report(void)
{
    int64_t now = esp_timer_get_time();
    int64_t window_us = (int64_t)s_window_s * 1000000LL;
    int rssi_min = s_rssi_min;

    portENTER_CRITICAL(&s_lock);
    int n = s_dev_count;
    memcpy(s_snap, s_dev, n * sizeof(device_t));
    uint32_t total = s_total_probes;
    portEXIT_CRITICAL(&s_lock);

    int any = 0, near = 0, near_rand = 0, near_real = 0;
    static uint32_t fps[MAX_DEVICES];
    int nfp = 0;
    for (int i = 0; i < n; i++) {
        device_t *d = &s_snap[i];
        if (now - d->last_seen_us > window_us) continue;
        any++;
        if (d->rssi < rssi_min) continue;
        near++;
        if (d->randomized) {
            near_rand++;
            /* group randomised MACs that share a fingerprint */
            bool dup = false;
            for (int k = 0; k < nfp; k++) if (fps[k] == d->fingerprint) { dup = true; break; }
            if (!dup) fps[nfp++] = d->fingerprint;
        } else {
            near_real++;
        }
    }
    int estimate = near_real + nfp;

    printf("\n================ Your Phone Is Talking About You ================\n");
    printf(" uptime %llds | window %ds | room filter RSSI >= %d dBm | probes heard %lu\n",
           now / 1000000LL, s_window_s, rssi_min, (unsigned long)total);
    printf(" MACs heard (any distance)      : %d\n", any);
    printf(" MACs in this room              : %d  (%d randomized, %d real)\n",
           near, near_rand, near_real);
    printf(" >>> I count %d devices in this room <<<   (fingerprint estimate)\n", estimate);

    if (s_pir_last_motion_us < 0) {
        printf(" PIR (GPIO%d)                   : no motion yet\n", PIR_GPIO);
    } else {
        printf(" PIR (GPIO%d)                   : %s, last motion %llds ago, %lu triggers\n",
               PIR_GPIO, s_pir_level ? "MOTION" : "idle",
               (now - s_pir_last_motion_us) / 1000000LL, (unsigned long)s_pir_triggers);
    }
    if (s_headcount > 0) {
        printf(" Real head count                : %d  -> %.2f devices/person, %+d vs estimate\n",
               s_headcount, (double)estimate / s_headcount, estimate - s_headcount);
    } else {
        printf(" Real head count                : (type a number + Enter)\n");
    }

    /* strongest devices, MAC partially hidden */
    printf(" --- nearest devices ---\n");
    for (int shown = 0; shown < 8; shown++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (now - s_snap[i].last_seen_us > window_us || s_snap[i].frames == 0) continue;
            if (best < 0 || s_snap[i].rssi > s_snap[best].rssi) best = i;
        }
        if (best < 0) break;
        device_t *d = &s_snap[best];
        printf("  %02X:%02X:xx:xx:xx:%02X  %4d dBm  %-10s fp=%08lX  %lu frames\n",
               d->mac[0], d->mac[1], d->mac[5], d->rssi,
               d->randomized ? "randomized" : "real",
               (unsigned long)d->fingerprint, (unsigned long)d->frames);
        d->frames = 0;  /* mark as shown in snapshot */
    }

    printf(" --- network names phones are asking for ---\n");
    portENTER_CRITICAL(&s_lock);
    int ns = s_ssid_count;
    static ssid_t ss[MAX_SSIDS];
    memcpy(ss, s_ssid, ns * sizeof(ssid_t));
    portEXIT_CRITICAL(&s_lock);
    int printed = 0;
    for (int i = 0; i < ns; i++) {
        if (now - ss[i].last_seen_us > window_us * 5) continue;
        printf("  \"%s\"\n", ss[i].ssid);
        printed++;
    }
    if (!printed) printf("  (none yet - modern phones mostly send wildcard probes)\n");
    printf("==================================================================\n");
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_promiscuous_filter_t filt = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    esp_log_level_set("wifi", ESP_LOG_WARN);

    ESP_LOGI(TAG, "listening for probe requests, PIR on GPIO%d", PIR_GPIO);

    xTaskCreate(channel_hop_task, "hop", 2048, NULL, 5, NULL);
    xTaskCreate(pir_task, "pir", 2048, NULL, 4, NULL);
    xTaskCreate(console_task, "console", 3072, NULL, 3, NULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
        print_report();
    }
}
