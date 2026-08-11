/*
 *
 * Copyright (c) 2023 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 */

#include <zephyr/kernel.h>
#include "util.h"

LV_IMG_DECLARE(bolt);

void rotate_canvas(lv_obj_t *canvas, lv_color_t cbuf[]) {
    static lv_color_t cbuf_tmp[CANVAS_SIZE * CANVAS_SIZE];
    memcpy(cbuf_tmp, cbuf, sizeof(cbuf_tmp));
    lv_img_dsc_t img;
    img.data = (void *)cbuf_tmp;
    img.header.cf = LV_IMG_CF_TRUE_COLOR;
    img.header.w = CANVAS_SIZE;
    img.header.h = CANVAS_SIZE;

    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);
#ifdef CONFIG_NICE_VIEW_DISP_ROTATE_180
    lv_canvas_transform(canvas, &img, -900, LV_IMG_ZOOM_NONE, -1, 0, CANVAS_SIZE / 2,
                        CANVAS_SIZE / 2 - 1, true);
#else
    lv_canvas_transform(canvas, &img, 900, LV_IMG_ZOOM_NONE, -1, 0, CANVAS_SIZE / 2,
                        CANVAS_SIZE / 2, true);
#endif
}

/* Compact 24x12 battery icon used by both split halves. */
void draw_battery_level(lv_obj_t *canvas, int x, int y, uint8_t level, bool charging) {
    lv_draw_rect_dsc_t bg_dsc;
    init_rect_dsc(&bg_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t fg_dsc;
    init_rect_dsc(&fg_dsc, LVGL_FOREGROUND);

    uint8_t clamped = level > 100 ? 100 : level;
    uint8_t fill_width = (clamped * 18 + 99) / 100;

    /* Body, inner cutout, charge fill and positive terminal. */
    lv_canvas_draw_rect(canvas, x, y, 22, 12, &fg_dsc);
    lv_canvas_draw_rect(canvas, x + 1, y + 1, 20, 10, &bg_dsc);
    if (fill_width > 0) {
        lv_canvas_draw_rect(canvas, x + 2, y + 2, fill_width, 8, &fg_dsc);
    }
    lv_canvas_draw_rect(canvas, x + 22, y + 3, 2, 6, &fg_dsc);

    if (charging) {
        lv_draw_img_dsc_t img_dsc;
        lv_draw_img_dsc_init(&img_dsc);
        /* Re-use the stock nice!view lightning bolt asset. */
        lv_canvas_draw_img(canvas, x + 6, y - 1, &bolt, &img_dsc);
    }
}

/* Keep the original helper for the peripheral status screen. */
void draw_battery(lv_obj_t *canvas, const struct status_state *state) {
    lv_draw_rect_dsc_t bg_dsc;
    init_rect_dsc(&bg_dsc, LVGL_BACKGROUND);
    lv_draw_rect_dsc_t fg_dsc;
    init_rect_dsc(&fg_dsc, LVGL_FOREGROUND);

    lv_canvas_draw_rect(canvas, 0, 2, 29, 12, &fg_dsc);
    lv_canvas_draw_rect(canvas, 1, 3, 27, 10, &bg_dsc);
    lv_canvas_draw_rect(canvas, 2, 4, (state->battery + 2) / 4, 8, &fg_dsc);
    lv_canvas_draw_rect(canvas, 30, 5, 3, 6, &fg_dsc);
    lv_canvas_draw_rect(canvas, 31, 6, 1, 4, &bg_dsc);

    if (state->charging) {
        lv_draw_img_dsc_t img_dsc;
        lv_draw_img_dsc_init(&img_dsc);
        lv_canvas_draw_img(canvas, 9, -1, &bolt, &img_dsc);
    }
}

void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align) {
    lv_draw_label_dsc_init(label_dsc);
    label_dsc->color = color;
    label_dsc->font = font;
    label_dsc->align = align;
}

void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color) {
    lv_draw_rect_dsc_init(rect_dsc);
    rect_dsc->bg_color = bg_color;
}

void init_line_dsc(lv_draw_line_dsc_t *line_dsc, lv_color_t color, uint8_t width) {
    lv_draw_line_dsc_init(line_dsc);
    line_dsc->color = color;
    line_dsc->width = width;
}

void init_arc_dsc(lv_draw_arc_dsc_t *arc_dsc, lv_color_t color, uint8_t width) {
    lv_draw_arc_dsc_init(arc_dsc);
    arc_dsc->color = color;
    arc_dsc->width = width;
}
