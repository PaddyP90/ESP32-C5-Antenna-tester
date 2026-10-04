/*
 * ESP32-C5 antenna tuning aid (2.4 GHz + 5 GHz incl. 5.8 GHz channels)
 *
 * Two boards:
 *   TX    - broadcasts ESP-NOW probes, hopping across a channel list
 *           (sweep) or parked on one channel (lock, via BOOT button).
 *   METER - the board with the antenna under test. Follows the TX hops,
 *           averages RSSI per channel, prints CSV lines over USB serial.
 *
 * Serial output (METER):
 *   S,<ch>,<MHz>,<avg_dBm>,<max_dBm>,<n>   one line per channel in sweep mode
 *   L,<ch>,<MHz>,<avg_dBm>,<max_dBm>,<n>   ~10 lines/s in lock mode
 *
 * RSSI only: relative, ~1 dB resolution. It tells you whether a change to
 * the antenna helped, not what its impedance is.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include <strings.h>
#include <stdlib.h>

/* Kconfig leaves a bool undefined when it is off */
#ifndef CONFIG_TUNER_METER_HT40
#define CONFIG_TUNER_METER_HT40 0
#endif

static const char *TAG = "tuner";

/* Non-DFS channels only: all of 2.4 GHz (1-13), 5.2 GHz low band, 5.8 GHz band. */
static const uint8_t CH[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    36, 40, 44, 48,
    149, 153, 157, 161, 165,
};
#define NCH ((int)(sizeof(CH) / sizeof(CH[0])))

#define PROBE_MAGIC   0x41544E54u
#define PROBE_PERIOD_MS 10
#define LOCK_CH_24    6
#define LOCK_CH_5     149

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  ch;            /* channel the TX is on right now */
    uint8_t  mode;          /* 0 = sweep, 1 = lock */
    uint8_t  bw;            /* TX bandwidth in MHz (20 or 40) */
    uint16_t remaining_ms;  /* sweep: ms until TX hops to next channel */
    uint32_t seq;
} probe_t;

static int ch_freq(uint8_t ch) { return ch <= 14 ? 2407 + 5 * ch : 5000 + 5 * ch; }

static int ch_index(uint8_t ch)
{
    for (int i = 0; i < NCH; i++) if (CH[i] == ch) return i;
    return -1;
}

/* Sets channel (+ bandwidth only when it actually changes).
 * Default is plain 20 MHz with no bandwidth calls at all, which is the
 * configuration known to work. HT40 is opt-in and falls back to 20 MHz if
 * the driver rejects it. Returns true if HT40 is in use. */
static bool bw40_active = false;

static bool set_bw40(bool on)
{
    if (on == bw40_active) return bw40_active;
    wifi_bandwidths_t bw = {
        .ghz_2g = on ? WIFI_BW_HT40 : WIFI_BW_HT20,
        .ghz_5g = on ? WIFI_BW_HT40 : WIFI_BW_HT20,
    };
    esp_err_t e = esp_wifi_set_bandwidths(WIFI_IF_STA, &bw);
    if (e != ESP_OK) {
        /* older/legacy API as a fallback */
        e = esp_wifi_set_bandwidth(WIFI_IF_STA, on ? WIFI_BW_HT40 : WIFI_BW_HT20);
    }
    if (e == ESP_OK) bw40_active = on;
    else ESP_LOGW(TAG, "bandwidth %d MHz rejected: %s", on ? 40 : 20, esp_err_to_name(e));
    return bw40_active;
}

static bool apply_channel(uint8_t ch, bool want40)
{
    wifi_second_chan_t sec = WIFI_SECOND_CHAN_NONE;
    bool can40 = false;
    if (want40) {
        if (ch <= 13) { sec = (ch <= 7) ? WIFI_SECOND_CHAN_ABOVE : WIFI_SECOND_CHAN_BELOW; can40 = true; }
        else if (ch == 36 || ch == 44 || ch == 149 || ch == 157) { sec = WIFI_SECOND_CHAN_ABOVE; can40 = true; }
        else if (ch == 40 || ch == 48 || ch == 153 || ch == 161) { sec = WIFI_SECOND_CHAN_BELOW; can40 = true; }
    }
    bool eff40 = set_bw40(can40);
    if (!eff40) sec = WIFI_SECOND_CHAN_NONE;
    esp_err_t e = esp_wifi_set_channel(ch, sec);
    if (e != ESP_OK) ESP_LOGW(TAG, "set_channel(%u) failed: %s", ch, esp_err_to_name(e));
    return eff40;
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    esp_err_t e = esp_wifi_set_country_code(CONFIG_TUNER_COUNTRY, false);
    if (e != ESP_OK) ESP_LOGW(TAG, "country code: %s", esp_err_to_name(e));

    ESP_ERROR_CHECK(esp_wifi_start());
    /* Enable both bands (C5 only). */
    e = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (e != ESP_OK) ESP_LOGW(TAG, "band mode: %s", esp_err_to_name(e));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static const uint8_t BCAST[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void espnow_start(esp_now_recv_cb_t rx_cb)
{
    ESP_ERROR_CHECK(esp_now_init());
    if (rx_cb) ESP_ERROR_CHECK(esp_now_register_recv_cb(rx_cb));
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BCAST, ESP_NOW_ETH_ALEN);
    peer.channel = 0;               /* 0 = current channel */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer));
}

#if CONFIG_ROLE_TX
/* ------------------------------------------------------------------ TX -- */

typedef struct {
    bool sweep;
    int  idx;        /* index into CH[] */
    bool want40;
    int  dwell_ms;
} tx_state_t;

/* Only touches the ESP-NOW rate when 40 MHz is wanted (or to undo it). */
static bool rate_custom = false;

static void tx_set_rate(bool bw40)
{
    if (!bw40 && !rate_custom) return;   /* keep the default rate that works */
    esp_now_rate_config_t rc = {
        .phymode = bw40 ? WIFI_PHY_MODE_HT40 : WIFI_PHY_MODE_HT20,
        .rate = WIFI_PHY_RATE_MCS0_LGI,
        .ersu = false,
        .dcm = false,
    };
    esp_err_t e = esp_now_set_peer_rate_config(BCAST, &rc);
    if (e != ESP_OK) ESP_LOGW(TAG, "peer rate config: %s", esp_err_to_name(e));
    else rate_custom = true;
}

static bool tx_eff40;

static void tx_apply(const tx_state_t *s)
{
    tx_eff40 = apply_channel(CH[s->idx], s->want40);
    tx_set_rate(tx_eff40);
}

static void tx_status(const tx_state_t *s)
{
    printf("# ok ch=%u freq=%d bw=%d mode=%s dwell=%d\n", CH[s->idx], ch_freq(CH[s->idx]),
           tx_eff40 ? 40 : 20, s->sweep ? "sweep" : "lock", s->dwell_ms);
}

/* Returns true if the radio needs re-applying. */
static bool handle_cmd(char *line, tx_state_t *s)
{
    int v;
    if (strncasecmp(line, "CH ", 3) == 0 && sscanf(line + 3, "%d", &v) == 1) {
        int i = ch_index((uint8_t)v);
        if (i < 0) { printf("# err channel %d not in list\n", v); return false; }
        s->idx = i; s->sweep = false;
        return true;
    }
    if (strncasecmp(line, "SWEEP", 5) == 0) { s->sweep = true; s->idx = 0; return true; }
    if (strncasecmp(line, "BW ", 3) == 0 && sscanf(line + 3, "%d", &v) == 1) {
        if (v != 20 && v != 40) { printf("# err bw must be 20 or 40\n"); return false; }
        s->want40 = (v == 40);
        return true;
    }
    if (strncasecmp(line, "POWER ", 6) == 0 && sscanf(line + 6, "%d", &v) == 1) {
        if (v < 2 || v > 20) { printf("# err power 2..20 dBm\n"); return false; }
        esp_wifi_set_max_tx_power((int8_t)(v * 4));
        printf("# ok power=%d dBm\n", v);
        return false;
    }
    if (strncasecmp(line, "DWELL ", 6) == 0 && sscanf(line + 6, "%d", &v) == 1) {
        if (v < 100 || v > 2000) { printf("# err dwell 100..2000 ms\n"); return false; }
        s->dwell_ms = v;
        printf("# ok dwell=%d\n", v);
        return false;
    }
    if (strncasecmp(line, "STATUS", 6) == 0) { tx_status(s); return false; }
    printf("# err unknown command (CH n | SWEEP | BW 20|40 | POWER dBm | DWELL ms | STATUS)\n");
    return false;
}

static void tx_task(void *arg)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_TUNER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&ucfg);

    tx_state_t st = { .sweep = true, .idx = 0, .want40 = false, .dwell_ms = CONFIG_TUNER_DWELL_MS };
    uint32_t seq = 0;
    char line[64]; int ll = 0;
    tx_apply(&st);
    int64_t dwell_start = esp_timer_get_time();
    tx_status(&st);

    for (;;) {
        bool reapply = false;

        /* Serial commands */
        uint8_t b[32];
        int n = usb_serial_jtag_read_bytes(b, sizeof(b), 0);
        for (int i = 0; i < n; i++) {
            char c = (char)b[i];
            if (c == '\n' || c == '\r') {
                if (ll) { line[ll] = 0; reapply |= handle_cmd(line, &st); ll = 0; }
            } else if (ll < (int)sizeof(line) - 1) {
                line[ll++] = c;
            }
        }

        /* Mode button (active low): sweep -> lock ch6 -> lock ch149 -> sweep */
        if (gpio_get_level(CONFIG_TUNER_BUTTON_GPIO) == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get_level(CONFIG_TUNER_BUTTON_GPIO) == 0) {
                if (st.sweep) { st.sweep = false; st.idx = ch_index(LOCK_CH_24); }
                else if (CH[st.idx] == LOCK_CH_24) { st.idx = ch_index(LOCK_CH_5); }
                else { st.sweep = true; st.idx = 0; }
                reapply = true;
                while (gpio_get_level(CONFIG_TUNER_BUTTON_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        if (reapply) {
            tx_apply(&st);
            dwell_start = esp_timer_get_time();
            tx_status(&st);
            if (st.want40 && !tx_eff40) printf("# note: ch %u has no HT40 pair, using 20 MHz\n", CH[st.idx]);
        }

        int el = (int)((esp_timer_get_time() - dwell_start) / 1000);
        int rem = st.dwell_ms - el;
        probe_t p = {
            .magic = PROBE_MAGIC,
            .ch = CH[st.idx],
            .mode = st.sweep ? 0 : 1,
            .bw = tx_eff40 ? 40 : 20,
            .remaining_ms = (st.sweep && rem > 0) ? (uint16_t)rem : 0,
            .seq = seq++,
        };
        esp_now_send(BCAST, (const uint8_t *)&p, sizeof(p));
        vTaskDelay(pdMS_TO_TICKS(PROBE_PERIOD_MS));

        if (st.sweep && el >= st.dwell_ms) {
            st.idx = (st.idx + 1) % NCH;
            tx_apply(&st);
            dwell_start = esp_timer_get_time();
        }
    }
}

void app_main(void)
{
    wifi_start();
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_TUNER_TX_POWER_QDBM));
    espnow_start(NULL);
    ESP_LOGI(TAG, "TX role, %d channels. Serial cmds: CH n | SWEEP | BW 20|40 | POWER dBm | DWELL ms | STATUS", NCH);
    xTaskCreate(tx_task, "tx", 6144, NULL, 5, NULL);
}

#else
/* --------------------------------------------------------------- METER -- */

typedef struct {
    int8_t  rssi;
    uint8_t ch;
    uint8_t mode;
    uint8_t bw;
    uint16_t remaining_ms;
} rx_item_t;

static QueueHandle_t rxq;
static uint8_t last_bw;

static void meter_set_ch(uint8_t ch)
{
    apply_channel(ch, CONFIG_TUNER_METER_HT40);
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != (int)sizeof(probe_t) || !info || !info->rx_ctrl) return;
    const probe_t *p = (const probe_t *)data;
    if (p->magic != PROBE_MAGIC) return;
    rx_item_t it = {
        .rssi = (int8_t)info->rx_ctrl->rssi,
        .ch = p->ch,
        .mode = p->mode,
        .bw = p->bw,
        .remaining_ms = p->remaining_ms,
    };
    xQueueSend(rxq, &it, 0);   /* never block the Wi-Fi task */
}

typedef struct { int sum, n; int8_t max; } acc_t;

static void acc_reset(acc_t *a) { a->sum = 0; a->n = 0; a->max = -127; }

static void acc_add(acc_t *a, int8_t r)
{
    a->sum += r; a->n++;
    if (r > a->max) a->max = r;
}

static void report(char tag, uint8_t ch, const acc_t *a)
{
    if (a->n == 0) return;
    printf("%c,%u,%d,%.1f,%d,%d\n", tag, ch, ch_freq(ch), (double)a->sum / a->n, a->max, a->n);
}

#define HOP_GUARD_MS   12     /* hop slightly after the TX does */
#define LOST_MS        300    /* no packets for this long -> hunt */
#define HUNT_DWELL_MS  120

static void meter_task(void *arg)
{
    int cur = 0;
    acc_t acc; acc_reset(&acc);
    int64_t last_rx = 0, last_hop = esp_timer_get_time(), last_print = 0, deadline = 0;
    bool have_deadline = false;
    meter_set_ch(CH[cur]);

    for (;;) {
        rx_item_t it;
        int64_t now = esp_timer_get_time();

        if (xQueueReceive(rxq, &it, pdMS_TO_TICKS(10)) == pdTRUE) {
            now = esp_timer_get_time();
            last_rx = now;
            if (it.bw != last_bw) { last_bw = it.bw; printf("# tx bw=%u\n", it.bw); }
            if (it.ch != CH[cur]) {
                /* Adjacent-channel leakage or lost sync: re-sync, drop sample. */
                int k = ch_index(it.ch);
                if (k >= 0) { cur = k; meter_set_ch(CH[cur]); acc_reset(&acc); last_hop = now; }
                have_deadline = false;
                continue;
            }
            acc_add(&acc, it.rssi);
            if (it.mode == 0) {
                deadline = now + (int64_t)(it.remaining_ms + HOP_GUARD_MS) * 1000;
                have_deadline = true;
            } else {
                have_deadline = false;
                if (now - last_print > 100000 && acc.n > 0) {
                    report('L', CH[cur], &acc);
                    acc_reset(&acc);
                    last_print = now;
                }
            }
        } else if (now - last_rx > (int64_t)LOST_MS * 1000 &&
                   now - last_hop > (int64_t)HUNT_DWELL_MS * 1000) {
            /* Lost the TX: hop through the list until we hear it again. */
            cur = (cur + 1) % NCH;
            meter_set_ch(CH[cur]);
            acc_reset(&acc);
            last_hop = now;
            have_deadline = false;
        }

        if (have_deadline && esp_timer_get_time() >= deadline) {
            report('S', CH[cur], &acc);
            acc_reset(&acc);
            cur = (cur + 1) % NCH;
            meter_set_ch(CH[cur]);
            last_hop = esp_timer_get_time();
            have_deadline = false;
        }
    }
}

void app_main(void)
{
    rxq = xQueueCreate(64, sizeof(rx_item_t));
    wifi_start();
    espnow_start(on_recv);
    printf("# c5-antenna-tuner meter, %d channels\n", NCH);
    xTaskCreate(meter_task, "meter", 4096, NULL, 5, NULL);
}
#endif
