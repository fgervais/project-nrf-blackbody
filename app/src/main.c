#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/debug/thread_analyzer.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#include <app_version.h>

#include "reset.h"
#include "watchdog.h"


#define BUTTON0_PRESS_EVENT		BIT(0)
#define BUTTON1_PRESS_EVENT		BIT(1)
#define BUTTON2_PRESS_EVENT		BIT(2)
#define BUTTON3_PRESS_EVENT		BIT(3)
#define BUTTON_ALL_PRESS_EVENT		(BUTTON0_PRESS_EVENT | \
					 BUTTON1_PRESS_EVENT | \
					 BUTTON2_PRESS_EVENT | \
					 BUTTON3_PRESS_EVENT)


#define TMP117_CFGR_DR_ALERT		BIT(2)


static K_EVENT_DEFINE(button_events);


int general_call_reset(const struct device *i2c_dev) {
	uint8_t command = 0x06;
	uint32_t num_bytes = 0x01;
	uint16_t addr = 0x00;

	return i2c_write(i2c_dev, &command, num_bytes, addr);
}

int get_current_temperature(const struct device *const dev, double *temperature)
{
	int ret;
	struct sensor_value temp_value;

	ret = sensor_sample_fetch(dev);
	if (ret < 0) {
		LOG_ERR("Failed to fetch measurements (%d)", ret);
		return ret;
	}

	ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP,
				 &temp_value);
	if (ret < 0) {
		LOG_ERR("Failed to get measurements (%d)", ret);
		return ret;
	}

	*temperature = sensor_value_to_double(&temp_value);

	return 0;
}

int set_alert_pin_as_data_ready(const struct device *const dev) {
	struct sensor_value config = { 0 };
	int ret;

	ret = sensor_attr_get(dev,
			      SENSOR_CHAN_AMBIENT_TEMP,
			      SENSOR_ATTR_CONFIGURATION,
			      &config);
	if (ret < 0) {
		LOG_ERR("Could not get configuration");
		return ret;
	}

	if (config.val1 & TMP117_CFGR_DR_ALERT) {
		return 0;
	}

	config.val1 |= TMP117_CFGR_DR_ALERT;

	ret = sensor_attr_set(dev,
			      SENSOR_CHAN_AMBIENT_TEMP,
			      SENSOR_ATTR_CONFIGURATION,
			      &config);
	if (ret < 0) {
		LOG_ERR("Could not set configuration");
		return ret;
	}

	return 0;
}

int main(void)
{
	const struct device *wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
#if defined(CONFIG_APP_SUSPEND_CONSOLE)
	const struct device *cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
#endif
	const struct device *i2c0 = DEVICE_DT_GET(DT_NODELABEL(i2c0));
	const struct device *tmp117_devs[] = {
		DEVICE_DT_GET(DT_CHILD(DT_NODELABEL(i2c0), tmp117_48)),
		DEVICE_DT_GET(DT_CHILD(DT_NODELABEL(i2c0), tmp117_49)),
		DEVICE_DT_GET(DT_CHILD(DT_NODELABEL(i2c0), tmp117_4a)),
		DEVICE_DT_GET(DT_CHILD(DT_NODELABEL(i2c0), tmp117_4b)),
	};

	int ret;
	int i;
	uint32_t reset_cause;
	int main_wdt_chan_id = -1;
	uint32_t events;
	double temperatures[ARRAY_SIZE(tmp117_devs)];

	watchdog_init(wdt, &main_wdt_chan_id);

	LOG_INF("\n\n🚀 MAIN START (%s) 🚀\n", APP_VERSION_FULL);

	reset_cause = show_reset_cause();
	clear_reset_cause();

	for (i = 0; i < ARRAY_SIZE(tmp117_devs); i++) {
		if (!device_is_ready(tmp117_devs[i])) {
			LOG_ERR("%s: device not ready", tmp117_devs[i]->name);
			return 1;
		}
		LOG_INF("Device %s - %p is ready",
			tmp117_devs[i]->name, tmp117_devs[i]);
	}

	LOG_INF("Setting ALERT pin as DATA READY");
	for (i = 0; i < ARRAY_SIZE(tmp117_devs); i++) {
		ret = set_alert_pin_as_data_ready(tmp117_devs[i]);
		if (ret < 0) {
			LOG_ERR("Could not configure tmp117 (#%d)", i);
			return 1;
		}
	}

	LOG_INF("🎬 reset all tmp117s");
	ret = general_call_reset(i2c0);
	if (ret < 0) {
		LOG_ERR("Could not send a general call reset");
		return ret;
	}

	LOG_INF("🆗 initialized");

#if defined(CONFIG_APP_SUSPEND_CONSOLE)
	ret = pm_device_action_run(cons, PM_DEVICE_ACTION_SUSPEND);
	if (ret < 0) {
		LOG_ERR("Could not suspend the console");
		return ret;
	}
#endif

	thread_analyzer_print();

	LOG_INF("┌──────────────────────────────────────────────────────────┐");
	LOG_INF("│ Entering main loop                                       │");
	LOG_INF("└──────────────────────────────────────────────────────────┘");

	while (1) {
		LOG_INF("💤 waiting for events");
		events = k_event_wait_all(&button_events,
				(BUTTON_ALL_PRESS_EVENT),
				true,
				K_SECONDS(CONFIG_APP_MAIN_LOOP_PERIOD_SEC));

		LOG_INF("⏰ events: %08x", events);

		if (events == BUTTON_ALL_PRESS_EVENT) {
			LOG_INF("handling button press event");
			for (i = 0; i < ARRAY_SIZE(tmp117_devs); i++) {
				ret = get_current_temperature(tmp117_devs[i],
							      &temperatures[i]);
				if (ret < 0) {
					LOG_ERR("Could not get temperature");
					return ret;
				}

				LOG_INF("🌡️  current temp: %g°C", temperatures[i]);
			}
		}

		LOG_INF("🦴 feed watchdog");
		wdt_feed(wdt, main_wdt_chan_id);
	}

	return 0;
}
