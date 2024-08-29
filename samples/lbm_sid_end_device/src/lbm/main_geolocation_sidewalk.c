#include <sidewalk.h>
#include <sid_pal_common_ifc.h>
#include <app_mfg_config.h>
#ifdef CONFIG_SIDEWALK_SUBGHZ_SUPPORT
#include <app_subGHz_config.h>
#else
#error subghz_required
#endif
#include <sid_hal_memory_ifc.h>
#include <zephyr/logging/log.h>

#include "smtc_modem_utilities.h"
#include "smtc_board_ralf.h"
#include "lr11xx_system.h"
#include "halo_lr11xx_radio.h"

#include "example_options_swdm015-feb14.h"

/**
 * @brief Supported LR11XX radio firmware
 */
#define LR1110_FW_VERSION 0x0401
#define LR1120_FW_VERSION 0x0201

#define KEEP_ALIVE_PORT ( 2 )
#define KEEP_ALIVE_PERIOD_S ( 3600 / 2 )
#define KEEP_ALIVE_SIZE ( 4 )
uint8_t keep_alive_payload[KEEP_ALIVE_SIZE] = { 0x00 };

#define CUSTOM_NB_TRANS ( 3 )
#define ADR_CUSTOM_LIST								\
	{												  \
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 \
	}

static const uint8_t adr_custom_list[16] = ADR_CUSTOM_LIST;
static const uint8_t custom_nb_trans	 = CUSTOM_NB_TRANS;

/**
 * @brief Stack credentials
 */
#if !defined( USE_LR11XX_CREDENTIALS )
static const uint8_t user_dev_eui[8]  = USER_LORAWAN_DEVICE_EUI;
static const uint8_t user_join_eui[8] = USER_LORAWAN_JOIN_EUI;
static const uint8_t user_app_key[16] = USER_LORAWAN_APP_KEY;
#endif

static uint8_t				  rx_payload[SMTC_MODEM_MAX_LORAWAN_PAYLOAD_LENGTH] = { 0 };  // Buffer for rx payload
static uint8_t				  rx_payload_size = 0;	  // Size of the payload in the rx_payload buffer
static smtc_modem_dl_metadata_t rx_metadata	 = { 0 };  // Metadata of downlink
static uint8_t				  rx_remaining	= 0;	  // Remaining downlink payload in modem

#ifdef CONFIG_SIDEWALK_LINK_MASK_BLE
#define DEFAULT_LM (uint32_t)(SID_LINK_TYPE_1)
#elif CONFIG_SIDEWALK_LINK_MASK_FSK
#define DEFAULT_LM (uint32_t)(SID_LINK_TYPE_2)
#elif CONFIG_SIDEWALK_LINK_MASK_LORA
#define DEFAULT_LM (uint32_t)(SID_LINK_TYPE_1 | SID_LINK_TYPE_3)
#else
#define DEFAULT_LM (uint32_t)(SID_LINK_TYPE_1)
#endif

LOG_MODULE_REGISTER(main_geoloc, CONFIG_SIDEWALK_LOG_LEVEL);

extern volatile bool smtc_modem_hal_timer_running;

void state_nav3_entry(void *o)
{
	platform_parameters_t platform_parameters = {
		.mfg_store_region.addr_start = APP_MFG_CFG_FLASH_START,
		.mfg_store_region.addr_end = APP_MFG_CFG_FLASH_END,
#ifdef CONFIG_SIDEWALK_SUBGHZ_SUPPORT
		.platform_init_parameters.radio_cfg =
#if defined(CONFIG_RADIO_LR11XX)
			(radio_lr11xx_device_config_t *)get_radio_cfg(),
#else
			(radio_sx126x_device_config_t *)get_radio_cfg(),
#endif  /* CONFIG_RADIO_LR11XX */
#endif /* CONFIG_SIDEWALK_SUBGHZ_SUPPORT */
	};

	sid_error_t e = sid_platform_init(&platform_parameters);
	if (SID_ERROR_NONE != e) {
		LOG_ERR("Sidewalk Platform Init err: %d", e);
		return;
	}

	if (app_mfg_cfg_is_valid()) {
		LOG_ERR("The mfg.hex version mismatch");
		LOG_ERR("Check if the file has been generated and flashed properly");
		LOG_ERR("START ADDRESS: 0x%08x", APP_MFG_CFG_FLASH_START);
		LOG_ERR("SIZE: 0x%08x", APP_MFG_CFG_FLASH_SIZE);
		return;
	}

#ifdef CONFIG_SID_END_DEVICE_AUTO_START
	sm_t *sm = (sm_t *)o;

#ifdef CONFIG_SID_END_DEVICE_PERSISTENT_LINK_MASK
	int err = settings_utils_link_mask_get(&sm->sid->config.link_mask);
	if (err <= 0) {
		LOG_WRN("Link mask get failed %d", err);
		sm->sid->config.link_mask = 0;
		settings_utils_link_mask_set(DEFAULT_LM);
	}
#endif /* CONFIG_SID_END_DEVICE_PERSISTENT_LINK_MASK */

	if (!sm->sid->config.link_mask) {
		sm->sid->config.link_mask = DEFAULT_LM;
	}

	LOG_INF("Sidewalk link switch to %s",
		(SID_LINK_TYPE_3 & sm->sid->config.link_mask) ? "LoRa" :
		(SID_LINK_TYPE_2 & sm->sid->config.link_mask) ? "FSK" :
								"BLE");

	e = sid_init(&sm->sid->config, &sm->sid->handle);
	if (e) {
		LOG_ERR("sid init err %d", (int)e);
	}

#ifdef CONFIG_SMTC_CLI
	{
		halo_drv_semtech_ctx_t *radio_ctx = lr11xx_get_drv_ctx();
		radio_ctx->suppress_rx_timeout.csuso = csuso;
		LOG_INF("csuso %d", radio_ctx->suppress_rx_timeout.csuso);
	}
#endif /* CONFIG_SMTC_CLI */

#ifdef NAV3_ONLY
	/* starting LBM (for NAV3) without sidewalk started */
	sidewalk_event_send(SID_EVENT_LBM_INIT, NULL);

#else
#ifdef CONFIG_SIDEWALK_FILE_TRANSFER
	app_file_transfer_demo_init(sm->sid->handle);
#endif
	e = sid_start(sm->sid->handle, sm->sid->config.link_mask); // state_sidewalk_entry()
	if (e) {
		LOG_ERR("sid start err %d", (int)e);
	}

#if CONFIG_SID_END_DEVICE_AUTO_CONN_REQ
	if (sm->sid->config.link_mask & SID_LINK_TYPE_1) {
		enum sid_link_connection_policy set_policy =
			SID_LINK_CONNECTION_POLICY_AUTO_CONNECT;

		e = sid_option(sm->sid->handle, SID_OPTION_SET_LINK_CONNECTION_POLICY, &set_policy,
			       sizeof(set_policy));
		if (e) {
			LOG_ERR("sid option multi link manager err %d", (int)e);
		}

		struct sid_link_auto_connect_params ac_params = {
			.link_type = SID_LINK_TYPE_1,
			.enable = true,
			.priority = 0,
			.connection_attempt_timeout_seconds = 30
		};

		e = sid_option(sm->sid->handle, SID_OPTION_SET_LINK_POLICY_AUTO_CONNECT_PARAMS,
			       &ac_params, sizeof(ac_params));
		if (e) {
			LOG_ERR("sid option multi link policy err %d", (int)e);
		}
	}
#endif /* CONFIG_SID_END_DEVICE_AUTO_CONN_REQ */
#endif /* !NAV3_ONLY */

	lr11xx_system_version_t lr11xx_fw_version;
	lr11xx_status_t		 status;
	smtc_board_initialise_and_get_ralf();

	void *lr_ctx = lr11xx_get_drv_ctx();
	/* Check LR11XX Firmware version */
	status = lr11xx_system_get_version( lr_ctx, &lr11xx_fw_version );
	if( status != LR11XX_STATUS_OK )
	{
		LOG_ERR( "Failed to get LR11XX firmware version" );
	}
	if( ( lr11xx_fw_version.type == LR11XX_SYSTEM_VERSION_TYPE_LR1110 ) &&
		( lr11xx_fw_version.fw < LR1110_FW_VERSION ) )
	{
		LOG_ERR( "Wrong LR1110 firmware version, expected 0x%04X, got 0x%04X", LR1110_FW_VERSION,
							  lr11xx_fw_version.fw );
	}
	if( ( lr11xx_fw_version.type == LR11XX_SYSTEM_VERSION_TYPE_LR1120 ) &&
		( lr11xx_fw_version.fw < LR1120_FW_VERSION ) )
	{
		LOG_ERR( "Wrong LR1120 firmware version, expected 0x%04X, got 0x%04X", LR1120_FW_VERSION,
							  lr11xx_fw_version.fw );
	}
	LOG_INF( "LR11XX FW: 0x%04X, type: 0x%02X", lr11xx_fw_version.fw, lr11xx_fw_version.type );
	sm->lbm_initialized = false;

#endif /* CONFIG_SIDEWALK_AUTO_START */
}

#ifdef NAV3_SEND
smtc_modem_gnss_event_data_scan_done_t					  gnss_scan_done_data;
uint8_t gnss_scan_idx;

#endif /* !NAV3_SEND */

static void start_scans(uint8_t stack_id)
{
	smtc_modem_wifi_scan( stack_id, 0 );
	smtc_modem_gnss_scan( stack_id, SMTC_MODEM_GNSS_MODE_MOBILE, 0 );
}

static void modem_event_callback( void )
{
	LOG_INF("get_event () callback");

	smtc_modem_event_t										  current_event;
	uint8_t													 event_pending_count;
	uint8_t													 stack_id = LBM_STACK_ID;
#ifndef NAV3_SEND
	smtc_modem_gnss_event_data_scan_done_t					  gnss_scan_done_data;
	smtc_modem_wifi_event_data_scan_done_t					  wifi_scan_done_data;
#endif /* !NAV3_SEND */
	smtc_modem_gnss_event_data_terminated_t					 terminated_data;
	smtc_modem_almanac_demodulation_event_data_almanac_update_t almanac_update_data;
	smtc_modem_wifi_event_data_terminated_t					 wifi_terminated_data;

	// Continue to read modem event until all event has been processed
	do
	{
		// Read modem event
		smtc_modem_get_event( &current_event, &event_pending_count );

		switch( current_event.event_type )
		{
		case SMTC_MODEM_EVENT_RESET:
			LOG_INF( "Event received: RESET" );

#if !defined( USE_LR11XX_CREDENTIALS )
			/* Set user credentials */
			smtc_modem_set_deveui( stack_id, user_dev_eui );
			smtc_modem_set_joineui( stack_id, user_join_eui );
			smtc_modem_set_nwkkey( stack_id, user_app_key );
#else
			// Get internal credentials
			smtc_modem_get_chip_eui( stack_id, chip_eui );
			LOG_HEXDUMP_INF(chip_eui, SMTC_MODEM_EUI_LENGTH, "CHIP_EUI");
			smtc_modem_get_pin( stack_id, chip_pin );
			LOG_HEXDUMP_INF(chip_pin, SMTC_MODEM_PIN_LENGTH, "CHIP_PIN");
#endif
			/* Set user region */
			smtc_modem_set_region( stack_id, MODEM_EXAMPLE_REGION );
			smtc_modem_gnss_send_mode( stack_id, SMTC_MODEM_SEND_MODE_BYPASS );
			smtc_modem_wifi_send_mode( stack_id, SMTC_MODEM_SEND_MODE_BYPASS );
			/* Start almanac demodulation service */
			smtc_modem_almanac_demodulation_set_constellations( stack_id, SMTC_MODEM_GNSS_CONSTELLATION_GPS_BEIDOU );
			smtc_modem_almanac_demodulation_start( stack_id );
			/* Program Wi-Fi scan */
			// in start_scans -- smtc_modem_wifi_scan( stack_id, 0 );
			/* Program GNSS scan */
			smtc_modem_gnss_set_constellations( stack_id, SMTC_MODEM_GNSS_CONSTELLATION_GPS_BEIDOU );
			// in start_scans -- smtc_modem_gnss_scan( stack_id, SMTC_MODEM_GNSS_MODE_MOBILE, 0 );
			start_scans(stack_id);
			app_event_lbm();
			break;

		case SMTC_MODEM_EVENT_ALARM:
			LOG_INF( "Event received: ALARM" );
			smtc_modem_request_uplink( stack_id, KEEP_ALIVE_PORT, false, keep_alive_payload, KEEP_ALIVE_SIZE );
			smtc_modem_alarm_start_timer( KEEP_ALIVE_PERIOD_S );
			break;

		case SMTC_MODEM_EVENT_JOINED:
			LOG_INF( "Event received: JOINED" );
			/* Set custom ADR profile for geolocation */
			smtc_modem_adr_set_profile( stack_id, SMTC_MODEM_ADR_PROFILE_CUSTOM, adr_custom_list );
			smtc_modem_set_nb_trans( stack_id, custom_nb_trans );
			/* Start time for regular uplink */
			smtc_modem_alarm_start_timer( KEEP_ALIVE_PERIOD_S );
			break;

		case SMTC_MODEM_EVENT_TXDONE:
			LOG_INF( "Event received: TXDONE (%d)", current_event.event_data.txdone.status );
			LOG_INF( "Transmission done" );
			break;

		case SMTC_MODEM_EVENT_DOWNDATA:
			LOG_INF( "Event received: DOWNDATA" );
			// Get downlink data
			smtc_modem_get_downlink_data( rx_payload, &rx_payload_size, &rx_metadata, &rx_remaining );
			LOG_INF( "Data received on port %u", rx_metadata.fport );
			LOG_HEXDUMP_INF(rx_payload, rx_payload_size, "Received payload");
			break;

		case SMTC_MODEM_EVENT_JOINFAIL:
			LOG_INF( "Event received: JOINFAIL" );
			LOG_WRN( "Join request failed " );
			break;

		case SMTC_MODEM_EVENT_ALCSYNC_TIME:
			LOG_INF( "Event received: TIME" );
			break;

		case SMTC_MODEM_EVENT_GNSS_SCAN_DONE:
			LOG_INF( "Event received: GNSS_SCAN_DONE" );
#ifndef NAV3_SEND
			/* Get event data */
			smtc_modem_gnss_get_event_data_scan_done( stack_id, &gnss_scan_done_data );
#endif /* NAV3_SEND */
			break;

		case SMTC_MODEM_EVENT_GNSS_TERMINATED:
			LOG_INF( "Event received: GNSS_TERMINATED" );
#ifdef NAV3_SEND
			smtc_modem_gnss_get_event_data_scan_done( stack_id, &gnss_scan_done_data );
			if (gnss_scan_done_data.is_valid) {
				int ret;
				gnss_scan_idx = 0;
				ret = uplink_gnss_scan(&gnss_scan_done_data, gnss_scan_idx, gnss_scan_done_data.nb_scans_valid == 1);
				if (ret != 0) {
					/* failed to send, schedule next scan later */
					if (smtc_modem_gnss_scan( stack_id, SMTC_MODEM_GNSS_MODE_MOBILE, (GEOLOCATION_GNSS_SCAN_PERIOD_S*2)) != SMTC_MODEM_RC_OK)
						LOG_ERR("smtc_modem_gnss_scan fail");
				}
			} else
				smtc_modem_gnss_scan( stack_id, SMTC_MODEM_GNSS_MODE_MOBILE, GEOLOCATION_GNSS_SCAN_PERIOD_S );
#else
			/* launch next scan */
			smtc_modem_gnss_scan( stack_id, SMTC_MODEM_GNSS_MODE_MOBILE, GEOLOCATION_GNSS_SCAN_PERIOD_S );
#endif /* !NAV3_SEND */
			/* Get event data */
			smtc_modem_gnss_get_event_data_terminated( stack_id, &terminated_data );
			break;

		case SMTC_MODEM_EVENT_GNSS_ALMANAC_DEMOD_UPDATE:
			LOG_INF( "Event received: GNSS_ALMANAC_DEMOD_UPDATE" );
			smtc_modem_almanac_demodulation_get_event_data_almanac_update( stack_id, &almanac_update_data );
			/* Store progress in keep alive payload */
			keep_alive_payload[0] = almanac_update_data.update_progress_gps;
			keep_alive_payload[1] = almanac_update_data.update_progress_beidou;
			if (almanac_update_data.status_gps != SMTC_MODEM_GNSS_ALMANAC_UPDATE_STATUS_UNKNOWN)
				LOG_INF( "GPS progress: %u%%", almanac_update_data.update_progress_gps );
			if (almanac_update_data.status_beidou != SMTC_MODEM_GNSS_ALMANAC_UPDATE_STATUS_UNKNOWN)
				LOG_INF( "BDS progress: %u%%", almanac_update_data.update_progress_beidou );
			LOG_INF( "aborted: %u", almanac_update_data.stat_nb_aborted_by_rp );
			break;

		case SMTC_MODEM_EVENT_WIFI_SCAN_DONE:
			LOG_INF( "Event received: WIFI_SCAN_DONE" );
#ifndef NAV3_SEND
			/* Get event data */
			smtc_modem_wifi_get_event_data_scan_done( stack_id, &wifi_scan_done_data );
#endif /* !NAV3_SEND */
			break;

		case SMTC_MODEM_EVENT_WIFI_TERMINATED:
			LOG_INF( "Event received: WIFI_TERMINATED" );
#ifdef NAV3_SEND
			smtc_modem_wifi_get_event_data_scan_done( stack_id, &wifi_scan_done_data );
			if (wifi_send_results() != 0) {
				// failed to send, schedule next scan later
				if (smtc_modem_wifi_scan( stack_id, (GEOLOCATION_WIFI_SCAN_PERIOD_S*2) ) != SMTC_MODEM_RC_OK)
					LOG_ERR("smtc_modem_wifi_scan fail");
			}
#endif /* !NAV3_SEND */
			/* Get event data */
			smtc_modem_wifi_get_event_data_terminated( stack_id, &wifi_terminated_data );
			break;

		case SMTC_MODEM_EVENT_LINK_CHECK:
			LOG_INF( "Event received: LINK_CHECK" );
			break;

		case SMTC_MODEM_EVENT_CLASS_B_STATUS:
			LOG_INF( "Event received: CLASS_B_STATUS" );
			break;

		default:
			LOG_ERR( "Unknown event %u", current_event.event_type );
			break;
		}
	} while( event_pending_count > 0 );
}

void state_nav3_run(void *o)
{
	sm_t *sm = (sm_t *)o;
	sid_error_t e = SID_ERROR_NONE;

	//LOG_INF("state_nav3_run event id %u", sm->event.id);
	switch (sm->event.id) {
	case SID_EVENT_SIDEWALK: {
		e = sid_process(sm->sid->handle);
		if (e) {
			LOG_ERR("sid process err %d", (int)e);
		}
	} break;
	case SID_EVENT_FACTORY_RESET: {
#ifdef CONFIG_SID_END_DEVICE_PERSISTENT_LINK_MASK
		(void)settings_utils_link_mask_set(0);
#endif /* CONFIG_SIDEWALK_PERSISTENT_LINK_MASK */
		e = sid_set_factory_reset(sm->sid->handle);
		if (e) {
			LOG_ERR("sid factory reset err %d", (int)e);
		}
	} break;
	case SID_EVENT_LINK_SWITCH: {
		static uint32_t new_link_mask = DEFAULT_LM;

		switch (sm->sid->config.link_mask) {
		case SID_LINK_TYPE_1:
			new_link_mask = SID_LINK_TYPE_2;
			break;
		case SID_LINK_TYPE_2:
			new_link_mask = SID_LINK_TYPE_1 | SID_LINK_TYPE_3;
			break;
		default:
			new_link_mask = SID_LINK_TYPE_1;
			break;
		}
		sm->sid->config.link_mask = new_link_mask;

		LOG_INF("Sidewalk link switch to %s", (SID_LINK_TYPE_3 & new_link_mask) ? "LoRa" :
						      (SID_LINK_TYPE_2 & new_link_mask) ? "FSK" :
											  "BLE");
#ifdef CONFIG_SID_END_DEVICE_PERSISTENT_LINK_MASK
		int err = settings_utils_link_mask_set(new_link_mask);
		if (err) {
			LOG_ERR("New link mask set err %d", err);
		}
#endif /* CONFIG_SID_END_DEVICE_PERSISTENT_LINK_MASK */

		if (sm->sid->handle != NULL) {
#ifdef CONFIG_SIDEWALK_FILE_TRANSFER
			app_file_transfer_demo_deinit(sm->sid->handle);
#endif
			e = sid_deinit(sm->sid->handle);
			if (e) {
				LOG_ERR("sid deinit err %d", (int)e);
			}
		}

		e = sid_init(&sm->sid->config, &sm->sid->handle);
		if (e) {
			LOG_ERR("sid init err %d", (int)e);
		}
#ifdef CONFIG_SIDEWALK_FILE_TRANSFER
		app_file_transfer_demo_init(sm->sid->handle);
#endif
		e = sid_start(sm->sid->handle, sm->sid->config.link_mask); // state_sidewalk_run() SID_EVENT_LINK_SWITCH
		if (e) {
			LOG_ERR("sid start err %d", (int)e);
		}
#if CONFIG_SID_END_DEVICE_AUTO_CONN_REQ
		if (sm->sid->config.link_mask & SID_LINK_TYPE_1) {
			enum sid_link_connection_policy set_policy =
				SID_LINK_CONNECTION_POLICY_AUTO_CONNECT;

			e = sid_option(sm->sid->handle, SID_OPTION_SET_LINK_CONNECTION_POLICY,
				       &set_policy, sizeof(set_policy));
			if (e) {
				LOG_ERR("sid option multi link manager err %d", (int)e);
			}

			struct sid_link_auto_connect_params ac_params = {
				.link_type = SID_LINK_TYPE_1,
				.enable = true,
				.priority = 0,
				.connection_attempt_timeout_seconds = 30
			};

			e = sid_option(sm->sid->handle,
				       SID_OPTION_SET_LINK_POLICY_AUTO_CONNECT_PARAMS, &ac_params,
				       sizeof(ac_params));
			if (e) {
				LOG_ERR("sid option multi link policy err %d", (int)e);
			}
		}
#endif /* CONFIG_SID_END_DEVICE_AUTO_CONN_REQ */
	} break;
	case SID_EVENT_NORDIC_DFU: {
#ifdef CONFIG_SIDEWALK_FILE_TRANSFER
		app_file_transfer_demo_deinit(sm->sid->handle);
#endif
		e = sid_deinit(sm->sid->handle);
		if (e) {
			LOG_ERR("sid deinit err %d", (int)e);
		}
		smf_set_state(SMF_CTX(sm), &sid_states[STATE_DFU]);
	} break;
	case SID_EVENT_NEW_STATUS: {
		struct sid_status *p_status = (struct sid_status *)sm->event.ctx;
		if (!p_status) {
			LOG_ERR("sid new status is NULL");
			break;
		}

		memcpy(&sm->sid->last_status, p_status, sizeof(struct sid_status));
		sid_hal_free(p_status);
	} break;
	case SID_EVENT_SEND_MSG: {
		sidewalk_msg_t *p_msg = (sidewalk_msg_t *)sm->event.ctx;
		if (!p_msg) {
			LOG_ERR("sid send msg is NULL");
			break;
		}

		e = sid_put_msg(sm->sid->handle, &p_msg->msg, &p_msg->desc);
		if (e) {
			LOG_ERR("sid send err %d", (int)e);
		}
		LOG_DBG("sid send (type: %d, id: %u)", (int)p_msg->desc.type, p_msg->desc.id);
		sid_hal_free(p_msg->msg.data);
		sid_hal_free(p_msg);
	} break;
	case SID_EVENT_CONNECT: {
		if (!(sm->sid->config.link_mask & SID_LINK_TYPE_1)) {
			LOG_ERR("Can not request connection - BLE not enabled");
			return;
		}
		sid_error_t e = sid_ble_bcn_connection_request(sm->sid->handle, true);
		if (e) {
			LOG_ERR("sid conn req err %d", (int)e);
		}
	} break;
	case SID_EVENT_FILE_TRANSFER: {
#ifdef CONFIG_SIDEWALK_FILE_TRANSFER
		struct data_received_args *args = (struct data_received_args *)sm->event.ctx;
		if (!args) {
			LOG_ERR("File transfer event data is NULL");
			break;
		}
		LOG_INF("Received file Id %d; buffer size %d; file offset %d", args->desc.file_id,
			args->buffer->size, args->desc.file_offset);
		uint8_t hash_out[32];
		sid_pal_hash_params_t params = { .algo = SID_PAL_HASH_SHA256,
						 .data = args->buffer->data,
						 .data_size = args->buffer->size,
						 .digest = hash_out,
						 .digest_size = sizeof(hash_out) };

		sid_error_t e = sid_pal_crypto_hash(&params);
		if (e != SID_ERROR_NONE) {
			LOG_ERR("Failed to hash received file transfer with error %s",
				SID_ERROR_T_STR(e));
		} else {
#define HEX_PRINTER(a, ...) "%02X"
#define HEX_PRINTER_ARG(a, ...) hash_out[a]
			char hex_str[sizeof(hash_out) * 2 + 1] = { 0 };
			snprintf(hex_str, sizeof(hex_str), LISTIFY(32, HEX_PRINTER, ()),
				 LISTIFY(32, HEX_PRINTER_ARG, (, )));
			LOG_INF("SHA256: %s", hex_str);
		}

		sid_error_t ret = sid_bulk_data_transfer_release_buffer(
			sm->sid->handle, args->desc.file_id, args->buffer);
		if (ret != SID_ERROR_NONE) {
			LOG_ERR("sid_bulk_data_transfer_release_buffer returned %s",
				SID_ERROR_T_STR(ret));
		}
		sid_hal_free(args);
#endif /* CONFIG_SIDEWALK_FILE_TRANSFER */
	} break;
	case SID_EVENT_TOGGLE_LBM_SIDEWALK: // this is not used for NAV3-over-sidewalk
		LOG_ERR("lbm-sidewalk toggle not implemented in NAV3");
		break;
	case SID_EVENT_LBM_INIT:
		if (!sm->lbm_initialized) {
			smtc_modem_init(&modem_event_callback, false);
			sm->lbm_initialized = true;
		}
		break;
	case SID_EVENT_LBM:
		break;
#ifdef NAV3_SEND
	case SID_EVENT_NAV3_SEND:
		send_scan_result(sm->sid);
		break;
#endif /* NAV3_SEND */
	case SID_EVENT_REBOOT: {
	} break;
	case SID_EVENT_LAST:
		break;
	} // ..switch (sm->event.id)

#ifdef CONFIG_SID_END_DEVICE_CLI
	if (sm->event.id >= SID_EVENT_LAST) {
		app_dut_event_process(sm->event, sm->sid);
	}
#endif /* CONFIG_SID_END_DEVICE_CLI */

	if (sm->lbm_initialized) {
		if (sm->sid->scan_result.currently_sending) {
			sm->sid->scan_result.run_engine_pending = true;
			return;
		}
		do {
			sm->sleep_time_ms = smtc_modem_run_engine();
			//LOG_INF("%u = smtc_modem_run_engine", sm->sleep_time_ms);
		} while(sm->sleep_time_ms == 0 || smtc_modem_is_irq_flag_pending());
	}
}
