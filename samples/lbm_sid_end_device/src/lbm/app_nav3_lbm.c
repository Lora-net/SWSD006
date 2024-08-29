#include <app.h>
#include <sidewalk.h>
#include <buttons.h>
#include <app_ble_config.h>
#include <app_subGHz_config.h>
#if defined(CONFIG_GPIO)
#include <state_notifier/notifier_gpio.h>
#endif
#if defined(CONFIG_LOG)
#include <state_notifier/notifier_log.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_lbm_nav3, CONFIG_SIDEWALK_LOG_LEVEL);

static uint32_t persistent_link_mask;

void app_event_lbm()
{
	sidewalk_event_send(SID_EVENT_LBM, NULL);
}

static void on_sidewalk_event(bool in_isr, void *context)
{
	/* not running sidewalk */
}

static void on_sidewalk_msg_received(const struct sid_msg_desc *msg_desc, const struct sid_msg *msg,
				     void *context)
{
	/* not running sidewalk */
}

static void on_sidewalk_msg_sent(const struct sid_msg_desc *msg_desc, void *context)
{
	/* not running sidewalk */
}

static void on_sidewalk_send_error(sid_error_t error, const struct sid_msg_desc *msg_desc,
				   void *context)
{
	/* not running sidewalk */
}

static void on_sidewalk_status_changed(const struct sid_status *status, void *context)
{
	/* not running sidewalk */
}

static void on_sidewalk_factory_reset(void *context)
{
	/* not running sidewalk */
}

static int lbm_buttons_init(void)
{
	return buttons_init();
}

void app_start(void)
{
	if (lbm_buttons_init()) {
		LOG_ERR("Cannot init buttons");
	}

#if defined(CONFIG_GPIO)
	state_watch_init_gpio(&global_state_notifier);
#endif
#if defined(CONFIG_LOG)
	state_watch_init_log(&global_state_notifier);
#endif
	application_state_working(&global_state_notifier, true);

	static sidewalk_ctx_t sid_ctx = { 0 };

	static struct sid_event_callbacks event_callbacks = {
		.context = &sid_ctx,
		.on_event = on_sidewalk_event,
		.on_msg_received = on_sidewalk_msg_received,
		.on_msg_sent = on_sidewalk_msg_sent,
		.on_send_error = on_sidewalk_send_error,
		.on_status_changed = on_sidewalk_status_changed,
		.on_factory_reset = on_sidewalk_factory_reset,
	};

	sid_ctx.config = (struct sid_config){
		.link_mask = persistent_link_mask,
		.callbacks = &event_callbacks,
		.link_config = app_get_ble_config(),
		.sub_ghz_link_config = app_get_sub_ghz_config(),
	};

	sidewalk_start(&sid_ctx);
}
