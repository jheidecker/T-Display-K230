#ifndef K230_PHONE_UI_MESHTASTICD_H
#define K230_PHONE_UI_MESHTASTICD_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Official meshtasticd plus the nRF52840 BLE bridge, run by
 * /etc/init.d/S90meshtasticd. When it's installed, ui_meshtastic.c hands its
 * public entry points to these, and the Meshtastic app becomes a start/stop
 * page for the service. */
int ui_meshtasticd_installed(void);
void ui_meshtasticd_create(lv_obj_t *scr);
void ui_meshtasticd_cleanup(void);
int ui_meshtasticd_is_running(void);
int ui_meshtasticd_autostart_enabled(void);
void ui_meshtasticd_set_autostart_enabled(int enabled);
void ui_meshtasticd_startup(void);
void ui_meshtasticd_pause_for_radio_owner(const char *owner);
void ui_meshtasticd_resume_after_radio_owner(void);
void ui_meshtasticd_apply_ble_setting(void);

#ifdef __cplusplus
}
#endif

#endif
