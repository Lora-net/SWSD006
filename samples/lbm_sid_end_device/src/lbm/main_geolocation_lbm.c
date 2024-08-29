#include <sidewalk.h>
#include <sid_pal_common_ifc.h>
#include <sid_pal_radio_ifc.h>
#include <app_mfg_config.h>
#include <app_subGHz_config.h>

#include <smtc_modem_utilities.h>
#include <example_options.h>
#include <smtc_board_ralf.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(nav3_lbm, CONFIG_SIDEWALK_LOG_LEVEL);

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
//static const uint8_t user_gen_app_key[16] = USER_LORAWAN_GEN_APP_KEY;
static const uint8_t user_app_key[16]	 = USER_LORAWAN_APP_KEY;
#endif

#define CUSTOM_NB_TRANS ( 3 )
#define ADR_CUSTOM_LIST                                \
    {                                                  \
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 \
    }

static const uint8_t adr_custom_list[16] = ADR_CUSTOM_LIST;
static const uint8_t custom_nb_trans     = CUSTOM_NB_TRANS;

#define KEEP_ALIVE_PORT ( 2 )
#define KEEP_ALIVE_PERIOD_S ( 3600 / 2 )
#define KEEP_ALIVE_SIZE ( 4 )
uint8_t keep_alive_payload[KEEP_ALIVE_SIZE] = { 0x00 };

static uint8_t                  rx_payload[SMTC_MODEM_MAX_LORAWAN_PAYLOAD_LENGTH] = { 0 };  // Buffer for rx payload
static uint8_t                  rx_payload_size = 0;      // Size of the payload in the rx_payload buffer
static smtc_modem_dl_metadata_t rx_metadata     = { 0 };  // Metadata of downlink
static uint8_t                  rx_remaining    = 0;      // Remaining downlink payload in modem
/**
 * @brief User callback for modem event
 *
 *  This callback is called every time an event ( see smtc_modem_event_t ) appears in the modem.
 *  Several events may have to be read from the modem when this callback is called.
 */
static void modem_event_callback( void )
{
    LOG_INF( "get_event () callback");

    smtc_modem_event_t                                          current_event;
    uint8_t                                                     event_pending_count;
    uint8_t                                                     stack_id = STACK_ID;
    smtc_modem_gnss_event_data_scan_done_t                      scan_done_data;
    smtc_modem_gnss_event_data_terminated_t                     terminated_data;
    smtc_modem_almanac_demodulation_event_data_almanac_update_t almanac_update_data;
    smtc_modem_wifi_event_data_scan_done_t                      wifi_scan_done_data;
    smtc_modem_wifi_event_data_terminated_t                     wifi_terminated_data;

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
            SMTC_HAL_TRACE_ARRAY( "CHIP_EUI", chip_eui, SMTC_MODEM_EUI_LENGTH );
            smtc_modem_get_pin( stack_id, chip_pin );
            SMTC_HAL_TRACE_ARRAY( "CHIP_PIN", chip_pin, SMTC_MODEM_PIN_LENGTH );
#endif
            /* Set user region */
            smtc_modem_set_region( stack_id, MODEM_EXAMPLE_REGION );
            /* Schedule a Join LoRaWAN network */
            smtc_modem_join_network( stack_id );
            /* Start almanac demodulation service */
            smtc_modem_almanac_demodulation_set_constellations( stack_id, SMTC_MODEM_GNSS_CONSTELLATION_GPS_BEIDOU );
            smtc_modem_almanac_demodulation_start( stack_id );
            /* Set GNSS and Wi-Fi send mode */
            //smtc_modem_store_and_forward_flash_clear_data( stack_id );
            //smtc_modem_store_and_forward_set_state( stack_id, SMTC_MODEM_STORE_AND_FORWARD_ENABLE );
            smtc_modem_gnss_send_mode( stack_id, SMTC_MODEM_SEND_MODE_STORE_AND_FORWARD );
            smtc_modem_wifi_send_mode( stack_id, SMTC_MODEM_SEND_MODE_UPLINK );
            /* Program Wi-Fi scan */
            smtc_modem_wifi_scan( stack_id, 0 );
            /* Program GNSS scan */
            smtc_modem_gnss_set_constellations( stack_id, SMTC_MODEM_GNSS_CONSTELLATION_GPS_BEIDOU );
            smtc_modem_gnss_scan( stack_id, SMTC_MODEM_GNSS_MODE_MOBILE, 0 );
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
            /* Get event data */
            smtc_modem_gnss_get_event_data_scan_done( stack_id, &scan_done_data );
            break;

        case SMTC_MODEM_EVENT_GNSS_TERMINATED:
            LOG_INF( "Event received: GNSS_TERMINATED" );
            /* Get event data */
            smtc_modem_gnss_get_event_data_terminated( stack_id, &terminated_data );
            /* launch next scan */
            smtc_modem_gnss_scan( stack_id, SMTC_MODEM_GNSS_MODE_MOBILE, GEOLOCATION_GNSS_SCAN_PERIOD_S );
            break;

        case SMTC_MODEM_EVENT_GNSS_ALMANAC_DEMOD_UPDATE:
            LOG_INF( "Event received: GNSS_ALMANAC_DEMOD_UPDATE" );
            smtc_modem_almanac_demodulation_get_event_data_almanac_update( stack_id, &almanac_update_data );
            /* Store progress in keep alive payload */
            keep_alive_payload[0] = almanac_update_data.update_progress_gps;
            keep_alive_payload[1] = almanac_update_data.update_progress_beidou;
            LOG_INF( "GPS progress: %u%%", almanac_update_data.update_progress_gps );
            LOG_INF( "BDS progress: %u%%", almanac_update_data.update_progress_beidou );
            LOG_INF( "aborted: %u", almanac_update_data.stat_nb_aborted_by_rp );
            break;

        case SMTC_MODEM_EVENT_WIFI_SCAN_DONE:
            LOG_INF( "Event received: WIFI_SCAN_DONE" );
            /* Get event data */
            smtc_modem_wifi_get_event_data_scan_done( stack_id, &wifi_scan_done_data );
            break;

        case SMTC_MODEM_EVENT_WIFI_TERMINATED:
            LOG_INF( "Event received: WIFI_TERMINATED" );
            /* Get event data */
            smtc_modem_wifi_get_event_data_terminated( stack_id, &wifi_terminated_data );
            /* launch next scan */
            smtc_modem_wifi_scan( stack_id, GEOLOCATION_WIFI_SCAN_PERIOD_S );
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

static void radio_event_notifier(sid_pal_radio_events_t event)
{
	// SID_PAL_RADIO_EVENT_*
}

sid_pal_radio_rx_packet_t rx_packet;

static void radio_irq_handler(void)
{
}

void state_nav3_entry(void *o)
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
#endif /* CONFIG_SIDEWALK_SUBGHZ_SUPPORT */
	};

	sid_error_t e = sid_platform_init(&platform_parameters);
	if (SID_ERROR_NONE != e) {
		LOG_ERR("Sidewalk Platform Init err: %d", e);
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

	LOG_INF( "geolocation example is starting, status %x ", status_mask);
}

void state_nav3_run(void *o)
{
	sm_t *sm = (sm_t *)o;
	switch (sm->event.id) {
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
		case SID_EVENT_LBM:
			break;
		case SID_EVENT_TOGGLE_LBM_SIDEWALK:
			break;
		case SID_EVENT_REBOOT:
			break;
		case SID_EVENT_LAST:
			break;
	} // ..switch (sm->event.id)

	do {
		sm->sleep_time_ms = smtc_modem_run_engine();
		//LOG_INF("%u = smtc_modem_run_engine", sm->sleep_time_ms);
	} while(sm->sleep_time_ms == 0 || smtc_modem_is_irq_flag_pending());
}
