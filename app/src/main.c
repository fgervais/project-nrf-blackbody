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


#define TMP117_48_NODE			DT_CHILD(DT_NODELABEL(i2c0), tmp117_48)
#define TMP117_49_NODE			DT_CHILD(DT_NODELABEL(i2c0), tmp117_49)
#define TMP117_4a_NODE			DT_CHILD(DT_NODELABEL(i2c0), tmp117_4a)
#define TMP117_4b_NODE			DT_CHILD(DT_NODELABEL(i2c0), tmp117_4b)

#define ALERT0_PRESS_EVENT		BIT(0)
#define ALERT1_PRESS_EVENT		BIT(1)
#define ALERT2_PRESS_EVENT		BIT(2)
#define ALERT3_PRESS_EVENT		BIT(3)
#define BUTTON_ALL_PRESS_EVENT		(ALERT0_PRESS_EVENT | \
					 ALERT1_PRESS_EVENT | \
					 ALERT2_PRESS_EVENT | \
					 ALERT3_PRESS_EVENT)


#define TMP117_CFGR_DR_ALERT		BIT(2)


struct tmp117 {
	const struct device *dev;
	const struct gpio_dt_spec alert;
	struct gpio_callback callback;
	uint32_t event;
};


static K_EVENT_DEFINE(alert_events);

static struct tmp117 tmp117s[] = {
	{
		.dev = DEVICE_DT_GET(TMP117_48_NODE),
		.alert = GPIO_DT_SPEC_GET(
			DT_CHILD(TMP117_48_NODE, alert), gpios),
		.event = ALERT0_PRESS_EVENT,
	},
	{
		.dev = DEVICE_DT_GET(TMP117_49_NODE),
		.alert = GPIO_DT_SPEC_GET(
			DT_CHILD(TMP117_49_NODE, alert), gpios),
		.event = ALERT1_PRESS_EVENT,
	},
	{
		.dev = DEVICE_DT_GET(TMP117_4a_NODE),
		.alert = GPIO_DT_SPEC_GET(
			DT_CHILD(TMP117_4a_NODE, alert), gpios),
		.event = ALERT2_PRESS_EVENT,
	},
	{
		.dev = DEVICE_DT_GET(TMP117_4b_NODE),
		.alert = GPIO_DT_SPEC_GET(
			DT_CHILD(TMP117_4b_NODE, alert), gpios),
		.event = ALERT3_PRESS_EVENT,
	},
};


static void alert_callback(const struct device *port,
			   struct gpio_callback *cb,
			   gpio_port_pins_t pins)
{
	LOG_INF("🛎️  Button pressed");
	// CONTAINER_OF() if original struct gpio_callback
	// k_event_post(&alert_events, BIT(KEY_ROW(evt->key_id)));
}

static int general_call_reset(const struct device *i2c_dev) {
	uint8_t command = 0x06;
	uint32_t num_bytes = 0x01;
	uint16_t addr = 0x00;

	return i2c_write(i2c_dev, &command, num_bytes, addr);
}

static int get_current_temperature(struct tmp117 *sensor, double *temperature)
{
	int ret;
	struct sensor_value temp_value;

	ret = sensor_sample_fetch(sensor->dev);
	if (ret < 0) {
		LOG_ERR("Failed to fetch measurements (%d)", ret);
		return ret;
	}

	ret = sensor_channel_get(sensor->dev, SENSOR_CHAN_AMBIENT_TEMP,
				 &temp_value);
	if (ret < 0) {
		LOG_ERR("Failed to get measurements (%d)", ret);
		return ret;
	}

	*temperature = sensor_value_to_double(&temp_value);

	return 0;
}

static int set_alert_pin_as_data_ready(struct tmp117 *sensor) {
	struct sensor_value config = { 0 };
	int ret;

	ret = sensor_attr_get(sensor->dev,
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

	ret = sensor_attr_set(sensor->dev,
			      SENSOR_CHAN_AMBIENT_TEMP,
			      SENSOR_ATTR_CONFIGURATION,
			      &config);
	if (ret < 0) {
		LOG_ERR("Could not set configuration");
		return ret;
	}

	return 0;
}

static int configure_alert_pin(struct tmp117 *sensor) {
	int ret;

	ret = gpio_pin_configure_dt(&sensor->alert, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Could not configure gpio");
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&sensor->alert,
					      GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("Could not configure interrupt");
		return ret;
	}

	gpio_init_callback(&sensor->callback, alert_callback,
			   BIT(sensor->alert.pin));
	ret = gpio_add_callback_dt(&sensor->alert, &sensor->callback);
	if (ret < 0) {
		LOG_ERR("Could not add callback");
		return ret;
	}

	return 0;
}


static int configure_temperature_sensor(struct tmp117 *sensor) {
	int ret;

	if (!device_is_ready(sensor->dev)) {
		LOG_ERR("%s: device not ready", sensor->dev->name);
		return -ENODEV;
	}
	LOG_INF("Device %s - %p is ready",
		sensor->dev->name, sensor->dev);

	ret = configure_alert_pin(sensor);
	if (ret < 0) {
		LOG_ERR("Could not configure alert pin");
		return ret;
	}

	LOG_INF("Setting ALERT pin as DATA READY");
	ret = set_alert_pin_as_data_ready(sensor);
	if (ret < 0) {
		LOG_ERR("Could not configure %s", sensor->dev->name);
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

	int ret;
	int i;
	uint32_t reset_cause;
	int main_wdt_chan_id = -1;
	uint32_t events;
	double temperatures[ARRAY_SIZE(tmp117s)];

	watchdog_init(wdt, &main_wdt_chan_id);

	LOG_INF("\n\n🚀 MAIN START (%s) 🚀\n", APP_VERSION_FULL);

	reset_cause = show_reset_cause();
	clear_reset_cause();

	for (i = 0; i < ARRAY_SIZE(tmp117s); i++) {
		LOG_INF("🦄 configuring tmp117 #%d", i);
		ret = configure_temperature_sensor(&tmp117s[i]);
		if (ret < 0) {
			LOG_ERR("Could not configure tmp117");
			return ret;
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
		events = k_event_wait_all(&alert_events,
				(BUTTON_ALL_PRESS_EVENT),
				true,
				K_SECONDS(CONFIG_APP_MAIN_LOOP_PERIOD_SEC));

		LOG_INF("⏰ events: %08x", events);

		if (events == BUTTON_ALL_PRESS_EVENT) {
			LOG_INF("handling button press event");
			for (i = 0; i < ARRAY_SIZE(tmp117s); i++) {
				ret = get_current_temperature(&tmp117s[i],
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
