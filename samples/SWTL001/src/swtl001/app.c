#include <app.h>
#include <sid_error.h>
#include <sid_pal_radio_ifc.h>
#include <sid_pal_common_ifc.h> // sid_platform_init
#include <app_mfg_config.h> // sid_platform_init
#include <sid_api.h> // sid_platform_init
#include <app_subGHz_config.h> // sid_platform_init

#include <lr11xx_firmware_update.h>
#include <lr1110_transceiver_0401.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(application, CONFIG_SIDEWALK_LOG_LEVEL);

static void radio_event_notifier(sid_pal_radio_events_t event)
{
	// SID_PAL_RADIO_EVENT_*
}

sid_pal_radio_rx_packet_t rx_packet;

static void radio_irq_handler(void)
{
}

void app_start(void)
{
	sid_error_t err;
    platform_parameters_t platform_parameters = {
        .mfg_store_region.addr_start = APP_MFG_CFG_FLASH_START,
        .mfg_store_region.addr_end = APP_MFG_CFG_FLASH_END,
#ifdef CONFIG_SIDEWALK_SUBGHZ_SUPPORT
        .platform_init_parameters.radio_cfg =
#if defined(CONFIG_RADIO_LR11XX)
            (radio_lr11xx_device_config_t *)get_radio_cfg(),
#else
#error lr11xx_required
            (radio_sx126x_device_config_t *)get_radio_cfg(),
#endif  /* CONFIG_RADIO_LR11XX */
#else
#error sub_ghz_required
#endif /* CONFIG_SIDEWALK_SUBGHZ_SUPPORT */
    };

    sid_error_t e = sid_platform_init(&platform_parameters);
    if (SID_ERROR_NONE != e) {
        LOG_ERR("Sidewalk Platform Init err: %d", e);
        return;
    }

	if ((err = sid_pal_radio_init(radio_event_notifier, radio_irq_handler, &rx_packet)) != RADIO_ERROR_NONE) {
		LOG_ERR("%d = sid_pal_radio_init()", err);
		return;
	}

	const lr11xx_fw_update_status_t status =
		lr11xx_update_firmware( LR11XX_FIRMWARE_UPDATE_TO, LR11XX_FIRMWARE_VERSION,
		lr11xx_firmware_image, ( uint32_t ) LR11XX_FIRMWARE_IMAGE_SIZE );

	switch( status )
	{
		case LR11XX_FW_UPDATE_OK:
			LOG_INF( "Expected firmware running!" );
			LOG_INF( "Please flash another application (like EVK Demo App)" );
			break;
		case LR11XX_FW_UPDATE_WRONG_CHIP_TYPE:
			LOG_ERR( "Wrong chip type!" );
			break;
		case LR11XX_FW_UPDATE_ERROR:
			LOG_ERR( "Error! Wrong firmware version - please retry." );
			break;
	}
}

