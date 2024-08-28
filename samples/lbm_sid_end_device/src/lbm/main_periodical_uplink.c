#include <sidewalk.h>
#include <sid_pal_common_ifc.h>
#include <sid_pal_radio_ifc.h>
#include <app_mfg_config.h>
#include <app_subGHz_config.h>

#include <smtc_modem_utilities.h>
#include <smtc_modem_hal.h>
#include <example_options.h>
#include <smtc_board_ralf.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lbm_uplink, CONFIG_SIDEWALK_LOG_LEVEL);

/*!
 * @brief Stringify constants
 */
#define xstr( a ) str( a )
#define str( a ) #a

/*!
 * @brief Helper macro that returned a human-friendly message if a command does not return SMTC_MODEM_RC_OK
 *
 * @remark The macro is implemented to be used with functions returning a @ref smtc_modem_return_code_t
 *
 * @param[in] rc  Return code
 */

#define ASSERT_SMTC_MODEM_RC( rc_func )										  \
	do																		   \
	{																			\
		smtc_modem_return_code_t rc = rc_func;								   \
		if( rc == SMTC_MODEM_RC_NOT_INIT )									   \
		{																		\
			LOG_ERR( "In %s - %s (line %d): %s", __FILE__, __func__, __LINE__,   \
								  xstr( SMTC_MODEM_RC_NOT_INIT ) );			  \
		}																		\
		else if( rc == SMTC_MODEM_RC_INVALID )								   \
		{																		\
			LOG_ERR( "In %s - %s (line %d): %s", __FILE__, __func__, __LINE__,   \
								  xstr( SMTC_MODEM_RC_INVALID ) );			   \
		}																		\
		else if( rc == SMTC_MODEM_RC_BUSY )									  \
		{																		\
			LOG_ERR( "In %s - %s (line %d): %s", __FILE__, __func__, __LINE__,   \
								  xstr( SMTC_MODEM_RC_BUSY ) );				  \
		}																		\
		else if( rc == SMTC_MODEM_RC_FAIL )									  \
		{																		\
			LOG_ERR( "In %s - %s (line %d): %s", __FILE__, __func__, __LINE__,   \
								  xstr( SMTC_MODEM_RC_FAIL ) );				  \
		}																		\
		else if( rc == SMTC_MODEM_RC_NO_TIME )								   \
		{																		\
			LOG_WRN( "In %s - %s (line %d): %s", __FILE__, __func__, __LINE__,   \
									xstr( SMTC_MODEM_RC_NO_TIME ) );			 \
		}																		\
		else if( rc == SMTC_MODEM_RC_INVALID_STACK_ID )						  \
		{																		\
			LOG_ERR( "In %s - %s (line %d): %s", __FILE__, __func__, __LINE__,   \
								  xstr( SMTC_MODEM_RC_INVALID_STACK_ID ) );	  \
		}																		\
		else if( rc == SMTC_MODEM_RC_NO_EVENT )								  \
		{																		\
			LOG_INF( "In %s - %s (line %d): %s", __FILE__, __func__, __LINE__,   \
								 xstr( SMTC_MODEM_RC_NO_EVENT ) );			   \
		}																		\
	} while( 0 )

/**
 * Stack id value (multistacks modem is not yet available)
 */
#define STACK_ID 0


/**
 * @brief Stack credentials
 */
#if !defined( USE_LR11XX_CREDENTIALS )
static const uint8_t user_dev_eui[8]	  = USER_LORAWAN_DEVICE_EUI;
static const uint8_t user_join_eui[8]	 = USER_LORAWAN_JOIN_EUI;
static const uint8_t user_gen_app_key[16] = USER_LORAWAN_GEN_APP_KEY;
static const uint8_t user_app_key[16]	 = USER_LORAWAN_APP_KEY;
#endif

static uint8_t				  rx_payload[SMTC_MODEM_MAX_LORAWAN_PAYLOAD_LENGTH] = { 0 };  // Buffer for rx payload
static uint8_t				  rx_payload_size = 0;	  // Size of the payload in the rx_payload buffer
static smtc_modem_dl_metadata_t rx_metadata	 = { 0 };  // Metadata of downlink
static uint8_t				  rx_remaining	= 0;	  // Remaining downlink payload in modem
static uint32_t	  uplink_counter	   = 0;	  // uplink raising counter

/**
 * @brief Periodical uplink alarm delay in seconds
 */
#define PERIODICAL_UPLINK_DELAY_S 60

static void send_uplink_counter_on_port( uint8_t port )
{
	// Send uplink counter on port 102
	uint8_t buff[4] = { 0 };
	buff[0]		 = ( uplink_counter >> 24 ) & 0xFF;
	buff[1]		 = ( uplink_counter >> 16 ) & 0xFF;
	buff[2]		 = ( uplink_counter >> 8 ) & 0xFF;
	buff[3]		 = ( uplink_counter & 0xFF );
	ASSERT_SMTC_MODEM_RC( smtc_modem_request_uplink( STACK_ID, port, false, buff, 4 ) );
	// Increment uplink counter
	uplink_counter++;
}

static void modem_event_callback( void )
{
	LOG_INF( "Modem event callback");

	smtc_modem_event_t current_event;
	uint8_t			event_pending_count;
	uint8_t			stack_id = STACK_ID;

	// Continue to read modem event until all event has been processed
	do
	{
		// Read modem event
		ASSERT_SMTC_MODEM_RC( smtc_modem_get_event( &current_event, &event_pending_count ) );

		switch( current_event.event_type )
		{
		case SMTC_MODEM_EVENT_RESET:
			LOG_INF("Event received: RESET");

#if !defined( USE_LR11XX_CREDENTIALS )
			// Set user credentials
			ASSERT_SMTC_MODEM_RC( smtc_modem_set_deveui( stack_id, user_dev_eui ) );
			ASSERT_SMTC_MODEM_RC( smtc_modem_set_joineui( stack_id, user_join_eui ) );
			ASSERT_SMTC_MODEM_RC( smtc_modem_set_appkey( stack_id, user_gen_app_key ) );
			ASSERT_SMTC_MODEM_RC( smtc_modem_set_nwkkey( stack_id, user_app_key ) );
#else
			// Get internal credentials
			ASSERT_SMTC_MODEM_RC( smtc_modem_get_chip_eui( stack_id, chip_eui ) );
			SMTC_HAL_TRACE_ARRAY( "CHIP_EUI", chip_eui, SMTC_MODEM_EUI_LENGTH );
			ASSERT_SMTC_MODEM_RC( smtc_modem_get_pin( stack_id, chip_pin ) );
			SMTC_HAL_TRACE_ARRAY( "CHIP_PIN", chip_pin, SMTC_MODEM_PIN_LENGTH );
#endif
			// Set user region
			ASSERT_SMTC_MODEM_RC( smtc_modem_set_region( stack_id, MODEM_EXAMPLE_REGION ) );
			// Schedule a Join LoRaWAN network
			ASSERT_SMTC_MODEM_RC( smtc_modem_join_network( stack_id ) );
			app_event_lbm();
			break;

		case SMTC_MODEM_EVENT_ALARM:
			LOG_INF( "Event received: ALARM" );
			// Send periodical uplink on port 101
			send_uplink_counter_on_port( 101 );
			// Restart periodical uplink alarm
			ASSERT_SMTC_MODEM_RC( smtc_modem_alarm_start_timer( PERIODICAL_UPLINK_DELAY_S ) );
			break;

		case SMTC_MODEM_EVENT_JOINED:
			LOG_INF( "Event received: JOINED" );
			LOG_INF( "Modem is now joined " );

			// Send first periodical uplink on port 101
			send_uplink_counter_on_port( 101 );
			// start periodical uplink alarm
			ASSERT_SMTC_MODEM_RC( smtc_modem_alarm_start_timer( PERIODICAL_UPLINK_DELAY_S ) );
			break;

		case SMTC_MODEM_EVENT_TXDONE:
			LOG_INF( "Event received: TXDONE" );
			LOG_INF( "Transmission done " );
			break;

		case SMTC_MODEM_EVENT_DOWNDATA:
			LOG_INF( "Event received: DOWNDATA" );
			// Get downlink data
			ASSERT_SMTC_MODEM_RC(
				smtc_modem_get_downlink_data( rx_payload, &rx_payload_size, &rx_metadata, &rx_remaining ) );
			LOG_INF("Data received on port %u", rx_metadata.fport );
			LOG_HEXDUMP_INF(rx_payload, rx_payload_size, "Received payload");
			break;

		case SMTC_MODEM_EVENT_JOINFAIL:
			LOG_INF( "Event received: JOINFAIL" );
			break;

		case SMTC_MODEM_EVENT_ALCSYNC_TIME:
			LOG_INF( "Event received: ALCSync service TIME" );
			break;

		case SMTC_MODEM_EVENT_LINK_CHECK:
			LOG_INF( "Event received: LINK_CHECK" );
			break;

		case SMTC_MODEM_EVENT_CLASS_B_PING_SLOT_INFO:
			LOG_INF( "Event received: CLASS_B_PING_SLOT_INFO" );
			break;

		case SMTC_MODEM_EVENT_CLASS_B_STATUS:
			LOG_INF( "Event received: CLASS_B_STATUS" );
			break;

		case SMTC_MODEM_EVENT_LORAWAN_MAC_TIME:
			LOG_WRN( "Event received: LORAWAN MAC TIME" );
			break;

		case SMTC_MODEM_EVENT_LORAWAN_FUOTA_DONE:
		{
			bool status = current_event.event_data.fuota_status.successful;
			if( status == true )
			{
				LOG_INF( "Event received: FUOTA SUCCESSFUL" );
			}
			else
			{
				LOG_WRN( "Event received: FUOTA FAIL" );
			}
			break;
		}

		case SMTC_MODEM_EVENT_NO_MORE_MULTICAST_SESSION_CLASS_C:
			LOG_INF( "Event received: MULTICAST CLASS_C STOP" );
			break;

		case SMTC_MODEM_EVENT_NO_MORE_MULTICAST_SESSION_CLASS_B:
			LOG_INF( "Event received: MULTICAST CLASS_B STOP" );
			break;

		case SMTC_MODEM_EVENT_NEW_MULTICAST_SESSION_CLASS_C:
			LOG_INF( "Event received: New MULTICAST CLASS_C " );
			break;

		case SMTC_MODEM_EVENT_NEW_MULTICAST_SESSION_CLASS_B:
			LOG_INF( "Event received: New MULTICAST CLASS_B" );
			break;

		case SMTC_MODEM_EVENT_FIRMWARE_MANAGEMENT:
			LOG_INF( "Event received: FIRMWARE_MANAGEMENT" );
			if( current_event.event_data.fmp.status == SMTC_MODEM_EVENT_FMP_REBOOT_IMMEDIATELY )
			{
				smtc_modem_hal_reset_mcu( );
			}
			break;

		case SMTC_MODEM_EVENT_STREAM_DONE:
			LOG_INF( "Event received: STREAM_DONE" );
			break;

		case SMTC_MODEM_EVENT_UPLOAD_DONE:
			LOG_INF( "Event received: UPLOAD_DONE" );
			break;

		case SMTC_MODEM_EVENT_DM_SET_CONF:
			LOG_INF( "Event received: DM_SET_CONF" );
			break;

		case SMTC_MODEM_EVENT_MUTE:
			LOG_INF( "Event received: MUTE" );
			break;

		default:
			LOG_ERR( "Unknown event %u", current_event.event_type );
			break;
		}
	} while( event_pending_count > 0 );
}

static void radio_event_notifier(sid_pal_radio_events_t event)
{
	// SID_PAL_RADIO_EVENT_*
}

sid_pal_radio_rx_packet_t rx_packet;

static void radio_irq_handler(void)
{
}

void state_lbm_entry(void *o)
{
	smtc_modem_status_mask_t status_mask;
	sid_error_t err;

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
#else
	#error subghz_required
#endif /* CONFIG_SIDEWALK_SUBGHZ_SUPPORT */
	};

	err = sid_platform_init(&platform_parameters);
	if (SID_ERROR_NONE != err) {
		LOG_ERR("Sidewalk Platform Init err: %d", err);
		return;
	}

	if ((err = sid_pal_radio_init(radio_event_notifier, radio_irq_handler, &rx_packet)) != RADIO_ERROR_NONE) {
		LOG_ERR("%d = sid_pal_radio_init", err);
		return;
	}

	smtc_board_initialise_and_get_ralf();
	LOG_INF("sid_pal_radio_init done, smtc_modem_init...");
	// Init the modem and use modem_event_callback as event callback, please note that the callback will be
	// called immediately after the first call to smtc_modem_run_engine because of the reset detection
	smtc_modem_init(&modem_event_callback, true);
	smtc_modem_get_status(STACK_ID, &status_mask);

	LOG_INF( "Periodical uplink example is starting, status %x ", status_mask);
}

void state_lbm_run(void *o)
{
	sm_t *sm = (sm_t *)o;

	switch (sm->event.id) {
		case SID_EVENT_LBM:
			/* ? only run engine on this event ? */
			break;
		case SID_EVENT_SIDEWALK:
			break;
		case SID_EVENT_FACTORY_RESET:
			break;
		case SID_EVENT_NEW_STATUS:
			break;
		case SID_EVENT_SEND_MSG:
			break;
		case SID_EVENT_CONNECT:
			break;
		case SID_EVENT_LINK_SWITCH:
			break;
		case SID_EVENT_NORDIC_DFU:
			break;
		case SID_EVENT_FILE_TRANSFER:
			break;
		case SID_EVENT_TOGGLE_LBM_SIDEWALK:
			smf_set_state(SMF_CTX(sm), &sid_states[STATE_SIDEWALK]);
			break;
		case SID_EVENT_REBOOT:
			break;
		case SID_EVENT_LAST:
			break;
	}

	do {
		sm->sleep_time_ms = smtc_modem_run_engine();
	} while(sm->sleep_time_ms == 0 || smtc_modem_is_irq_flag_pending());
}
