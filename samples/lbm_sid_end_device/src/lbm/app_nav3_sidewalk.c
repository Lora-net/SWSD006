#include <app.h>
#include <sidewalk.h>
#include <app_ble_config.h>
#include <app_subGHz_config.h>
#include <sid_hal_reset_ifc.h>
#include <sid_hal_memory_ifc.h>
#if defined(CONFIG_GPIO)
#include <state_notifier/notifier_gpio.h>
#endif
#if defined(CONFIG_LOG)
#include <state_notifier/notifier_log.h>
#endif

#include <buttons.h>
#include <json_printer/sidTypes2Json.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app, CONFIG_SIDEWALK_LOG_LEVEL);

static uint32_t persistent_link_mask;
sid_pal_timer_t lr_timer;
sidewalk_event_t lr_timer_cb_arg;

static sidewalk_ctx_t sid_ctx = { 0 };

#ifdef NAV3_SEND
#include <math.h>
#define FRAGMENT_TYPE_GNSS         0	// deprecated -- to cloud port 198
#define FRAGMENT_TYPE_WIFI         1	// deprecated -- to cloud port 197
#define FRAGMENT_TYPE_MODEM_MSG    2	// deprecated -- to cloud port 199
#define FRAGMENT_TYPE_GNSS_NG      3	// deprecated -- to cloud port 192
typedef struct {
    uint32_t link_status_mask;
    uint16_t fcnt;
    int8_t rssi;
    int8_t snr;
} downlink_t;

struct sb_s {
	uint8_t buf[96];
	uint8_t length;
};
uint16_t scan_counter;
struct sb_s sendBuffer;  // cant be on the stack, this is sent later in fragments
downlink_t downlink;
static sid_pal_timer_t retry_timer;

static void retry_timer_cb(void *arg, sid_pal_timer_t *owner)
{
	sidewalk_ctx_t *sid_ctx = (sidewalk_ctx_t *)arg;
	/* resend fragment */
	sid_ctx->scan_result.send_attempt++;

	int err = sidewalk_event_send(SID_EVENT_NAV3_SEND, NULL);
	if (err) {
		LOG_ERR("send nav3-event err %d", err);
	}
}

const char *send_to_string(sending_t s)
{
	switch (s) {
		case SENDING_NONE: return "NONE";
		case SENDING_GNSS: return "GNSS";
		case SENDING_WIFI: return "WIFI";
		default: return NULL;
	}
}

int uplink_scan(uint8_t tlv_tag, const uint8_t *scan_result_buf, uint8_t scan_result_length)
{
	static uint16_t AppUp_counter;
	scan_counter++;
	if (sid_ctx.scan_result.total_fragments > 0) {
		LOG_WRN("already sending scan-result %s", send_to_string(sid_ctx.scan_result.sending));
		if (tlv_tag == TLV_TAG_GNSS_SINGLE_SCAN || tlv_tag == TLV_TAG_GNSS_NG) {
			if (sid_ctx.scan_result.pending_send == SENDING_WIFI) {
				LOG_ERR("already pending WIFI");
				return -1;
			}
			sid_ctx.scan_result.pending_send = SENDING_GNSS;
		} else if (tlv_tag == TLV_TAG_WIFI_SCAN) {
			if (sid_ctx.scan_result.pending_send == SENDING_GNSS) {
				LOG_ERR("already pending GNSS");
				return -1;
			}
			sid_ctx.scan_result.pending_send = SENDING_WIFI;
		} else {
			sid_ctx.scan_result.pending_send = SENDING_NONE;
			LOG_ERR("unknown tlv_tag %x", tlv_tag);
			return -1;
		}
		return 0;
	}

	if (tlv_tag == TLV_TAG_GNSS_SINGLE_SCAN || tlv_tag == TLV_TAG_GNSS_NG)
		sid_ctx.scan_result.sending = SENDING_GNSS;
	else if (tlv_tag == TLV_TAG_WIFI_SCAN)
		sid_ctx.scan_result.sending = SENDING_WIFI;
	else {
		sid_ctx.scan_result.sending = SENDING_NONE;
		LOG_ERR("unknown tlv_tag %x", tlv_tag);
	}
	//LOG_HEXDUMP_INF(scan_result_buf, scan_result_length, "scan_result_buf");
	unsigned header_length = 5; // fragment control byte, TLV tag, TLV length, 16bit scan counter
	if ((scan_result_length + header_length) > sizeof(sendBuffer.buf)) {
		LOG_WRN("send buffer too small, need %u", scan_result_length + header_length);
		return -1;
	}
	uint8_t *dest_ptr = sendBuffer.buf;
	*dest_ptr++ = tlv_tag;
	*dest_ptr++ = scan_result_length + 2;	// TLV length --> +2 for scan counter
	*dest_ptr++ = scan_counter >> 8;		// start of TLV value
	*dest_ptr++ = scan_counter & 0xff;
	memcpy(dest_ptr, scan_result_buf, scan_result_length);
	dest_ptr += scan_result_length;
	LOG_INF("sending scan at %u, link mask %x rssi %d snr %d", dest_ptr - sendBuffer.buf, downlink.link_status_mask, downlink.rssi, downlink.snr);
	{ // mirror uplink:
		*dest_ptr++ = TLV_TAG_MIRROR_UPLINK;	// TLV tag
		*dest_ptr++ = 7;	// TLV length
		*dest_ptr++ = downlink.fcnt >> 8;	// start of TLV value
		*dest_ptr++ = downlink.fcnt & 0xff;	// 
		*dest_ptr++ = AppUp_counter >> 8;	// 
		*dest_ptr++ = AppUp_counter & 0xff;	// 
		*dest_ptr++ = downlink.rssi + 64;	//
		*dest_ptr++ = downlink.snr;	//
		if (downlink.link_status_mask & SID_LINK_TYPE_2)
			*dest_ptr++ = 1;	// 0=CSS, 1=FSK
		else if (downlink.link_status_mask & SID_LINK_TYPE_3)
			*dest_ptr++ = 0;	// 0=CSS, 1=FSK
		else if (downlink.link_status_mask & SID_LINK_TYPE_1)
			*dest_ptr++ = 2;	// 2=BLE
		else {
			LOG_ERR("downlink.link_status_mask %x", downlink.link_status_mask);
			*dest_ptr++ = 0xff;	// 2=BLE
		}
		AppUp_counter++;
	}
	//LOG_HEXDUMP_INF(send_buffer, 8, "SB_F8");

	sendBuffer.length = dest_ptr - sendBuffer.buf;
	LOG_HEXDUMP_INF(sendBuffer.buf, sendBuffer.length, "SB");
	float total_fragments = sendBuffer.length / (float)(sid_ctx.scan_result.mtu-1);	// -1 space for header
	sid_ctx.scan_result.total_fragments = ceil(total_fragments);
	LOG_INF("scan num %u, mtu %d, total fragments %u", scan_counter, sid_ctx.scan_result.mtu, sid_ctx.scan_result.total_fragments);
	sid_ctx.scan_result.index = 0;
	sid_ctx.scan_result.total_bytes = sendBuffer.length;
	sid_ctx.scan_result.buffer = sendBuffer.buf;
	sid_ctx.scan_result.frag_type = FRAGMENT_TYPE_GNSS;
	sid_ctx.scan_result.send_attempt = 0;
	sid_ctx.scan_result.currently_sending = true;
	int err = sidewalk_event_send(SID_EVENT_NAV3_SEND, NULL);
	if (err) {
		LOG_ERR("send nav3-event err %d", err);
	}

	return 0;
}

int uplink_gnss_scan(const smtc_modem_gnss_event_data_scan_done_t *gnss, unsigned idx, bool last_scan)
{
	uint8_t buf[96];
	if (gnss->scans[idx].nav_size > sizeof(buf)-1) {
		LOG_ERR("gnss size too large %u", gnss->scans[idx].nav_size);
		return -1;
	}
	if (last_scan)
		buf[0] = 0x80 | gnss->token;
	else
		buf[0] = gnss->token;
	memcpy(buf+1, gnss->scans[idx].nav, gnss->scans[idx].nav_size);
	return uplink_scan(TLV_TAG_GNSS_NG, buf, gnss->scans[idx].nav_size+1);
}

void send_scan_result(sidewalk_ctx_t *sid_ctx)
{
	sid_error_t err;
	static struct sid_msg msg;
	static struct sid_msg_desc desc;
	static uint8_t dummy[258];
	uint8_t remaining = sid_ctx->scan_result.total_bytes - sid_ctx->scan_result.index;
	uint8_t this_length = remaining;
	uint8_t byte_per_fragment = sid_ctx->scan_result.mtu - 1;

#ifdef GNSS_DEBUG
	LOG_INF("send_scan_result index:%d total:%d cur_frag:%d mtu=%d", sid_ctx->scan_result.index, sid_ctx->scan_result.total_bytes, sid_ctx->scan_result.current_fragment, sid_ctx->scan_result.mtu);
#endif /* GNSS_DEBUG */

	if (this_length > byte_per_fragment)
		this_length = byte_per_fragment;

	sid_ctx->scan_result.nbytes_sent_this_fragment = this_length;

	dummy[0] = sid_ctx->scan_result.current_fragment & 7;
	dummy[0] |= (sid_ctx->scan_result.total_fragments & 7) << 3;
	dummy[0] |= (sid_ctx->scan_result.frag_type & 3) << 6;
	memcpy(dummy+1, sid_ctx->scan_result.buffer + sid_ctx->scan_result.index, this_length);

#ifdef GNSS_DEBUG
	LOG_HEXDUMP_INF(dummy, this_length+1, "fragment");
#endif /* GNSS_DEBUG */
	msg = (struct sid_msg){ .data = dummy, .size = this_length+1};
	desc = (struct sid_msg_desc){
		.type = SID_MSG_TYPE_NOTIFY,
		.link_type = SID_LINK_TYPE_ANY,
		.link_mode = SID_LINK_MODE_CLOUD,
	};

	err = sid_put_msg(sid_ctx->handle, &msg, &desc);
	switch (err) {
	case SID_ERROR_NONE: {
		application_state_sending(&global_state_notifier, true);
		LOG_INF("queued data message id:%d", desc.id);
		//lcd_display_string("    sending     ", 1, 0);
		break;
	}
	case SID_ERROR_TRY_AGAIN: {
		LOG_ERR("there is no space in the transmit queue, Try again.");
		break;
	}
	default: LOG_ERR("Unknown error returned from sid_put_msg() -> %d", err);
	}
}
#endif /* NAV3_SEND */

static void nav3_button_handler(uint32_t event)
{
	if (event == SID_EVENT_SEND_MSG) {
		LOG_INF("Send hello message");
		const char payload[] = "hello";
		sidewalk_msg_t *hello = sid_hal_malloc(sizeof(sidewalk_msg_t));
		if (!hello) {
			LOG_ERR("Failed to alloc memory for message context");
			return;
		}
		memset(hello, 0x0, sizeof(*hello));

		hello->msg.size = sizeof(payload);
		hello->msg.data = sid_hal_malloc(hello->msg.size);
		if (!hello->msg.data) {
			sid_hal_free(hello);
			LOG_ERR("Failed to allocate memory for message data");
			return;
		}
		memcpy(hello->msg.data, payload, hello->msg.size);

		hello->desc.type = SID_MSG_TYPE_NOTIFY;
		hello->desc.link_type = SID_LINK_TYPE_ANY;
		hello->desc.link_mode = SID_LINK_MODE_CLOUD;

		int err = sidewalk_event_send(SID_EVENT_SEND_MSG, hello);
		if (err) {
			sid_hal_free(hello->msg.data);
			sid_hal_free(hello);
			LOG_ERR("Send event err %d", err);
		} else {
			application_state_sending(&global_state_notifier, true);
		}
		return;
	} else if (event == SID_EVENT_LINK_SWITCH) {
		LOG_INF("TODO link switch");
	}

	sidewalk_event_send((sidewalk_event_t)event, NULL);
}

static int nav3_buttons_init(void)
{
	button_set_action_short_press(DK_BTN1, nav3_button_handler, SID_EVENT_SEND_MSG);
	button_set_action_long_press(DK_BTN1, nav3_button_handler, SID_EVENT_NORDIC_DFU);
	button_set_action_short_press(DK_BTN2, nav3_button_handler, SID_EVENT_CONNECT);
	button_set_action_long_press(DK_BTN2, nav3_button_handler, SID_EVENT_FACTORY_RESET);
	button_set_action(DK_BTN3, nav3_button_handler, SID_EVENT_LINK_SWITCH);

	return buttons_init();
}

static void on_sidewalk_event(bool in_isr, void *context)
{
	int err = sidewalk_event_send(SID_EVENT_SIDEWALK, NULL);
	if (err) {
		LOG_ERR("Send event err %d", err);
	};
}

static void on_sidewalk_send_error(sid_error_t error, const struct sid_msg_desc *msg_desc,
				   void *context)
{
	LOG_ERR("Message send err %d", (int)error);
	printk(JSON_NEW_LINE(JSON_OBJ(JSON_NAME(
		"on_send_error",
		JSON_OBJ(JSON_LIST_2(JSON_VAL_sid_error_t("error", error),
				     JSON_VAL_sid_msg_desc("sid_msg_desc", msg_desc, 0)))))));

	application_state_sending(&global_state_notifier, false);

#ifdef NAV3_SEND
	struct sid_timespec first_shot;
	sidewalk_ctx_t *sid_ctx = (sidewalk_ctx_t *)context;
	sid_clock_now(SID_CLOCK_SOURCE_UPTIME, &first_shot, NULL);
	first_shot.tv_sec += 1;	// retry speed
	if (sid_pal_timer_arm(&retry_timer, SID_PAL_TIMER_PRIO_CLASS_LOWPOWER, &first_shot, NULL) != SID_ERROR_NONE) {
        LOG_ERR("timer fail, aborting fragment-send");
        sid_ctx->scan_result.total_fragments = 0;   // indicate done sending
        sid_ctx->scan_result.current_fragment = 0;
		int err = sidewalk_event_send(SID_EVENT_NAV3_SEND, NULL);
		if (err) {
			LOG_ERR("send nav3-event err %d", err);
		}
	} else {
		/* should probably give up trying at some point */
		LOG_WRN("resend attempt %u", sid_ctx->scan_result.send_attempt);
	}
#endif /* NAV3_SEND */
}

static void on_sidewalk_msg_sent(const struct sid_msg_desc *msg_desc, void *context)
{
	LOG_INF("Message send success");
	printk(JSON_NEW_LINE(JSON_OBJ(JSON_NAME(
		"on_msg_sent", JSON_OBJ(JSON_VAL_sid_msg_desc("sid_msg_desc", msg_desc, 0))))));

	application_state_sending(&global_state_notifier, false);

#ifdef NAV3_SEND
	sidewalk_ctx_t *sid_ctx = (sidewalk_ctx_t *)context;
	if (sid_ctx->scan_result.total_fragments > 0) {
		/* done sending gnss fragment */
		sid_ctx->scan_result.index += sid_ctx->scan_result.nbytes_sent_this_fragment;
		if (++sid_ctx->scan_result.current_fragment < sid_ctx->scan_result.total_fragments) {
			/* send next fragment */
			sid_ctx->scan_result.send_attempt = 0;
			int err = sidewalk_event_send(SID_EVENT_NAV3_SEND, NULL);
			if (err) {
				LOG_ERR("send nav3-event err %d", err);
			}
		} else {
			smtc_modem_return_code_t rc;
			if (sid_ctx->scan_result.index != sid_ctx->scan_result.total_bytes) {
				LOG_ERR("incorrect send completion count (%d %d)", sid_ctx->scan_result.index, sid_ctx->scan_result.total_bytes);
				sid_ctx->scan_result.total_fragments = 0;	// indicate done sending
				sid_ctx->scan_result.current_fragment = 0;
				if (sid_ctx->scan_result.sending == SENDING_GNSS)
					rc = smtc_modem_gnss_scan( LBM_STACK_ID, SMTC_MODEM_GNSS_MODE_MOBILE, GEOLOCATION_GNSS_SCAN_PERIOD_S );
				else
					rc = smtc_modem_wifi_scan( LBM_STACK_ID, GEOLOCATION_WIFI_SCAN_PERIOD_S );
				if (rc != SMTC_MODEM_RC_OK)
					LOG_ERR("start scan fail");
			} else {
				bool completed = true;
				LOG_INF("done sending fragments (%u %u)", sid_ctx->scan_result.index, sid_ctx->scan_result.total_bytes);
				sid_ctx->scan_result.total_fragments = 0;	// indicate done sending
				sid_ctx->scan_result.current_fragment = 0;
				if (sid_ctx->scan_result.sending == SENDING_GNSS) {
					if (++gnss_scan_idx < gnss_scan_done_data.nb_scans_valid) {
						LOG_INF("another GNSS scan %d %d", gnss_scan_idx, gnss_scan_done_data.nb_scans_valid);
						/* another gnss scan to send */
						uplink_gnss_scan(&gnss_scan_done_data, gnss_scan_idx, (gnss_scan_idx+1) == gnss_scan_done_data.nb_scans_valid);
						completed = false;
					} else {
						/* no more gnss scans, launch next gnss scan */
						LOG_INF("no more GNSS scans %d %d", gnss_scan_idx, gnss_scan_done_data.nb_scans_valid);
						if (smtc_modem_gnss_scan( LBM_STACK_ID, SMTC_MODEM_GNSS_MODE_MOBILE, GEOLOCATION_GNSS_SCAN_PERIOD_S ) != SMTC_MODEM_RC_OK)
							LOG_ERR("smtc_modem_gnss_scan");
					}
				} else if (sid_ctx->scan_result.sending == SENDING_WIFI) {
					/* wifi: more than one valid scan? */
					LOG_INF("sending done of wifi scan");
					if (smtc_modem_wifi_scan( LBM_STACK_ID, GEOLOCATION_WIFI_SCAN_PERIOD_S ) != SMTC_MODEM_RC_OK)
						LOG_ERR("smtc_modem_wifi_scan fail");
				}

				if (completed && sid_ctx->scan_result.pending_send != SENDING_NONE) {
					int ret = -1;
					if (sid_ctx->scan_result.pending_send == SENDING_GNSS) {
						LOG_INF("uplink pending GNSS");
						ret = uplink_gnss_scan(&gnss_scan_done_data, gnss_scan_idx, gnss_scan_idx+1 == gnss_scan_done_data.nb_scans_valid);
					} else if (sid_ctx->scan_result.pending_send == SENDING_WIFI) {
						LOG_INF("uplink pending WIFI");
						ret = wifi_uplink();
					} else {
						LOG_ERR("unrecognized pending_send %d", sid_ctx->scan_result.pending_send);
					}
					if (ret == 0)
						sid_ctx->scan_result.pending_send = SENDING_NONE;
				} else {
					LOG_INF("completed %s, pending %s", completed ? "y" : "n", send_to_string(sid_ctx->scan_result.pending_send));
					if (completed) {
						sid_ctx->scan_result.currently_sending = false;
						if (sid_ctx->scan_result.run_engine_pending) {
							sid_ctx->scan_result.run_engine_pending = false;
							app_event_lbm();
						}
					}
				}
			}
		}
	}
#endif /* NAV3_SEND */
}

static void on_sidewalk_factory_reset(void *context)
{
	ARG_UNUSED(context);
#ifndef CONFIG_SID_END_DEVICE_CLI
	LOG_INF("Factory reset notification received from sid api");
	if (sid_hal_reset(SID_HAL_RESET_NORMAL)) {
		LOG_WRN("Cannot reboot");
	}
#else
	LOG_INF("sid_set_factory_reset success");
#endif
}

static void on_sidewalk_msg_received(const struct sid_msg_desc *msg_desc, const struct sid_msg *msg,
				     void *context)
{
	LOG_HEXDUMP_INF((uint8_t *)msg->data, msg->size, "Message received success");
	printk(JSON_NEW_LINE(JSON_OBJ(JSON_NAME(
		"on_msg_received", JSON_OBJ(JSON_VAL_sid_msg_desc("sid_msg_desc", msg_desc, 1))))));

	application_state_receiving(&global_state_notifier, true);
	application_state_receiving(&global_state_notifier, false);

#ifdef CONFIG_SID_END_DEVICE_ECHO_MSGS
	if (msg_desc->type == SID_MSG_TYPE_GET || msg_desc->type == SID_MSG_TYPE_SET) {
		LOG_INF("Send echo message");
		sidewalk_msg_t *echo = sid_hal_malloc(sizeof(sidewalk_msg_t));
		if (!echo) {
			LOG_ERR("Failed to allocate event context for echo message");
			return;
		}
		memset(echo, 0x0, sizeof(*echo));
		echo->msg.size = msg->size;
		echo->msg.data = sid_hal_malloc(echo->msg.size);
		if (!echo->msg.data) {
			LOG_ERR("Failed to allocate memory for message echo data");
			sid_hal_free(echo);
			return;
		}
		memcpy(echo->msg.data, msg->data, echo->msg.size);

		echo->desc.type = (msg_desc->type == SID_MSG_TYPE_GET) ? SID_MSG_TYPE_RESPONSE :
									 SID_MSG_TYPE_NOTIFY;
		echo->desc.id =
			(msg_desc->type == SID_MSG_TYPE_GET) ? msg_desc->id : msg_desc->id + 1;
		echo->desc.link_type = SID_LINK_TYPE_ANY;
		echo->desc.link_mode = SID_LINK_MODE_CLOUD;

		int err = sidewalk_event_send(SID_EVENT_SEND_MSG, echo);
		if (err) {
			sid_hal_free(echo->msg.data);
			sid_hal_free(echo);
			LOG_ERR("Send event err %d", err);
		} else {
			application_state_sending(&global_state_notifier, true);
		}
	};
#endif /* CONFIG_SID_END_DEVICE_ECHO_MSGS */
#ifdef NAV3_SEND
	const uint8_t *down_pay;
	downlink.rssi = msg_desc->msg_desc_attr.rx_attr.rssi;
	downlink.snr = msg_desc->msg_desc_attr.rx_attr.snr;
	down_pay = msg->data;
	downlink.fcnt = down_pay[2];
	downlink.fcnt <<= 8;
	downlink.fcnt |= down_pay[3];
#endif /* NAV3_SEND */
}

static void lr_timer_cb(void *arg, sid_pal_timer_t *owner)
{
	sidewalk_event_t *event = arg;
	sidewalk_event_send(*event, NULL);
	*event = SID_EVENT_LAST;
}

static void on_sidewalk_status_changed(const struct sid_status *status, void *context)
{
	int err = 0;
	uint32_t new_link_mask = status->detail.link_status_mask;
	struct sid_status *new_status = sid_hal_malloc(sizeof(struct sid_status));
	if (!new_status) {
		LOG_ERR("Failed to allocate memory for new status value");
	} else {
		memcpy(new_status, status, sizeof(struct sid_status));
	}
	sidewalk_event_send(SID_EVENT_NEW_STATUS, new_status);

	switch (status->state) {
	case SID_STATE_READY:
	case SID_STATE_SECURE_CHANNEL_READY:
#if defined(CONFIG_SID_END_DEVICE_NAV3) && !defined(NAV3_ONLY)
		struct sid_timespec first_shot, tm2;
		sid_clock_now(SID_CLOCK_SOURCE_UPTIME, &first_shot, NULL);
		tm2.tv_sec = 5;
		tm2.tv_nsec = 0;
		sid_time_add(&first_shot, &tm2); /* Add tm1 and tm2, set result in tm1 */
		lr_timer_cb_arg = SID_EVENT_LBM_INIT;
		if (sid_pal_timer_arm(&lr_timer, SID_PAL_TIMER_PRIO_CLASS_LOWPOWER, &first_shot, NULL) != SID_ERROR_NONE)
			LOG_ERR("sid_pal_timer_arm fail");
#endif
#ifdef NAV3_SEND
		sid_error_t err = sid_get_mtu(sid_ctx.handle, status->detail.link_status_mask, &sid_ctx.scan_result.mtu);
		if (err != SID_ERROR_NONE)
			LOG_ERR("%d = sid_get_mtu()", err);
		else
			LOG_INF("mtu is %d", sid_ctx.scan_result.mtu);
		downlink.link_status_mask = status->detail.link_status_mask;
#endif /* NAV3_SEND */
		application_state_connected(&global_state_notifier, true);
		LOG_INF("Status changed: ready");
		break;
	case SID_STATE_NOT_READY:
		sid_pal_timer_cancel(&lr_timer);
		application_state_connected(&global_state_notifier, false);
		LOG_INF("Status changed: not ready");
		break;
	case SID_STATE_ERROR:
		sid_pal_timer_cancel(&lr_timer);
		application_state_error(&global_state_notifier, true);
		LOG_INF("Status not changed: error");
		break;
	}

	if (err) {
		LOG_ERR("Send event err %d", err);
	}

	application_state_registered(&global_state_notifier,
				     status->detail.registration_status == SID_STATUS_REGISTERED);
	application_state_time_sync(&global_state_notifier,
				    status->detail.time_sync_status == SID_STATUS_TIME_SYNCED);

	LOG_INF("Device %sregistered, Time Sync %s, Link status: {BLE: %s, FSK: %s, LoRa: %s}",
		(SID_STATUS_REGISTERED == status->detail.registration_status) ? "Is " : "Un",
		(SID_STATUS_TIME_SYNCED == status->detail.time_sync_status) ? "Success" : "Fail",
		(new_link_mask & SID_LINK_TYPE_1) ? "Up" : "Down",
		(new_link_mask & SID_LINK_TYPE_2) ? "Up" : "Down",
		(new_link_mask & SID_LINK_TYPE_3) ? "Up" : "Down");

	for (int i = 0; i < SID_LINK_TYPE_MAX_IDX; i++) {
		enum sid_link_mode mode =
			(enum sid_link_mode)status->detail.supported_link_modes[i];

		if (mode) {
			LOG_INF("Link mode on %s = {Cloud: %s, Mobile: %s}",
				(SID_LINK_TYPE_1_IDX == i) ? "BLE" :
				(SID_LINK_TYPE_2_IDX == i) ? "FSK" :
				(SID_LINK_TYPE_3_IDX == i) ? "LoRa" :
							     "unknow",
				(mode & SID_LINK_MODE_CLOUD) ? "True" : "False",
				(mode & SID_LINK_MODE_MOBILE) ? "True" : "False");
		}
	}
}

void app_event_lbm()
{
	sidewalk_event_send(SID_EVENT_LBM, NULL);
}

void app_start(void)
{
	if (nav3_buttons_init()) {
		LOG_ERR("Cannot init buttons");
	}

#if defined(CONFIG_GPIO)
	state_watch_init_gpio(&global_state_notifier);
#endif
#if defined(CONFIG_LOG)
	state_watch_init_log(&global_state_notifier);
#endif
	application_state_working(&global_state_notifier, true);

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

	if (sid_pal_timer_init(&lr_timer, lr_timer_cb, &lr_timer_cb_arg) != SID_ERROR_NONE) {
		LOG_ERR("sid_pal_timer_init fail");
		return;
	}
	lr_timer_cb_arg = SID_EVENT_LAST;
	//radio_ctx = lr11xx_get_drv_ctx();

#ifdef NAV3_SEND
	sid_error_t err;
	if ((err = sid_pal_timer_init(&retry_timer, retry_timer_cb, &sid_ctx)) != SID_ERROR_NONE) {
		LOG_ERR("%d = sid_pal_timer_init()", err);
		return;
	}
#endif /* NAV3_SEND */

	sidewalk_start(&sid_ctx);
}
