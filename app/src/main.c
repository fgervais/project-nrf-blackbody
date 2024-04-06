#include <app_event_manager.h>
#include <zephyr/drivers/gpio.h>
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


#define BUTTON_PRESS_EVENT		BIT(0)


static K_EVENT_DEFINE(button_events);


int main(void)
{
	const struct device *wdt = DEVICE_DT_GET(DT_NODELABEL(wdt0));
#if defined(CONFIG_APP_SUSPEND_CONSOLE)
	const struct device *cons = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
#endif
	const struct device *tmp117_devs[] = {
		DEVICE_DT_GET(DT_INST(0, ti_tmp116)),
		DEVICE_DT_GET(DT_INST(1, ti_tmp116)),
		DEVICE_DT_GET(DT_INST(2, ti_tmp116)),
		DEVICE_DT_GET(DT_INST(3, ti_tmp116)),
	};

	int ret;
	int i;
	uint32_t reset_cause;
	int main_wdt_chan_id = -1;
	uint32_t events;

	watchdog_init(wdt, &main_wdt_chan_id);

	LOG_INF("\n\n🚀 MAIN START (%s) 🚀\n", APP_VERSION_FULL);

	reset_cause = show_reset_cause();
	clear_reset_cause();
	
	if (app_event_manager_init()) {
		LOG_ERR("Event manager not initialized");
	} else {
		module_set_state(MODULE_STATE_READY);
	}

	for (i = 0; i<4; i++) {
		if (!device_is_ready(tmp117_devs[i])) {
			LOG_ERR("%s: device not ready", tmp117_devs[i]->name);
			return 1;
		}
		LOG_INF("Device %s - %p is ready",
			tmp117_devs[i]->name, tmp117_devs[i]);
	}

	LOG_INF("🎉 init done 🎉");

#if defined(CONFIG_APP_SUSPEND_CONSOLE)
	ret = pm_device_action_run(cons, PM_DEVICE_ACTION_SUSPEND);
	if (ret < 0) {
		LOG_ERR("Could not suspend the console");
		return ret;
	}
#endif

	thread_analyzer_print();

	while (1) {
		LOG_INF("💤 waiting for events");
		events = k_event_wait(&button_events,
				(BUTTON_PRESS_EVENT),
				true,
				K_SECONDS(CONFIG_APP_MAIN_LOOP_PERIOD_SEC));

		if (events & BUTTON_PRESS_EVENT) {
			LOG_INF("handling button press event");
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
			k_event_post(&button_events, BUTTON_PRESS_EVENT);
		}
	}

	return true;
}

APP_EVENT_LISTENER(MODULE, event_handler);
APP_EVENT_SUBSCRIBE(MODULE, button_event);
