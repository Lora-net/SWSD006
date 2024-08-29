/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#ifndef SIDEWALK_APP_H
#define SIDEWALK_APP_H

#include <sid_api.h>
#include <zephyr/smf.h>
#include <sid_time_ops.h>
#include <sid_clock_ifc.h>
#include <sid_pal_timer_ifc.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/slist.h>

#define LBM_STACK_ID 0

enum state {
	STATE_SIDEWALK,
	STATE_DFU,
	STATE_SMTC,
#if defined(CONFIG_LBM_END_DEVICE)
	STATE_LBM,
#elif (defined(CONFIG_SID_END_DEVICE_NAV3) || defined(CONFIG_LBM_END_DEVICE_NAV3))
	STATE_NAV3,
#endif
};

typedef enum {
	SID_EVENT_SIDEWALK = 0,
	/* 1  */ SID_EVENT_FACTORY_RESET,
	/* 2  */ SID_EVENT_NEW_STATUS,
	/* 3  */ SID_EVENT_SEND_MSG,
	/* 4  */ SID_EVENT_CONNECT,
	/* 5  */ SID_EVENT_LINK_SWITCH,
	/* 6  */ SID_EVENT_NORDIC_DFU,
	/* 7  */ SID_EVENT_FILE_TRANSFER,
	/* 8  */ SID_EVENT_LBM,
	/* 9  */ SID_EVENT_TOGGLE_LBM_SIDEWALK,
#if defined(CONFIG_SID_END_DEVICE_NAV3)
	/* 10 */ SID_EVENT_LBM_INIT,
#endif
#ifdef NAV3_SEND
	/*    */ SID_EVENT_NAV3_SEND,
#endif /* !NAV3_SEND */
	/*    */ SID_EVENT_REBOOT,
	/*    */ SID_EVENT_LAST,
} sidewalk_event_t;

typedef struct {
	sidewalk_event_t id;
	void *ctx;
} sidewalk_ctx_event_t;

#if defined(NAV3_SEND) || defined(CONFIG_LBM_END_DEVICE_NAV3)
/*!
 * @brief Defines the delay before starting the next scan sequence, value in [s].
 */
#define GEOLOCATION_GNSS_SCAN_PERIOD_S ( 2 * 60 )
#define GEOLOCATION_WIFI_SCAN_PERIOD_S ( 1 * 60 )
#endif

#ifdef NAV3_SEND
typedef enum {
	SENDING_NONE = 0,
	SENDING_GNSS,
	SENDING_WIFI,
} sending_t;
#endif /* !NAV3_SEND */

typedef struct {
	struct sid_handle *handle;
	struct sid_config config;
	struct sid_status last_status;
#ifdef NAV3_SEND
	struct {
		uint8_t index;
		uint8_t nbytes_sent_this_fragment;
		uint8_t current_fragment;
		uint8_t total_fragments;
		uint8_t total_bytes;
		uint8_t num_bytes_sending;
		size_t mtu;
		uint8_t frag_type;
		const uint8_t *buffer;
		sending_t sending;
		sending_t pending_send;
		uint8_t send_attempt;
		bool currently_sending;
		bool run_engine_pending;
	} scan_result;
#endif /* !NAV3_SEND */
} sidewalk_ctx_t;

typedef struct sm_s {
	struct smf_ctx ctx;
	struct k_msgq msgq;
	sidewalk_ctx_event_t event;
	sidewalk_ctx_t *sid;
#if (defined(CONFIG_LBM_END_DEVICE) || defined(CONFIG_SID_END_DEVICE_NAV3) || defined(CONFIG_LBM_END_DEVICE_NAV3))
	uint32_t sleep_time_ms;
#endif
#if defined(CONFIG_SID_END_DEVICE_NAV3)
	bool lbm_initialized;
#endif
} sm_t;

typedef struct {
	sys_snode_t node;
	struct sid_msg msg;
	struct sid_msg_desc desc;
} sidewalk_msg_t;

typedef struct {
	enum sid_option option;
	void *data;
	size_t data_len;
} sidewalk_option_t;

typedef struct {
	uint32_t file_id;
	size_t file_offset;
	void *data;
	size_t data_size;
} sidewalk_transfer_t;

void sidewalk_start(sidewalk_ctx_t *context);

int sidewalk_event_send(sidewalk_event_t event, void *ctx);

sidewalk_msg_t *get_message_buffer(uint16_t message_id);

extern const struct smf_state sid_states[];
void app_event_lbm(void);
#if defined(CONFIG_LBM_END_DEVICE)
void state_lbm_entry(void *o);
void state_lbm_run(void *o);
#elif defined(CONFIG_SID_END_DEVICE_NAV3) || defined(CONFIG_LBM_END_DEVICE_NAV3)
void state_nav3_entry(void *o);
void state_nav3_run(void *o);
extern sid_pal_timer_t lr_timer;
extern sidewalk_event_t lr_timer_cb_arg;
#endif

#if defined(CONFIG_SID_END_DEVICE_NAV3) || defined(CONFIG_LBM_END_DEVICE_NAV3)
#include <smtc_modem_geolocation_api.h>
#endif

#ifdef NAV3_SEND
#define TLV_TAG_GNSS_SINGLE_SCAN	0x50
#define TLV_TAG_WIFI_SCAN			0x51
#define TLV_TAG_MODEM_MESSAGE		0x52
#define TLV_TAG_GNSS_NG				0x53
#define TLV_TAG_MIRROR_UPLINK		0x54
#define TLV_TAG_					0x55
extern smtc_modem_wifi_event_data_scan_done_t					  wifi_scan_done_data;
extern smtc_modem_gnss_event_data_scan_done_t					  gnss_scan_done_data;
extern uint8_t gnss_scan_idx;
int uplink_scan(uint8_t tlv_tag, const uint8_t *scan_result_buf, uint8_t scan_result_length);
int uplink_gnss_scan(const smtc_modem_gnss_event_data_scan_done_t *gnss, unsigned idx, bool);
int wifi_send_results(void); // prepare send, and call send
int wifi_uplink(void);
void send_scan_result(sidewalk_ctx_t *sid_ctx);
#endif /* NAV3_SEND */

#endif /* SIDEWALK_APP_H */
