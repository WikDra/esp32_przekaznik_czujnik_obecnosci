/*
 * HLK-LD2420 24 GHz presence radar driver (UART).
 *
 * Board variant used here: HLK-LD2420 v2.1, pads: 3V3 | GND | OT1 | RX | OT2
 *   OT1 -> module UART TX  (connect to ESP RX)
 *   RX  -> module UART RX  (connect to ESP TX)
 *   OT2 -> digital presence output (optional)
 * Default baud rate for firmware >= v1.5.4 is 115200 8N1 (older ones: 256000).
 *
 * Protocol (little endian):
 *   header FD FC FB FA | len(2) | cmd(2) | data | footer 04 03 02 01
 * Data streams:
 *   simple mode  ("ON/OFF Range xxx\r\n")           - system mode 0x0064
 *   energy mode  (F4 F3 F2 F1 ... F8 F7 F6 F5)      - system mode 0x0004
 */
#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LD2420_GATES 16

/* System modes (register 0x0000 written with CMD 0x0012). */
#define LD2420_MODE_SIMPLE 0x0064
#define LD2420_MODE_ENERGY 0x0004
#define LD2420_MODE_DEBUG  0x0000

typedef struct {
    bool link_ok;                        /* at least one frame parsed recently   */
    bool presence;
    uint16_t distance_cm;
    uint16_t mode;                       /* LD2420_MODE_*                        */
    uint16_t gate_energy[LD2420_GATES];  /* energy mode only                     */
    int64_t last_frame_us;
    char fw[16];
    /* configuration read back from / written to the module NVM */
    uint16_t min_gate;                   /* gate = 0.7 m each                    */
    uint16_t max_gate;
    uint16_t timeout_s;                  /* module-side presence hold time       */
    uint32_t move_thresh[LD2420_GATES];
    uint32_t still_thresh[LD2420_GATES];
    bool config_valid;
} ld2420_state_t;

typedef void (*ld2420_presence_cb_t)(bool presence, uint16_t distance_cm);

/* Starts UART + reader task, reads firmware version and current configuration. */
esp_err_t ld2420_init(ld2420_presence_cb_t cb);

/* Snapshot of the current sensor state (thread-safe copy). */
void ld2420_get_state(ld2420_state_t *out);

/* --- configuration (all of these talk to the module and block up to ~2 s) --- */
esp_err_t ld2420_refresh_config(void);
esp_err_t ld2420_set_ranges(uint16_t min_gate, uint16_t max_gate, uint16_t timeout_s);
esp_err_t ld2420_set_gate_threshold(uint8_t gate, uint32_t move_thresh, uint32_t still_thresh);
esp_err_t ld2420_set_mode(uint16_t mode);
esp_err_t ld2420_factory_reset(void);
esp_err_t ld2420_restart(void);

#ifdef __cplusplus
}
#endif
