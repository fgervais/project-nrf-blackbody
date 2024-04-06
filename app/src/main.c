#include <app_event_manager.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/debug/thread_analyzer.h>

#define MODULE main
#include <caf/events/module_state_event.h>
#include <caf/events/button_event.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#include <app_version.h>

#include "reset.h"
#include "watchdog.h"


#define BUTTON0_PRESS_EVENT		BIT(0)
#define BUTTON1_PRESS_EVENT		BIT(1)
#define BUTTON2_PRESS_EVENT		BIT(2)
#define BUTTON3_PRESS_EVENT		BIT(3)


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
	
	if (app_event_manager_init()) {
		LOG_ERR("Event manager not initialized");
	} else {
		module_set_state(MODULE_STATE_READY);
	}

	for (i = 0; i < ARRAY_SIZE(tmp117_devs); i++) {
		if (!device_is_ready(tmp117_devs[i])) {
			LOG_ERR("%s: device not ready", tmp117_devs[i]->name);
			return 1;
		}
		LOG_INF("Device %s - %p is ready",
			tmp117_devs[i]->name, tmp117_devs[i]);
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
				(BUTTON0_PRESS_EVENT),
				true,
				K_SECONDS(CONFIG_APP_MAIN_LOOP_PERIOD_SEC));

		LOG_INF("⏰ events: %08x", events);

		if (events == BUTTON0_PRESS_EVENT) {
			LOG_INF("handling button press event");
			for (i = 0; i < ARRAY_SIZE(tmp117_devs); i++) {
				ret = get_current_temperature(tmp117_devs[i],
							      &temperatures[i]);
				if (ret < 0) {
					LOG_ERR("Could not get temperature");
					return ret;
				}
			}
		}

		LOG_INF("🦴 feed watchdog");
		wdt_feed(wdt, main_wdt_chan_id);
	}

	return 0;
}

static bool event_handler(const struct app_event_header *eh)
{
	const struct button_event *evt;

	if (is_button_event(eh)) {
		evt = cast_button_event(eh);

		if (evt->pressed) {
			LOG_INF("🛎️  Button pressed");
			k_event_post(&button_events, BUTTON0_PRESS_EVENT);
		}
	}

	return true;
}

APP_EVENT_LISTENER(MODULE, event_handler);
APP_EVENT_SUBSCRIBE(MODULE, button_event);
