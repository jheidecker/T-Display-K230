#include "ui_meshtasticd.h"

#include "ui_common.h"
#include "ui_i18n.h"
#include "ui_prefs.h"

#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define MESHTASTICD_BIN "/usr/bin/meshtasticd"
#define MESHTASTICD_INIT "/etc/init.d/S90meshtasticd"
#define MESHTASTICD_PID "/var/run/meshtasticd.pid"
#define MESHTASTICD_BRIDGE_PID "/var/run/k230_meshtastic_ble_bridge.pid"
#define MESHTASTICD_LOG "/tmp/k230_meshtasticd.log"
#define MESHTASTICD_BLE_STATUS "/tmp/k230_meshtastic_ble_status"
#define MESHTASTICD_PREF_AUTOSTART "meshtasticd.autostart"
#define MESHTASTICD_LOG_TAIL_BYTES 2048
#define MESHTASTICD_LOG_TAIL_LINES 6

typedef struct {
    char name[48];
    char pin[16];
    int phone_connected;
    int known;
} meshtasticd_ble_status_t;

static pthread_mutex_t meshtasticd_lock = PTHREAD_MUTEX_INITIALIZER;
static int meshtasticd_busy;
static int meshtasticd_paused;
static char meshtasticd_action_text[96];
static char meshtasticd_version[48];

static lv_timer_t *meshtasticd_timer;
static lv_obj_t *meshtasticd_status_label;
static lv_obj_t *meshtasticd_ble_label;
static lv_obj_t *meshtasticd_pin_label;
static lv_obj_t *meshtasticd_version_label;
static lv_obj_t *meshtasticd_hint_label;
static lv_obj_t *meshtasticd_log_label;
static lv_obj_t *meshtasticd_start_btn;
static lv_obj_t *meshtasticd_stop_btn;
static lv_obj_t *meshtasticd_autostart_switch;

int ui_meshtasticd_installed(void)
{
    static int installed = -1;

    if(installed < 0) {
        installed = access(MESHTASTICD_BIN, X_OK) == 0 &&
                    access(MESHTASTICD_INIT, X_OK) == 0;
    }
    return installed;
}

static int meshtasticd_pid_running(const char *pid_path)
{
    FILE *fp = fopen(pid_path, "r");
    long pid = 0;

    if(!fp) {
        return 0;
    }
    if(fscanf(fp, "%ld", &pid) != 1) {
        pid = 0;
    }
    fclose(fp);
    return pid > 0 && kill((pid_t)pid, 0) == 0;
}

int ui_meshtasticd_is_running(void)
{
    return meshtasticd_pid_running(MESHTASTICD_PID);
}

int ui_meshtasticd_autostart_enabled(void)
{
    char value[8];

    ui_prefs_get(MESHTASTICD_PREF_AUTOSTART, value, sizeof(value), "1");
    return strcmp(value, "0") != 0;
}

void ui_meshtasticd_set_autostart_enabled(int enabled)
{
    ui_prefs_set(MESHTASTICD_PREF_AUTOSTART, enabled ? "1" : "0");
}

static int meshtasticd_run_init(const char *verb)
{
    char cmd[160];

    snprintf(cmd, sizeof(cmd), "%s %s >/dev/null 2>&1", MESHTASTICD_INIT,
             verb);
    return ui_shell_exit_code(system(cmd));
}

static void *meshtasticd_action_thread(void *arg)
{
    const char *verb = (const char *)arg;
    int rc = meshtasticd_run_init(verb);

    pthread_mutex_lock(&meshtasticd_lock);
    meshtasticd_busy = 0;
    if(rc != 0) {
        snprintf(meshtasticd_action_text, sizeof(meshtasticd_action_text),
                 "\"%s\" failed (%d), see %s", verb, rc, MESHTASTICD_LOG);
    } else {
        meshtasticd_action_text[0] = '\0';
    }
    pthread_mutex_unlock(&meshtasticd_lock);
    app_request_fast_refresh();
    return NULL;
}

/* verb must be a string literal: the thread keeps the pointer. */
static void meshtasticd_start_action(const char *verb, const char *busy_text)
{
    pthread_t thread;

    pthread_mutex_lock(&meshtasticd_lock);
    if(meshtasticd_busy) {
        pthread_mutex_unlock(&meshtasticd_lock);
        return;
    }
    meshtasticd_busy = 1;
    snprintf(meshtasticd_action_text, sizeof(meshtasticd_action_text), "%s",
             busy_text ? busy_text : "");
    pthread_mutex_unlock(&meshtasticd_lock);

    if(pthread_create(&thread, NULL, meshtasticd_action_thread,
                      (void *)verb) == 0) {
        pthread_detach(thread);
    } else {
        pthread_mutex_lock(&meshtasticd_lock);
        meshtasticd_busy = 0;
        snprintf(meshtasticd_action_text, sizeof(meshtasticd_action_text),
                 "%s", "Thread failed");
        pthread_mutex_unlock(&meshtasticd_lock);
    }
}

void ui_meshtasticd_startup(void)
{
    /* At boot S90meshtasticd has already run; after an nRF52840 update the
     * daemon is still up but the bridge was stopped. */
    if(ui_meshtasticd_is_running()) {
        meshtasticd_start_action("bridge", NULL);
    } else if(ui_meshtasticd_autostart_enabled()) {
        meshtasticd_start_action("start", "Starting...");
    }
}

void ui_meshtasticd_pause_for_radio_owner(const char *owner)
{
    int was_running = ui_meshtasticd_is_running();

    /* Synchronous: the LoRa page needs the SPI radio as soon as it opens. */
    if(was_running) {
        (void)meshtasticd_run_init("stop");
    }
    pthread_mutex_lock(&meshtasticd_lock);
    meshtasticd_paused = meshtasticd_paused || was_running;
    pthread_mutex_unlock(&meshtasticd_lock);
    (void)owner;
}

void ui_meshtasticd_resume_after_radio_owner(void)
{
    int paused;

    pthread_mutex_lock(&meshtasticd_lock);
    paused = meshtasticd_paused;
    meshtasticd_paused = 0;
    pthread_mutex_unlock(&meshtasticd_lock);
    if(paused) {
        meshtasticd_start_action("run", "Resuming...");
    }
}

void ui_meshtasticd_apply_ble_setting(void)
{
    if(ui_meshtasticd_is_running()) {
        meshtasticd_start_action("bridge", NULL);
    }
}

static void meshtasticd_read_ble_status(meshtasticd_ble_status_t *st)
{
    FILE *fp;
    char line[96];

    memset(st, 0, sizeof(*st));
    fp = fopen(MESHTASTICD_BLE_STATUS, "r");
    if(!fp) {
        return;
    }
    st->known = 1;
    while(fgets(line, sizeof(line), fp)) {
        ui_trim_text(line);
        if(strncmp(line, "name=", 5) == 0) {
            snprintf(st->name, sizeof(st->name), "%s", line + 5);
        } else if(strncmp(line, "pin=", 4) == 0) {
            snprintf(st->pin, sizeof(st->pin), "%s", line + 4);
        } else if(strncmp(line, "phone=", 6) == 0) {
            st->phone_connected = strcmp(line + 6, "connected") == 0;
        }
    }
    fclose(fp);
}

/* The firmware logs "S:B:<hw>,<version>,<env>,..." near the top. */
static void meshtasticd_read_version(void)
{
    FILE *fp;
    char line[256];

    if(meshtasticd_version[0]) {
        return;
    }
    fp = fopen(MESHTASTICD_LOG, "r");
    if(!fp) {
        return;
    }
    for(int i = 0; i < 200 && fgets(line, sizeof(line), fp); i++) {
        char *p = strstr(line, "S:B:");
        char *start;
        char *end;

        if(!p || !(start = strchr(p, ','))) {
            continue;
        }
        start++;
        end = strchr(start, ',');
        if(end) {
            *end = '\0';
        }
        ui_trim_text(start);
        snprintf(meshtasticd_version, sizeof(meshtasticd_version), "%s",
                 start);
        break;
    }
    fclose(fp);
}

static void meshtasticd_read_log_tail(char *out, size_t len)
{
    FILE *fp;
    char buf[MESHTASTICD_LOG_TAIL_BYTES + 1];
    size_t n;
    long size;
    char *p;
    int lines = 0;

    snprintf(out, len, "%s", "No log yet");
    fp = fopen(MESHTASTICD_LOG, "r");
    if(!fp) {
        return;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, size > MESHTASTICD_LOG_TAIL_BYTES ?
              size - MESHTASTICD_LOG_TAIL_BYTES : 0, SEEK_SET);
    n = fread(buf, 1, MESHTASTICD_LOG_TAIL_BYTES, fp);
    fclose(fp);
    buf[n] = '\0';
    while(n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    if(n == 0) {
        return;
    }
    p = buf + n;
    while(p > buf) {
        if(*(p - 1) == '\n' && ++lines == MESHTASTICD_LOG_TAIL_LINES) {
            break;
        }
        p--;
    }
    snprintf(out, len, "%s", p);
}

static void meshtasticd_start_event_cb(lv_event_t *event)
{
    (void)event;
    pthread_mutex_lock(&meshtasticd_lock);
    meshtasticd_paused = 0;
    pthread_mutex_unlock(&meshtasticd_lock);
    meshtasticd_start_action("run", "Starting...");
}

static void meshtasticd_stop_event_cb(lv_event_t *event)
{
    (void)event;
    meshtasticd_start_action("stop", "Stopping...");
}

static void meshtasticd_autostart_event_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);

    ui_meshtasticd_set_autostart_enabled(lv_obj_has_state(sw,
                                                          LV_STATE_CHECKED));
}

static void meshtasticd_set_enabled(lv_obj_t *btn, int enabled)
{
    if(!btn) {
        return;
    }
    if(enabled) {
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

static void meshtasticd_update(void)
{
    int running = ui_meshtasticd_is_running();
    int bridge = meshtasticd_pid_running(MESHTASTICD_BRIDGE_PID);
    int busy;
    int paused;
    char action[96];
    char text[160];
    char ip[64];
    char log_text[MESHTASTICD_LOG_TAIL_BYTES + 1];
    meshtasticd_ble_status_t ble;

    pthread_mutex_lock(&meshtasticd_lock);
    busy = meshtasticd_busy;
    paused = meshtasticd_paused;
    snprintf(action, sizeof(action), "%s", meshtasticd_action_text);
    pthread_mutex_unlock(&meshtasticd_lock);

    if(meshtasticd_status_label) {
        uint32_t color = running ? 0x25C281 : (action[0] ? 0xF5A524 : 0x9AA4AF);
        const char *status = action[0] ? action :
                             running ? "Running" :
                             paused ? "Paused while the LoRa app has the radio" :
                             "Stopped";

        lv_label_set_text(meshtasticd_status_label, ui_tr(status));
        lv_obj_set_style_text_color(meshtasticd_status_label,
                                    lv_color_hex(color), 0);
    }

    meshtasticd_read_ble_status(&ble);
    if(meshtasticd_ble_label) {
        if(!running) {
            snprintf(text, sizeof(text), "%s", ui_tr("Bluetooth: off"));
        } else if(!bridge) {
            snprintf(text, sizeof(text), "%s",
                     ui_tr("Bluetooth: off (Settings > Bluetooth, Meshtastic mode)"));
        } else if(ble.phone_connected) {
            snprintf(text, sizeof(text), "%s", ui_tr("Bluetooth: phone connected"));
        } else if(ble.name[0]) {
            snprintf(text, sizeof(text), "%s %s", ui_tr("Bluetooth: advertising as"),
                     ble.name);
        } else {
            snprintf(text, sizeof(text), "%s", ui_tr("Bluetooth: starting..."));
        }
        lv_label_set_text(meshtasticd_ble_label, text);
    }
    if(meshtasticd_pin_label) {
        if(bridge && ble.pin[0]) {
            snprintf(text, sizeof(text), "PIN %s", ble.pin);
            lv_label_set_text(meshtasticd_pin_label, text);
            lv_obj_clear_flag(meshtasticd_pin_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(meshtasticd_pin_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    meshtasticd_read_version();
    if(meshtasticd_version_label) {
        snprintf(text, sizeof(text), "Meshtastic %s",
                 meshtasticd_version[0] ? meshtasticd_version : "firmware");
        lv_label_set_text(meshtasticd_version_label, text);
    }
    if(meshtasticd_hint_label) {
        char *slash;

        if(ui_read_iface_ip(NET_WIFI_IFACE, ip, sizeof(ip)) == 0) {
            slash = strchr(ip, '/');
            if(slash) {
                *slash = '\0';
            }
            snprintf(text, sizeof(text), "%s %s:4403",
                     ui_tr("Connect the Meshtastic app over Bluetooth, or over Wi-Fi to"),
                     ip);
        } else {
            snprintf(text, sizeof(text), "%s",
                     ui_tr("Connect the Meshtastic app over Bluetooth."));
        }
        lv_label_set_text(meshtasticd_hint_label, text);
    }
    if(meshtasticd_log_label) {
        meshtasticd_read_log_tail(log_text, sizeof(log_text));
        lv_label_set_text(meshtasticd_log_label, log_text);
    }
    meshtasticd_set_enabled(meshtasticd_start_btn, !running && !busy);
    meshtasticd_set_enabled(meshtasticd_stop_btn, running && !busy);
}

static void meshtasticd_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    meshtasticd_update();
}

void ui_meshtasticd_create(lv_obj_t *scr)
{
    lv_obj_t *body;
    lv_obj_t *panel;
    lv_obj_t *label;
    int x = ui_page_panel_x();
    int w = ui_page_panel_width();
    int body_y = ui_page_top_y(144);
    int landscape = ui_is_landscape();
    int button_w = landscape ? 180 : 232;
    int actions_y = landscape ? 196 : 226;
    int autostart_y = actions_y + 84;
    int hint_y = autostart_y + 58;
    int log_y = hint_y + 70;

    ui_create_header(scr, "Meshtastic");

    body = ui_page_body(scr, 144);
    panel = ui_panel(body, x, 18, w, ui_body_height(body_y) - 42);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x101418), 0);
    ui_make_scrollable(panel, 48);

    label = ui_label(panel, "meshtasticd", &lv_font_montserrat_24, 0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    meshtasticd_version_label = ui_label(panel, "Meshtastic firmware",
                                         &lv_font_montserrat_16, 0x9AA4AF);
    lv_obj_align(meshtasticd_version_label, LV_ALIGN_TOP_LEFT, 0, 34);

    meshtasticd_status_label = ui_label(panel, "--", &lv_font_montserrat_20,
                                        0x9AA4AF);
    lv_obj_set_width(meshtasticd_status_label, w - 32);
    lv_label_set_long_mode(meshtasticd_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(meshtasticd_status_label, LV_ALIGN_TOP_LEFT, 0, 66);

    meshtasticd_ble_label = ui_label(panel, "--", &lv_font_montserrat_16,
                                     0x3DA5FF);
    lv_obj_set_width(meshtasticd_ble_label, w - 32);
    lv_label_set_long_mode(meshtasticd_ble_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(meshtasticd_ble_label, LV_ALIGN_TOP_LEFT, 0, 100);

    /* Shown while a phone pairs; the nRF52840 asks for a passkey. */
    meshtasticd_pin_label = ui_label(panel, "", &lv_font_montserrat_48,
                                     0xF5A524);
    lv_obj_align(meshtasticd_pin_label, LV_ALIGN_TOP_LEFT, 0, 142);
    lv_obj_add_flag(meshtasticd_pin_label, LV_OBJ_FLAG_HIDDEN);

    meshtasticd_start_btn = ui_command_button(panel, 0, actions_y, button_w,
                                              "Start", 0x25C281);
    lv_obj_add_event_cb(meshtasticd_start_btn, meshtasticd_start_event_cb,
                        LV_EVENT_CLICKED, NULL);

    meshtasticd_stop_btn = ui_command_button(panel, button_w + 24, actions_y,
                                             button_w, "Stop", 0xEF4D5A);
    lv_obj_add_event_cb(meshtasticd_stop_btn, meshtasticd_stop_event_cb,
                        LV_EVENT_CLICKED, NULL);

    label = ui_label(panel, "Start at boot", &lv_font_montserrat_20,
                     0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, autostart_y + 8);

    meshtasticd_autostart_switch = lv_switch_create(panel);
    lv_obj_set_size(meshtasticd_autostart_switch, 72, 38);
    lv_obj_align(meshtasticd_autostart_switch, LV_ALIGN_TOP_RIGHT, 0,
                 autostart_y);
    if(ui_meshtasticd_autostart_enabled()) {
        lv_obj_add_state(meshtasticd_autostart_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(meshtasticd_autostart_switch,
                        meshtasticd_autostart_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    meshtasticd_hint_label = ui_label(panel, "", &lv_font_montserrat_16,
                                      0x9AA4AF);
    lv_obj_set_width(meshtasticd_hint_label, w - 32);
    lv_label_set_long_mode(meshtasticd_hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(meshtasticd_hint_label, LV_ALIGN_TOP_LEFT, 0, hint_y);

    label = ui_label(panel, "Log", &lv_font_montserrat_18, 0xF2F5F8);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, log_y);

    meshtasticd_log_label = ui_label(panel, "No log yet",
                                     &lv_font_montserrat_14, 0x9AA4AF);
    lv_obj_set_width(meshtasticd_log_label, w - 32);
    lv_label_set_long_mode(meshtasticd_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(meshtasticd_log_label, LV_ALIGN_TOP_LEFT, 0, log_y + 32);

    meshtasticd_timer = lv_timer_create(meshtasticd_timer_cb, 1000, NULL);
    meshtasticd_update();
}

void ui_meshtasticd_cleanup(void)
{
    if(meshtasticd_timer) {
        lv_timer_delete(meshtasticd_timer);
        meshtasticd_timer = NULL;
    }
    meshtasticd_status_label = NULL;
    meshtasticd_ble_label = NULL;
    meshtasticd_pin_label = NULL;
    meshtasticd_version_label = NULL;
    meshtasticd_hint_label = NULL;
    meshtasticd_log_label = NULL;
    meshtasticd_start_btn = NULL;
    meshtasticd_stop_btn = NULL;
    meshtasticd_autostart_switch = NULL;
}
