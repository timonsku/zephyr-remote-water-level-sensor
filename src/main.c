/*
 * copyright (c) 2023 golioth, inc.
 *
 * spdx-license-identifier: apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(water_level_sensor, LOG_LEVEL_DBG);

#include "golioth.h"
#include <samples/common/sample_credentials.h>
#include <string.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <lvgl.h>

#include <samples/common/net_connect.h>

#if !DT_NODE_EXISTS(DT_PATH(zephyr_user)) || \
	!DT_NODE_HAS_PROP(DT_PATH(zephyr_user), io_channels)
#error "No suitable devicetree overlay specified"
#endif

#define DT_SPEC_AND_COMMA(node_id, prop, idx) \
	ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels,
			     DT_SPEC_AND_COMMA)
};

static const struct gpio_dt_spec backlight = GPIO_DT_SPEC_GET(DT_NODELABEL(backlight), gpios);

golioth_client_t client;
static K_SEM_DEFINE(connected, 0, 1);

static void on_client_event(golioth_client_t client, golioth_client_event_t event, void* arg) {
    bool is_connected = (event == GOLIOTH_CLIENT_EVENT_CONNECTED);
    if (is_connected) {
        k_sem_give(&connected);
    }
    LOG_INF("Golioth client %s", is_connected ? "connected" : "disconnected");
}

uint64_t get_water_level_um(int32_t adc_count, uint32_t nv_per_sample, uint32_t milliohms, uint32_t maxDepthUM) {
    // 4-20mA sensor
    int64_t sample_nv = adc_count*nv_per_sample;
    int64_t maxLevel = (16*milliohms)*1000;
    int64_t offset_nv = sample_nv-((4*milliohms)*1000);
    if(offset_nv <= 0) {
        return 0;
    }
    uint64_t waterlevel = (offset_nv*maxDepthUM)/maxLevel;
    return waterlevel;
};

uint32_t get_water_level_mm(int32_t millivolts, uint32_t milliohms, uint32_t maxDepthMM) {
    // 4-20mA sensor
    int32_t maxLevel = (16*milliohms)/1000;
    int32_t offset_mv = millivolts-((4*milliohms)/1000);
    if(offset_mv <= 0) {
        return 0;
    }
    int32_t waterlevel = (offset_mv*maxDepthMM)/maxLevel;
    return (uint32_t)(waterlevel);
}

int main(void) {
    int err;
    uint32_t buf;

    struct adc_sequence sequence = {
        .buffer = &buf,
        /* buffer size in bytes, not number of samples */
        .buffer_size = sizeof(buf),
    };

    if (!adc_is_ready_dt(&adc_channels[0])) {
        LOG_ERR("ADC controller device %s not ready\n", adc_channels[0].dev->name);
        return 0;
    }

    err = adc_channel_setup_dt(&adc_channels[0]);
    if (err < 0) {
        LOG_ERR("Could not setup channel #0 (%d)\n", err);
        return 0;
    }

    (void)adc_sequence_init_dt(&adc_channels[0], &sequence);

    char water_level_str[32] = {0};
	const struct device *display_dev;
	lv_obj_t *mid_label;
	lv_obj_t *bottom_label;
    static lv_style_t level_style;
    lv_style_init(&level_style);
    // lv_style_set_text_color(&level_style, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_text_font(&level_style, &lv_font_montserrat_48);

	int ret;
	gpio_is_ready_dt(&backlight);
	gpio_pin_configure_dt(&backlight, GPIO_OUTPUT_ACTIVE);
	gpio_pin_set_dt(&backlight, 1);

	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display not ready");
		return 0;
	}

    mid_label = lv_label_create(lv_scr_act());
    lv_obj_align(mid_label, LV_ALIGN_CENTER, 0, -20);

    bottom_label = lv_label_create(lv_scr_act());
    lv_obj_align(bottom_label, LV_ALIGN_BOTTOM_MID, 0, -50);

    lv_label_set_text(mid_label, "Connecting...");
    lv_task_handler();
    lv_obj_add_style(mid_label, &level_style, 0);
    display_blanking_off(display_dev);

    net_connect();

    const golioth_client_config_t* client_config = golioth_sample_credentials_get();

    client = golioth_client_create(client_config);

    golioth_client_register_event_callback(client, on_client_event, NULL);

    k_sem_take(&connected, K_FOREVER);

    while (true) {

        int32_t val_mv;
        int32_t adc_count;
        uint64_t water_level = 0;
        bool is_connected = true;
        err = adc_read(adc_channels[0].dev, &sequence);
        if (err < 0) {
            LOG_ERR("Failed to read ADC: %d", err);
            continue;
        }
        
        adc_count = (int32_t)buf;
        val_mv = adc_count;
        // LOG_INF("adc: %d", adc_count);

        err = adc_raw_to_millivolts_dt(&adc_channels[0], &val_mv);
        // LOG_INF("adc: %d", val_mv);

        if (val_mv < 300){
            LOG_ERR("Sensor disconnected");
            lv_label_set_text(bottom_label, "     Sensor\nDisconnected");
            lv_label_set_text(mid_label, "?");
            is_connected = false;
        }else{
            is_connected = true;
            lv_label_set_text(bottom_label, "   Sensor\nConnected");
            water_level = get_water_level_um(adc_count, 15625, 99640, 1000*1000);

            err = golioth_lightdb_stream_set_float_sync(client, "water-level", (float)water_level/1000, 2);
            if (err) {
                LOG_WRN("Failed to transmit water-level stream: %d", err);
            }

            LOG_INF("water level: %4.2f", (float)water_level/1000);

            err = golioth_lightdb_set_int_sync(client, "water-level", water_level/1000, 2);
            if (err) {
                LOG_WRN("Failed to transmit water-level: %d", err);
            }

            lv_label_set_text_fmt(mid_label, "%llu", water_level/1000U);
        }

        err = golioth_lightdb_set_bool_sync(client, "sensor-connected", is_connected, 2);
        if (err) {
            LOG_WRN("Failed to transmit sensor-connected: %d", err);
        }
    
        lv_task_handler();
        
        // k_sleep(K_SECONDS(1));
    }

    return 0;
}
