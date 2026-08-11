/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/battery.h>
#include <zmk/display.h>
#include "status.h"
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/wpm_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/usb.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/wpm.h>

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#endif

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

struct output_status_state {
    struct zmk_endpoint_instance selected_endpoint;
    int active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
};

struct layer_status_state {
    zmk_keymap_layer_index_t index;
    const char *label;
};

struct wpm_status_state {
    uint8_t wpm;
};

struct peripheral_battery_status_state {
    uint8_t level;
    bool available;
};

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
struct caps_status_state {
    bool caps_lock;
};
#endif

static void draw_separator(lv_obj_t *canvas, int y) {
    lv_draw_line_dsc_t line_dsc;
    init_line_dsc(&line_dsc, LVGL_FOREGROUND, 1);
    lv_point_t points[2] = {{2, y}, {65, y}};
    lv_canvas_draw_line(canvas, points, 2, &line_dsc);
}

/* First 68 px block: left and right batteries. */
static void draw_top(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 0);

    lv_draw_rect_dsc_t bg_dsc;
    init_rect_dsc(&bg_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t small_left;
    init_label_dsc(&small_left, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t small_right;
    init_label_dsc(&small_right, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_RIGHT);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &bg_dsc);

    /* Left / peripheral battery. */
    lv_canvas_draw_text(canvas, 1, 7, 8, &small_left, "L");
    if (state->peripheral_battery_available) {
        draw_battery_level(canvas, 10, 4, state->peripheral_battery, false);
        char left_text[6] = {};
        snprintf(left_text, sizeof(left_text), "%u%%", state->peripheral_battery);
        lv_canvas_draw_text(canvas, 37, 7, 30, &small_right, left_text);
    } else {
        draw_battery_level(canvas, 10, 4, 0, false);
        lv_canvas_draw_text(canvas, 37, 7, 30, &small_right, "--%");
    }

    /* Right / local central battery. */
    lv_canvas_draw_text(canvas, 1, 34, 8, &small_left, "R");
    draw_battery_level(canvas, 10, 31, state->local_battery, state->local_charging);
    char right_text[6] = {};
    snprintf(right_text, sizeof(right_text), "%u%%", state->local_battery);
    lv_canvas_draw_text(canvas, 37, 34, 30, &small_right, right_text);

    draw_separator(canvas, 61);
    rotate_canvas(canvas, cbuf);
}

/* Middle 68 px block: endpoint / Caps Lock and numeric WPM. */
static void draw_middle(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 1);

    lv_draw_rect_dsc_t bg_dsc;
    init_rect_dsc(&bg_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t icon_dsc;
    init_label_dsc(&icon_dsc, LVGL_FOREGROUND, &lv_font_montserrat_16, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t small_left;
    init_label_dsc(&small_left, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t small_right;
    init_label_dsc(&small_right, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_RIGHT);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &bg_dsc);

    char output_text[10] = {};
    switch (state->selected_endpoint.transport) {
    case ZMK_TRANSPORT_USB:
        strcat(output_text, LV_SYMBOL_USB);
        break;
    case ZMK_TRANSPORT_BLE:
        if (state->active_profile_bonded) {
            strcat(output_text,
                   state->active_profile_connected ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE);
        } else {
            strcat(output_text, LV_SYMBOL_SETTINGS);
        }
        break;
    }
    lv_canvas_draw_text(canvas, 2, 3, 22, &icon_dsc, output_text);

    if (state->caps_lock) {
        lv_canvas_draw_text(canvas, 34, 8, 32, &small_right, "CAPS");
    }

    draw_separator(canvas, 27);

    char wpm_text[12] = {};
    snprintf(wpm_text, sizeof(wpm_text), "WPM %u", state->wpm);
    lv_canvas_draw_text(canvas, 3, 40, 62, &small_left, wpm_text);

    draw_separator(canvas, 61);
    rotate_canvas(canvas, cbuf);
}

/* Final 68 px block: compact Bluetooth profile row and active layer. */
static void draw_bottom(lv_obj_t *widget, lv_color_t cbuf[], const struct status_state *state) {
    lv_obj_t *canvas = lv_obj_get_child(widget, 2);

    lv_draw_rect_dsc_t bg_dsc;
    init_rect_dsc(&bg_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t fg_dsc;
    init_rect_dsc(&fg_dsc, LVGL_FOREGROUND);
    lv_draw_label_dsc_t small_left;
    init_label_dsc(&small_left, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t small_center;
    init_label_dsc(&small_center, LVGL_FOREGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER);
    lv_draw_label_dsc_t small_center_inv;
    init_label_dsc(&small_center_inv, LVGL_BACKGROUND, &lv_font_unscii_8, LV_TEXT_ALIGN_CENTER);
    lv_draw_label_dsc_t layer_dsc;
    init_label_dsc(&layer_dsc, LVGL_FOREGROUND, &lv_font_montserrat_14, LV_TEXT_ALIGN_CENTER);

    lv_canvas_draw_rect(canvas, 0, 0, CANVAS_SIZE, CANVAS_SIZE, &bg_dsc);

    lv_canvas_draw_text(canvas, 1, 6, 18, &small_left, "BT");

    /* Five profiles on one line. The selected profile is inverted. */
    const int profile_x[5] = {22, 31, 40, 49, 58};
    for (int i = 0; i < 5; i++) {
        bool selected = i == state->active_profile_index;
        char profile[2] = {};
        snprintf(profile, sizeof(profile), "%d", i + 1);
        if (selected) {
            lv_canvas_draw_rect(canvas, profile_x[i], 3, 9, 13, &fg_dsc);
        }
        lv_canvas_draw_text(canvas, profile_x[i], 6, 9,
                            selected ? &small_center_inv : &small_center, profile);
    }

    draw_separator(canvas, 26);

    if (state->layer_label == NULL || strlen(state->layer_label) == 0) {
        char text[12] = {};
        snprintf(text, sizeof(text), "LAYER %u", state->layer_index);
        lv_canvas_draw_text(canvas, 0, 40, 68, &layer_dsc, text);
    } else {
        lv_canvas_draw_text(canvas, 0, 40, 68, &layer_dsc, state->layer_label);
    }

    rotate_canvas(canvas, cbuf);
}

/* Local battery (right / central). */
static void set_battery_status(struct zmk_widget_status *widget,
                               struct battery_status_state state) {
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    widget->state.local_charging = state.usb_present;
#endif
    widget->state.local_battery = state.level;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void battery_status_update_cb(struct battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_battery_status(widget, state); }
}

static struct battery_status_state battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    return (struct battery_status_state){
        .level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .usb_present = zmk_usb_is_powered(),
#endif
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_battery_status, struct battery_status_state,
                            battery_status_update_cb, battery_status_get_state)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_battery_status, zmk_usb_conn_state_changed);
#endif

/* Split peripheral battery (left). */
static void set_peripheral_battery_status(struct zmk_widget_status *widget,
                                          struct peripheral_battery_status_state state) {
    widget->state.peripheral_battery = state.level;
    widget->state.peripheral_battery_available = state.available;
    draw_top(widget->obj, widget->cbuf, &widget->state);
}

static void peripheral_battery_status_update_cb(struct peripheral_battery_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
        set_peripheral_battery_status(widget, state);
    }
}

static struct peripheral_battery_status_state
peripheral_battery_status_get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);
    return (struct peripheral_battery_status_state){
        .level = ev != NULL ? ev->state_of_charge : 0,
        .available = ev != NULL,
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_peripheral_battery_status,
                            struct peripheral_battery_status_state,
                            peripheral_battery_status_update_cb,
                            peripheral_battery_status_get_state)
ZMK_SUBSCRIPTION(widget_peripheral_battery_status, zmk_peripheral_battery_state_changed);

/* Host endpoint and selected Bluetooth profile. */
static void set_output_status(struct zmk_widget_status *widget,
                              const struct output_status_state *state) {
    widget->state.selected_endpoint = state->selected_endpoint;
    widget->state.active_profile_index = state->active_profile_index;
    widget->state.active_profile_connected = state->active_profile_connected;
    widget->state.active_profile_bonded = state->active_profile_bonded;

    draw_middle(widget->obj, widget->cbuf2, &widget->state);
    draw_bottom(widget->obj, widget->cbuf3, &widget->state);
}

static void output_status_update_cb(struct output_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_output_status(widget, &state); }
}

static struct output_status_state output_status_get_state(const zmk_event_t *_eh) {
    return (struct output_status_state){
        .selected_endpoint = zmk_endpoints_selected(),
        .active_profile_index = zmk_ble_active_profile_index(),
        .active_profile_connected = zmk_ble_active_profile_is_connected(),
        .active_profile_bonded = !zmk_ble_active_profile_is_open(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_output_status, struct output_status_state,
                            output_status_update_cb, output_status_get_state)
ZMK_SUBSCRIPTION(widget_output_status, zmk_endpoint_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(widget_output_status, zmk_usb_conn_state_changed);
#endif
#if defined(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(widget_output_status, zmk_ble_active_profile_changed);
#endif

/* Caps Lock state as reported back by the currently selected host. */
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
static void set_caps_status(struct zmk_widget_status *widget, struct caps_status_state state) {
    widget->state.caps_lock = state.caps_lock;
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
}

static void caps_status_update_cb(struct caps_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_caps_status(widget, state); }
}

static struct caps_status_state caps_status_get_state(const zmk_event_t *eh) {
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    zmk_hid_indicators_t indicators =
        ev != NULL ? ev->indicators : zmk_hid_indicators_get_current_profile();
    /* HID LED report: bit 0 = Num Lock, bit 1 = Caps Lock. */
    return (struct caps_status_state){.caps_lock = (indicators & BIT(1)) != 0};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_caps_status, struct caps_status_state, caps_status_update_cb,
                            caps_status_get_state)
ZMK_SUBSCRIPTION(widget_caps_status, zmk_hid_indicators_changed);
#endif

/* Active layer. */
static void set_layer_status(struct zmk_widget_status *widget, struct layer_status_state state) {
    widget->state.layer_index = state.index;
    widget->state.layer_label = state.label;
    draw_bottom(widget->obj, widget->cbuf3, &widget->state);
}

static void layer_status_update_cb(struct layer_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_layer_status(widget, state); }
}

static struct layer_status_state layer_status_get_state(const zmk_event_t *eh) {
    zmk_keymap_layer_index_t index = zmk_keymap_highest_layer_active();
    return (struct layer_status_state){
        .index = index, .label = zmk_keymap_layer_name(zmk_keymap_layer_index_to_id(index))};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_layer_status, struct layer_status_state, layer_status_update_cb,
                            layer_status_get_state)
ZMK_SUBSCRIPTION(widget_layer_status, zmk_layer_state_changed);

/* Current WPM only; the previous 10-sample graph is intentionally removed. */
static void set_wpm_status(struct zmk_widget_status *widget, struct wpm_status_state state) {
    widget->state.wpm = state.wpm;
    draw_middle(widget->obj, widget->cbuf2, &widget->state);
}

static void wpm_status_update_cb(struct wpm_status_state state) {
    struct zmk_widget_status *widget;
    SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) { set_wpm_status(widget, state); }
}

static struct wpm_status_state wpm_status_get_state(const zmk_event_t *eh) {
    return (struct wpm_status_state){.wpm = zmk_wpm_get_state()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_wpm_status, struct wpm_status_state, wpm_status_update_cb,
                            wpm_status_get_state)
ZMK_SUBSCRIPTION(widget_wpm_status, zmk_wpm_state_changed);

#ifdef CONFIG_NICE_VIEW_DISP_ROTATE_180
int top_pos = 0;
int middle_pos = 68;
int bottom_pos = 136;
#else
int top_pos = 92;
int middle_pos = 24;
int bottom_pos = -44;
#endif

int zmk_widget_status_init(struct zmk_widget_status *widget, lv_obj_t *parent) {
    widget->obj = lv_obj_create(parent);
    lv_obj_set_size(widget->obj, 160, 68);

    lv_obj_t *top = lv_canvas_create(widget->obj);
    lv_obj_align(top, LV_ALIGN_TOP_LEFT, top_pos, 0);
    lv_canvas_set_buffer(top, widget->cbuf, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *middle = lv_canvas_create(widget->obj);
    lv_obj_align(middle, LV_ALIGN_TOP_LEFT, middle_pos, 0);
    lv_canvas_set_buffer(middle, widget->cbuf2, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    lv_obj_t *bottom = lv_canvas_create(widget->obj);
    lv_obj_align(bottom, LV_ALIGN_TOP_LEFT, bottom_pos, 0);
    lv_canvas_set_buffer(bottom, widget->cbuf3, CANVAS_SIZE, CANVAS_SIZE, LV_IMG_CF_TRUE_COLOR);

    sys_slist_append(&widgets, &widget->node);
    widget_battery_status_init();
    widget_peripheral_battery_status_init();
    widget_output_status_init();
#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    widget_caps_status_init();
#endif
    widget_layer_status_init();
    widget_wpm_status_init();

    return 0;
}

lv_obj_t *zmk_widget_status_obj(struct zmk_widget_status *widget) { return widget->obj; }
