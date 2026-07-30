/*
 * HLK-LD2420 driver. Protocol constants follow the module datasheet and the
 * (well tested) ESPHome ld2420 component.
 */
#include "ld2420.h"

#include <string.h>
#include <stdlib.h>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_check.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>

static const char *TAG = "ld2420";

/* ------------------------------------------------------------ protocol */

#define CMD_FRAME_HEADER 0xFAFBFCFDu
#define CMD_FRAME_FOOTER 0x01020304u
#define ENERGY_FRAME_HEADER 0xF1F2F3F4u
#define ENERGY_FRAME_FOOTER 0xF5F6F7F8u

#define CMD_ENABLE_CONF 0x00FF
#define CMD_DISABLE_CONF 0x00FE
#define CMD_PROTOCOL_VER 0x0002
#define CMD_READ_VERSION 0x0000
#define CMD_WRITE_ABD_PARAM 0x0007
#define CMD_READ_ABD_PARAM 0x0008
#define CMD_WRITE_SYS_PARAM 0x0012
#define CMD_RESTART 0x0068

#define REG_MIN_GATE 0x0000
#define REG_MAX_GATE 0x0001
#define REG_TIMEOUT 0x0004
#define REG_SYSTEM_MODE 0x0000
#define REG_MOVE_THRESH(gate) ((uint16_t)(0x0010 + (gate)))
#define REG_STILL_THRESH(gate) ((uint16_t)(0x0020 + (gate)))

#define ENERGY_FRAME_LEN 45
#define PARSE_BUF_LEN 64
#define LINK_TIMEOUT_US (10 * 1000 * 1000LL)

static const uint32_t FACTORY_MOVE_THRESH[LD2420_GATES] = {60000, 30000, 400, 250, 250, 250, 250, 250,
                                                           250,   250,   250, 250, 250, 250, 250, 250};
static const uint32_t FACTORY_STILL_THRESH[LD2420_GATES] = {40000, 20000, 200, 200, 200, 200, 200, 150,
                                                            150,   100,   100, 100, 100, 100, 100, 100};
#define FACTORY_TIMEOUT 120
#define FACTORY_MIN_GATE 1
#define FACTORY_MAX_GATE 12

/* ------------------------------------------------------------ state */

static const uart_port_t s_port = (uart_port_t)CONFIG_APP_LD2420_UART_PORT;
static SemaphoreHandle_t s_lock;          /* serializes UART access + state */
static ld2420_state_t s_state;
static ld2420_presence_cb_t s_cb;

static uint8_t s_parse_buf[PARSE_BUF_LEN];
static size_t s_parse_pos;

typedef struct {
    bool got_ack;
    uint8_t command;   /* low byte of the echoed command */
    uint16_t error;
    uint32_t data[4];
} ld2420_reply_t;

static ld2420_reply_t s_reply;

static inline void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_lock); }

/* ------------------------------------------------------------ parsing */

static void handle_energy_frame(const uint8_t *buf, size_t len)
{
    if (len < ENERGY_FRAME_LEN) {
        return;
    }
    /* frame: header(4) len(2) presence(1) distance(2) 16x energy(2) footer(4) */
    const uint8_t *p = buf + len - ENERGY_FRAME_LEN;
    uint32_t header;
    memcpy(&header, p, sizeof(header));
    if (header != ENERGY_FRAME_HEADER) {
        return;
    }

    bool presence = p[6] != 0;
    uint16_t distance;
    memcpy(&distance, p + 7, sizeof(distance));

    s_state.presence = presence;
    s_state.distance_cm = distance;
    s_state.last_frame_us = esp_timer_get_time();
    s_state.link_ok = true;
    for (int i = 0; i < LD2420_GATES; i++) {
        memcpy(&s_state.gate_energy[i], p + 9 + i * 2, sizeof(uint16_t));
    }
}

static void handle_simple_line(const uint8_t *buf, size_t len)
{
    /* e.g. "ON Range 123\r\n" / "OFF\r\n" */
    char line[PARSE_BUF_LEN];
    size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
    memcpy(line, buf, n);
    line[n] = '\0';

    bool presence = s_state.presence;
    if (strstr(line, "OFF") != NULL) {
        presence = false;
    } else if (strstr(line, "ON") != NULL) {
        presence = true;
    } else {
        return;
    }

    uint16_t distance = s_state.distance_cm;
    const char *digits = line;
    while (*digits && (*digits < '0' || *digits > '9')) {
        digits++;
    }
    if (*digits) {
        long v = strtol(digits, NULL, 10);
        if (v >= 0 && v <= 65535) {
            distance = (uint16_t)v;
        }
    }

    s_state.presence = presence;
    s_state.distance_cm = distance;
    s_state.last_frame_us = esp_timer_get_time();
    s_state.link_ok = true;
}

static void handle_ack_frame(const uint8_t *buf, size_t len)
{
    if (len < 12) {
        return;
    }
    /* header(4) len(2) cmd(2) error(2) [data...] footer(4) */
    s_reply.command = buf[6];
    memcpy(&s_reply.error, buf + 8, sizeof(s_reply.error));
    memset(s_reply.data, 0, sizeof(s_reply.data));

    if (s_reply.command == (CMD_READ_VERSION & 0xFF) && len >= 14) {
        uint8_t ver_len = buf[10];
        if (ver_len > sizeof(s_state.fw) - 1) {
            ver_len = sizeof(s_state.fw) - 1;
        }
        if ((size_t)(12 + ver_len) <= len) {
            memcpy(s_state.fw, buf + 12, ver_len);
            s_state.fw[ver_len] = '\0';
        }
    } else if (s_reply.command == (CMD_READ_ABD_PARAM & 0xFF)) {
        /* data words start at offset 0x0A, 4 bytes each */
        size_t avail = (len >= 14) ? (len - 10 - 4) / 4 : 0;
        if (avail > 4) {
            avail = 4;
        }
        for (size_t i = 0; i < avail; i++) {
            memcpy(&s_reply.data[i], buf + 10 + i * 4, sizeof(uint32_t));
        }
    }
    s_reply.got_ack = true;
}

/* Feeds one received byte into the frame parser. Caller holds the lock. */
static void parse_byte(uint8_t b)
{
    if (s_parse_pos >= PARSE_BUF_LEN) {
        /* keep the tail so a footer can still be matched after a desync */
        memmove(s_parse_buf, s_parse_buf + PARSE_BUF_LEN / 2, PARSE_BUF_LEN / 2);
        s_parse_pos = PARSE_BUF_LEN / 2;
    }
    s_parse_buf[s_parse_pos++] = b;

    if (s_parse_pos < 4) {
        return;
    }

    uint32_t tail;
    memcpy(&tail, s_parse_buf + s_parse_pos - 4, sizeof(tail));

    if (tail == CMD_FRAME_FOOTER) {
        handle_ack_frame(s_parse_buf, s_parse_pos);
        s_parse_pos = 0;
    } else if (tail == ENERGY_FRAME_FOOTER) {
        handle_energy_frame(s_parse_buf, s_parse_pos);
        s_parse_pos = 0;
    } else if (s_state.mode == LD2420_MODE_SIMPLE && s_parse_pos >= 2 &&
               s_parse_buf[s_parse_pos - 2] == '\r' && s_parse_buf[s_parse_pos - 1] == '\n') {
        handle_simple_line(s_parse_buf, s_parse_pos - 2);
        s_parse_pos = 0;
    }
}

/* ------------------------------------------------------------ commands */

/* Sends a command frame and waits for its ack. Caller must hold the lock. */
static esp_err_t send_cmd(uint16_t command, const uint8_t *data, size_t data_len, bool expect_ack)
{
    uint8_t frame[64];
    size_t pos = 0;
    uint32_t header = CMD_FRAME_HEADER;
    uint32_t footer = CMD_FRAME_FOOTER;
    uint16_t len_field = (uint16_t)(data_len + 2);

    if (data_len > sizeof(frame) - 12) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(frame + pos, &header, 4);
    pos += 4;
    memcpy(frame + pos, &len_field, 2);
    pos += 2;
    memcpy(frame + pos, &command, 2);
    pos += 2;
    if (data_len) {
        memcpy(frame + pos, data, data_len);
        pos += data_len;
    }
    memcpy(frame + pos, &footer, 4);
    pos += 4;

    for (int attempt = 0; attempt < 2; attempt++) {
        s_reply.got_ack = false;
        s_reply.error = 0;
        s_parse_pos = 0;
        uart_write_bytes(s_port, frame, pos);

        if (!expect_ack) {
            return ESP_OK;
        }

        int64_t deadline = esp_timer_get_time() + 700 * 1000;
        while (esp_timer_get_time() < deadline) {
            uint8_t rx[64];
            int n = uart_read_bytes(s_port, rx, sizeof(rx), pdMS_TO_TICKS(20));
            for (int i = 0; i < n; i++) {
                parse_byte(rx[i]);
            }
            if (s_reply.got_ack && s_reply.command == (command & 0xFF)) {
                if (s_reply.error != 0) {
                    ESP_LOGW(TAG, "cmd 0x%04X returned error 0x%04X", command, s_reply.error);
                    return ESP_FAIL;
                }
                return ESP_OK;
            }
        }
        ESP_LOGD(TAG, "cmd 0x%04X timeout (attempt %d)", command, attempt + 1);
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t config_mode(bool enable)
{
    if (enable) {
        uint16_t ver = CMD_PROTOCOL_VER;
        return send_cmd(CMD_ENABLE_CONF, (const uint8_t *)&ver, sizeof(ver), true);
    }
    return send_cmd(CMD_DISABLE_CONF, NULL, 0, true);
}

static esp_err_t read_abd(const uint16_t *regs, size_t count)
{
    uint8_t data[12];
    if (count > 3) {
        count = 3;
    }
    for (size_t i = 0; i < count; i++) {
        memcpy(data + i * 2, &regs[i], 2);
    }
    return send_cmd(CMD_READ_ABD_PARAM, data, count * 2, true);
}

static esp_err_t write_abd(const uint16_t *regs, const uint32_t *values, size_t count)
{
    uint8_t data[18];
    if (count > 3) {
        count = 3;
    }
    for (size_t i = 0; i < count; i++) {
        memcpy(data + i * 6, &regs[i], 2);
        memcpy(data + i * 6 + 2, &values[i], 4);
    }
    return send_cmd(CMD_WRITE_ABD_PARAM, data, count * 6, true);
}

static esp_err_t write_system_mode(uint16_t mode)
{
    uint8_t data[6];
    uint16_t reg = REG_SYSTEM_MODE;
    uint16_t pad = 0;
    memcpy(data, &reg, 2);
    memcpy(data + 2, &mode, 2);
    memcpy(data + 4, &pad, 2);
    esp_err_t err = send_cmd(CMD_WRITE_SYS_PARAM, data, sizeof(data), true);
    if (err == ESP_OK) {
        s_state.mode = mode;
    }
    return err;
}

static int fw_version_int(const char *v)
{
    /* "v1.5.4" -> 154 */
    int result = 0;
    for (const char *p = (*v == 'v') ? v + 1 : v; *p; p++) {
        if (*p == '.') {
            continue;
        }
        if (*p < '0' || *p > '9') {
            return 0;
        }
        result = result * 10 + (*p - '0');
    }
    return result;
}

static esp_err_t read_config_locked(void)
{
    const uint16_t range_regs[3] = {REG_MIN_GATE, REG_MAX_GATE, REG_TIMEOUT};
    esp_err_t err = read_abd(range_regs, 3);
    if (err != ESP_OK) {
        return err;
    }
    s_state.min_gate = (uint16_t)s_reply.data[0];
    s_state.max_gate = (uint16_t)s_reply.data[1];
    s_state.timeout_s = (uint16_t)s_reply.data[2];

    for (uint8_t gate = 0; gate < LD2420_GATES; gate++) {
        const uint16_t regs[2] = {REG_MOVE_THRESH(gate), REG_STILL_THRESH(gate)};
        if (read_abd(regs, 2) != ESP_OK) {
            return ESP_FAIL;
        }
        s_state.move_thresh[gate] = s_reply.data[0];
        s_state.still_thresh[gate] = s_reply.data[1];
    }
    s_state.config_valid = true;
    return ESP_OK;
}

/* ------------------------------------------------------------ tasks */

static void reader_task(void *arg)
{
    (void)arg;
    bool last_presence = false;
    bool has_last = false;
    uint16_t last_distance = 0;
    int64_t last_cb_us = 0;

    for (;;) {
        uint8_t rx[128];
        int n = uart_read_bytes(s_port, rx, sizeof(rx), pdMS_TO_TICKS(50));
        bool changed = false;
        bool presence = false;
        uint16_t distance = 0;

        lock();
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                parse_byte(rx[i]);
            }
        }
        if (s_state.link_ok && (esp_timer_get_time() - s_state.last_frame_us) > LINK_TIMEOUT_US) {
            s_state.link_ok = false;
            s_state.presence = false;
            ESP_LOGW(TAG, "no data from sensor for %d s", (int)(LINK_TIMEOUT_US / 1000000));
        }
        presence = s_state.presence;
        distance = s_state.distance_cm;
        changed = !has_last || presence != last_presence || distance != last_distance;
        unlock();

        /* Callback przy zmianie, ale też nie rzadziej niż raz na sekundę: aplikacja
         * musi móc przeliczyć swoje okno odległości i tryb nocny po zmianie ustawień,
         * nawet gdy odczyty z modułu stoją w miejscu. */
        const int64_t now_us = esp_timer_get_time();
        if (changed || (now_us - last_cb_us) >= 1000 * 1000) {
            has_last = true;
            last_presence = presence;
            last_distance = distance;
            last_cb_us = now_us;
            if (s_cb) {
                s_cb(presence, distance);
            }
        }
    }
}

/* Handshake in a separate task so a missing/miswired sensor never blocks boot. */
static void config_task(void *arg)
{
    (void)arg;
    for (;;) {
        lock();
        esp_err_t err = config_mode(true);
        if (err == ESP_OK) {
            if (send_cmd(CMD_READ_VERSION, NULL, 0, true) == ESP_OK) {
                ESP_LOGI(TAG, "firmware %s", s_state.fw);
            }
            if (read_config_locked() == ESP_OK) {
                ESP_LOGI(TAG, "config: min_gate=%u max_gate=%u timeout=%us",
                         s_state.min_gate, s_state.max_gate, s_state.timeout_s);
            }
            uint16_t mode = (fw_version_int(s_state.fw) >= 154) ? LD2420_MODE_ENERGY : LD2420_MODE_SIMPLE;
            write_system_mode(mode);
            config_mode(false);
            s_state.link_ok = true;
            s_state.last_frame_us = esp_timer_get_time();
            unlock();
            ESP_LOGI(TAG, "sensor ready (mode=%s)", mode == LD2420_MODE_ENERGY ? "energy" : "simple");
            vTaskDelete(NULL);
            return;
        }
        unlock();
        ESP_LOGW(TAG, "sensor not responding on UART%d (rx=%d tx=%d), retrying in 30 s",
                 (int)s_port, CONFIG_APP_LD2420_RX_GPIO, CONFIG_APP_LD2420_TX_GPIO);
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* ------------------------------------------------------------ public API */

esp_err_t ld2420_init(ld2420_presence_cb_t cb)
{
    if (s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_cb = cb;
    memset(&s_state, 0, sizeof(s_state));
    s_state.mode = LD2420_MODE_ENERGY;
    strcpy(s_state.fw, "unknown");

    const uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_driver_install(s_port, 1024, 256, 0, NULL, 0), TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(s_port, &cfg), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(s_port, CONFIG_APP_LD2420_TX_GPIO, CONFIG_APP_LD2420_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin");
    gpio_set_pull_mode((gpio_num_t)CONFIG_APP_LD2420_RX_GPIO, GPIO_PULLUP_ONLY);

#if CONFIG_APP_LD2420_OT2_GPIO >= 0
    gpio_config_t ot2 = {
        .pin_bit_mask = 1ULL << CONFIG_APP_LD2420_OT2_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ot2);
#endif

    if (xTaskCreate(reader_task, "ld2420_rx", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(config_task, "ld2420_cfg", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "driver started: UART%d rx=%d tx=%d @115200",
             (int)s_port, CONFIG_APP_LD2420_RX_GPIO, CONFIG_APP_LD2420_TX_GPIO);
    return ESP_OK;
}

void ld2420_get_state(ld2420_state_t *out)
{
    if (!out) {
        return;
    }
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    lock();
    memcpy(out, &s_state, sizeof(*out));
    unlock();
}

esp_err_t ld2420_refresh_config(void)
{
    lock();
    esp_err_t err = config_mode(true);
    if (err == ESP_OK) {
        err = read_config_locked();
        config_mode(false);
    }
    unlock();
    return err;
}

esp_err_t ld2420_set_ranges(uint16_t min_gate, uint16_t max_gate, uint16_t timeout_s)
{
    if (min_gate > 15 || max_gate > 15 || min_gate > max_gate) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t regs[3] = {REG_MIN_GATE, REG_MAX_GATE, REG_TIMEOUT};
    const uint32_t vals[3] = {min_gate, max_gate, timeout_s};

    lock();
    uint16_t mode = s_state.mode;
    esp_err_t err = config_mode(true);
    if (err == ESP_OK) {
        err = write_abd(regs, vals, 3);
        if (err == ESP_OK) {
            s_state.min_gate = min_gate;
            s_state.max_gate = max_gate;
            s_state.timeout_s = timeout_s;
        }
        write_system_mode(mode);
        config_mode(false);
    }
    unlock();
    return err;
}

esp_err_t ld2420_set_gate_threshold(uint8_t gate, uint32_t move_thresh, uint32_t still_thresh)
{
    if (gate >= LD2420_GATES) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t regs[2] = {REG_MOVE_THRESH(gate), REG_STILL_THRESH(gate)};
    const uint32_t vals[2] = {move_thresh, still_thresh};

    lock();
    uint16_t mode = s_state.mode;
    esp_err_t err = config_mode(true);
    if (err == ESP_OK) {
        err = write_abd(regs, vals, 2);
        if (err == ESP_OK) {
            s_state.move_thresh[gate] = move_thresh;
            s_state.still_thresh[gate] = still_thresh;
        }
        write_system_mode(mode);
        config_mode(false);
    }
    unlock();
    return err;
}

esp_err_t ld2420_set_mode(uint16_t mode)
{
    if (mode != LD2420_MODE_SIMPLE && mode != LD2420_MODE_ENERGY) {
        return ESP_ERR_INVALID_ARG;
    }
    lock();
    esp_err_t err = config_mode(true);
    if (err == ESP_OK) {
        err = write_system_mode(mode);
        config_mode(false);
    }
    unlock();
    return err;
}

esp_err_t ld2420_factory_reset(void)
{
    lock();
    uint16_t mode = s_state.mode;
    esp_err_t err = config_mode(true);
    if (err == ESP_OK) {
        const uint16_t regs[3] = {REG_MIN_GATE, REG_MAX_GATE, REG_TIMEOUT};
        const uint32_t vals[3] = {FACTORY_MIN_GATE, FACTORY_MAX_GATE, FACTORY_TIMEOUT};
        err = write_abd(regs, vals, 3);
        for (uint8_t gate = 0; gate < LD2420_GATES && err == ESP_OK; gate++) {
            const uint16_t g_regs[2] = {REG_MOVE_THRESH(gate), REG_STILL_THRESH(gate)};
            const uint32_t g_vals[2] = {FACTORY_MOVE_THRESH[gate], FACTORY_STILL_THRESH[gate]};
            err = write_abd(g_regs, g_vals, 2);
            vTaskDelay(1);
        }
        write_system_mode(mode);
        config_mode(false);
        if (err == ESP_OK) {
            read_config_locked();
        }
    }
    unlock();
    return err;
}

esp_err_t ld2420_restart(void)
{
    lock();
    esp_err_t err = send_cmd(CMD_RESTART, NULL, 0, false);
    unlock();
    return err;
}
