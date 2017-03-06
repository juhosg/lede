/*
 *  MikroTik SPI-NOR RouterBOARDs support
 *
 *  - MikroTik RouterBOARD mAP L-2nD
 *  - MikroTik RouterBOARD 941L-2nD
 *  - MikroTik RouterBOARD 951Ui-2nD
 *  - MikroTik RouterBOARD 750UP r2
 *  - MikroTik RouterBOARD 750 r2
 *  - MikroTik RouterBOARD LHG 5nD
 *
 *  Preliminary support for the following hardware
 *  - MikroTik RouterBOARD wAP2nD
 *  - MikroTik RouterBOARD cAP2nD
 *  - MikroTik RouterBOARD mAP2nD
 *  Furthermore, the cAP lite (cAPL2nD) appears to feature the exact same
 *  hardware as the mAP L-2nD. It is unknown if they share the same board
 *  identifier.
 *
 *  Copyright (C) 2017 Thibaut VARENE <varenet@parisc-linux.org>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 as published
 *  by the Free Software Foundation.
 */

#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/routerboot.h>
#include <linux/gpio.h>

#include <linux/spi/spi.h>
#include <linux/spi/74x164.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

#include <asm/prom.h>
#include <asm/mach-ath79/ar71xx_regs.h>
#include <asm/mach-ath79/ath79.h>

#include "common.h"
#include "dev-eth.h"
#include "dev-spi.h"
#include "dev-gpio-buttons.h"
#include "dev-leds-gpio.h"
#include "dev-m25p80.h"
#include "dev-usb.h"
#include "dev-wmac.h"
#include "machtypes.h"
#include "routerboot.h"

#define RBSPI_KEYS_POLL_INTERVAL 20 /* msecs */
#define RBSPI_KEYS_DEBOUNCE_INTERVAL (3 * RBSPI_KEYS_POLL_INTERVAL)

#define RBSPI_HAS_USB		BIT(0)
#define RBSPI_HAS_WLAN0		BIT(1)
#define RBSPI_HAS_WLAN1		BIT(2)
#define RBSPI_HAS_WAN4		BIT(3)	/* has WAN port on PHY4 */
#define RBSPI_HAS_SSR		BIT(4)	/* has an SSR on SPI bus 0 */
#define RBSPI_HAS_POE		BIT(5)
#define RBSPI_HAS_MDIO1		BIT(6)

#define RB_ROUTERBOOT_OFFSET    0x0000
#define RB_BIOS_SIZE            0x1000
#define RB_SOFT_CFG_SIZE        0x1000

/* Flash partitions indexes */
enum {
	RBSPI_PART_RBOOT,
	RBSPI_PART_HCONF,
	RBSPI_PART_BIOS,
	RBSPI_PART_RBOOT2,
	RBSPI_PART_SCONF,
	RBSPI_PART_FIRMW,
	RBSPI_PARTS
};

static struct mtd_partition rbspi_spi_partitions[RBSPI_PARTS];

/*
 * Setup the SPI flash partition table based on initial parsing.
 * The kernel can be at any aligned position and have any size.
 */
static void __init rbspi_init_partitions(const struct rb_info *info)
{
	struct mtd_partition *parts = rbspi_spi_partitions;
	memset(parts, 0x0, sizeof(*parts));

	parts[RBSPI_PART_RBOOT].name = "routerboot";
	parts[RBSPI_PART_RBOOT].offset = RB_ROUTERBOOT_OFFSET;
	parts[RBSPI_PART_RBOOT].size = info->hard_cfg_offs;
	parts[RBSPI_PART_RBOOT].mask_flags = MTD_WRITEABLE;

	parts[RBSPI_PART_HCONF].name = "hard_config";
	parts[RBSPI_PART_HCONF].offset = info->hard_cfg_offs;
	parts[RBSPI_PART_HCONF].size = info->hard_cfg_size;
	parts[RBSPI_PART_HCONF].mask_flags = MTD_WRITEABLE;

	parts[RBSPI_PART_BIOS].name = "bios";
	parts[RBSPI_PART_BIOS].offset = info->hard_cfg_offs
					+ info->hard_cfg_size;
	parts[RBSPI_PART_BIOS].size = RB_BIOS_SIZE;
	parts[RBSPI_PART_BIOS].mask_flags = MTD_WRITEABLE;

	parts[RBSPI_PART_RBOOT2].name = "routerboot2";
	parts[RBSPI_PART_RBOOT2].offset = parts[RBSPI_PART_BIOS].offset
					+ RB_BIOS_SIZE;
	parts[RBSPI_PART_RBOOT2].size = info->soft_cfg_offs
					- parts[RBSPI_PART_RBOOT2].offset;
	parts[RBSPI_PART_RBOOT2].mask_flags = MTD_WRITEABLE;

	parts[RBSPI_PART_SCONF].name = "soft_config";
	parts[RBSPI_PART_SCONF].offset = info->soft_cfg_offs;
	parts[RBSPI_PART_SCONF].size = RB_SOFT_CFG_SIZE;

	parts[RBSPI_PART_FIRMW].name = "firmware";
	parts[RBSPI_PART_FIRMW].offset = parts[RBSPI_PART_SCONF].offset
					+ parts[RBSPI_PART_SCONF].size;
	parts[RBSPI_PART_FIRMW].size = MTDPART_SIZ_FULL;
}

static struct flash_platform_data rbspi_spi_flash_data = {
	.parts = rbspi_spi_partitions,
	.nr_parts = ARRAY_SIZE(rbspi_spi_partitions),
};

/* Several boards only have a single reset button wired to GPIO 16 */
#define RBSPI_GPIO_BTN_RESET16	16

static struct gpio_keys_button rbspi_gpio_keys_reset16[] __initdata = {
	{
		.desc = "Reset button",
		.type = EV_KEY,
		.code = KEY_RESTART,
		.debounce_interval = RBSPI_KEYS_DEBOUNCE_INTERVAL,
		.gpio = RBSPI_GPIO_BTN_RESET16,
		.active_low = 1,
	},
};

/* RB mAP L-2nD gpios */
#define RBMAPL_GPIO_LED_POWER	17
#define RBMAPL_GPIO_LED_USER	14
#define RBMAPL_GPIO_LED_ETH	4
#define RBMAPL_GPIO_LED_WLAN	11

static struct gpio_led rbmapl_leds[] __initdata = {
	{
		.name = "rb:green:power",
		.gpio = RBMAPL_GPIO_LED_POWER,
		.active_low = 0,
		.default_state = LEDS_GPIO_DEFSTATE_ON,
	}, {
		.name = "rb:green:user",
		.gpio = RBMAPL_GPIO_LED_USER,
		.active_low = 0,
	}, {
		.name = "rb:green:eth",
		.gpio = RBMAPL_GPIO_LED_ETH,
		.active_low = 0,
	}, {
		.name = "rb:green:wlan",
		.gpio = RBMAPL_GPIO_LED_WLAN,
		.active_low = 0,
	},
};

/* RB 941L-2nD gpios */
#define RBHAPL_GPIO_LED_USER   14
static struct gpio_led rbhapl_leds[] __initdata = {
	{
		.name = "rb:green:user",
		.gpio = RBHAPL_GPIO_LED_USER,
		.active_low = 1,
	},
};

/* common RB SSRs */
#define RBSPI_SSR_GPIO_BASE	40
#define RBSPI_SSR_GPIO(bit)	(RBSPI_SSR_GPIO_BASE + (bit))

/* RB 951Ui-2nD gpios */
#define RB952_SSR_BIT_LED_LAN1	0
#define RB952_SSR_BIT_LED_LAN2	1
#define RB952_SSR_BIT_LED_LAN3	2
#define RB952_SSR_BIT_LED_LAN4	3
#define RB952_SSR_BIT_LED_LAN5	4
#define RB952_SSR_BIT_USB_POWER	5
#define RB952_SSR_BIT_LED_WLAN	6
#define RB952_GPIO_SSR_CS	11
#define RB952_GPIO_LED_USER	4
#define RB952_GPIO_POE_POWER	14
#define RB952_GPIO_POE_STATUS	12
#define RB952_GPIO_USB_POWER	RBSPI_SSR_GPIO(RB952_SSR_BIT_USB_POWER)
#define RB952_GPIO_LED_LAN1	RBSPI_SSR_GPIO(RB952_SSR_BIT_LED_LAN1)
#define RB952_GPIO_LED_LAN2	RBSPI_SSR_GPIO(RB952_SSR_BIT_LED_LAN2)
#define RB952_GPIO_LED_LAN3	RBSPI_SSR_GPIO(RB952_SSR_BIT_LED_LAN3)
#define RB952_GPIO_LED_LAN4	RBSPI_SSR_GPIO(RB952_SSR_BIT_LED_LAN4)
#define RB952_GPIO_LED_LAN5	RBSPI_SSR_GPIO(RB952_SSR_BIT_LED_LAN5)
#define RB952_GPIO_LED_WLAN	RBSPI_SSR_GPIO(RB952_SSR_BIT_LED_WLAN)

static struct gpio_led rb952_leds[] __initdata = {
	{
		.name = "rb:green:user",
		.gpio = RB952_GPIO_LED_USER,
		.active_low = 0,
	}, {
		.name = "rb:blue:wlan",
		.gpio = RB952_GPIO_LED_WLAN,
		.active_low = 1,
	}, {
		.name = "rb:green:port1",
		.gpio = RB952_GPIO_LED_LAN1,
		.active_low = 1,
	}, {
		.name = "rb:green:port2",
		.gpio = RB952_GPIO_LED_LAN2,
		.active_low = 1,
	}, {
		.name = "rb:green:port3",
		.gpio = RB952_GPIO_LED_LAN3,
		.active_low = 1,
	}, {
		.name = "rb:green:port4",
		.gpio = RB952_GPIO_LED_LAN4,
		.active_low = 1,
	}, {
		.name = "rb:green:port5",
		.gpio = RB952_GPIO_LED_LAN5,
		.active_low = 1,
	},
};

/* RB wAP-2nD gpios */
#define RBWAP_GPIO_LED_USER	14
#define RBWAP_GPIO_LED_WLAN	11

static struct gpio_led rbwap_leds[] __initdata = {
	{
		.name = "rb:green:user",
		.gpio = RBWAP_GPIO_LED_USER,
		.active_low = 1,
	}, {
		.name = "rb:green:wlan",
		.gpio = RBWAP_GPIO_LED_WLAN,
		.active_low = 1,
	},
};

/* RB cAP-2nD gpios */
#define RBCAP_GPIO_LED_1	14
#define RBCAP_GPIO_LED_2	12
#define RBCAP_GPIO_LED_3	11
#define RBCAP_GPIO_LED_4	4
#define RBCAP_GPIO_LED_ALL	13

static struct gpio_led rbcap_leds[] __initdata = {
	{
		.name = "rb:green:rssi1",
		.gpio = RBCAP_GPIO_LED_1,
		.active_low = 1,
	}, {
		.name = "rb:green:rssi2",
		.gpio = RBCAP_GPIO_LED_2,
		.active_low = 1,
	}, {
		.name = "rb:green:rssi3",
		.gpio = RBCAP_GPIO_LED_3,
		.active_low = 1,
	}, {
		.name = "rb:green:rssi4",
		.gpio = RBCAP_GPIO_LED_4,
		.active_low = 1,
	},
};

/* RB mAP-2nD gpios */
#define RBMAP_SSR_BIT_LED_LAN1	0
#define RBMAP_SSR_BIT_LED_LAN2	1
#define RBMAP_SSR_BIT_LED_POEO	2
#define RBMAP_SSR_BIT_LED_USER	3
#define RBMAP_SSR_BIT_LED_WLAN	4
#define RBMAP_SSR_BIT_USB_POWER	5
#define RBMAP_SSR_BIT_LED_APCAP	6
#define RBMAP_GPIO_SSR_CS	11
#define RBMAP_GPIO_LED_POWER	4
#define RBMAP_GPIO_POE_POWER	14
#define RBMAP_GPIO_POE_STATUS	12
#define RBMAP_GPIO_USB_POWER	RBSPI_SSR_GPIO(RBMAP_SSR_BIT_USB_POWER)
#define RBMAP_GPIO_LED_LAN1	RBSPI_SSR_GPIO(RBMAP_SSR_BIT_LED_LAN1)
#define RBMAP_GPIO_LED_LAN2	RBSPI_SSR_GPIO(RBMAP_SSR_BIT_LED_LAN2)
#define RBMAP_GPIO_LED_POEO	RBSPI_SSR_GPIO(RBMAP_SSR_BIT_LED_POEO)
#define RBMAP_GPIO_LED_USER	RBSPI_SSR_GPIO(RBMAP_SSR_BIT_LED_USER)
#define RBMAP_GPIO_LED_WLAN	RBSPI_SSR_GPIO(RBMAP_SSR_BIT_LED_WLAN)
#define RBMAP_GPIO_LED_APCAP	RBSPI_SSR_GPIO(RBMAP_SSR_BIT_LED_APCAP)

static struct gpio_led rbmap_leds[] __initdata = {
	{
		.name = "rb:green:power",
		.gpio = RBMAP_GPIO_LED_POWER,
		.active_low = 1,
		.default_state = LEDS_GPIO_DEFSTATE_ON,
	}, {
		.name = "rb:green:eth1",
		.gpio = RBMAP_GPIO_LED_LAN1,
		.active_low = 1,
	}, {
		.name = "rb:green:eth2",
		.gpio = RBMAP_GPIO_LED_WLAN,
		.active_low = 1,
	}, {
		.name = "rb:red:poe_out",
		.gpio = RBMAP_GPIO_LED_POEO,
		.active_low = 1,
	}, {
		.name = "rb:green:user",
		.gpio = RBMAP_GPIO_LED_USER,
		.active_low = 1,
	}, {
		.name = "rb:green:wlan",
		.gpio = RBMAP_GPIO_LED_WLAN,
		.active_low = 1,
	}, {
		.name = "rb:green:ap_cap",
		.gpio = RBMAP_GPIO_LED_APCAP,
		.active_low = 1,
	},
};

/* RB LHG 5nD gpios */
#define RBLHG_GPIO_LED_0	13
#define RBLHG_GPIO_LED_1	12
#define RBLHG_GPIO_LED_2	4
#define RBLHG_GPIO_LED_3	21
#define RBLHG_GPIO_LED_4	18
#define RBLHG_GPIO_LED_ETH	14
#define RBLHG_GPIO_LED_POWER	11
#define RBLHG_GPIO_LED_USER	20
#define RBLHG_GPIO_BTN_RESET	15

static struct gpio_led rblhg_leds[] __initdata = {
	{
		.name = "rb:green:rssi0",
		.gpio = RBLHG_GPIO_LED_0,
		.active_low = 1,
	}, {
		.name = "rb:green:rssi1",
		.gpio = RBLHG_GPIO_LED_1,
		.active_low = 1,
	}, {
		.name = "rb:green:rssi2",
		.gpio = RBLHG_GPIO_LED_2,
		.active_low = 1,
	}, {
		.name = "rb:green:rssi3",
		.gpio = RBLHG_GPIO_LED_3,
		.active_low = 1,
	}, {
		.name = "rb:green:rssi4",
		.gpio = RBLHG_GPIO_LED_4,
		.active_low = 1,
	}, {
		.name = "rb:green:eth",
		.gpio = RBLHG_GPIO_LED_ETH,
		.active_low = 1,
	}, {
		.name = "rb:green:user",
		.gpio = RBLHG_GPIO_LED_USER,
		.active_low = 1,
	}, {
		.name = "rb:blue:power",
		.gpio = RBLHG_GPIO_LED_POWER,
		.active_low = 0,
		.default_state = LEDS_GPIO_DEFSTATE_ON,
	},
};

static struct gpio_keys_button rblhg_gpio_keys[] __initdata = {
	{
		.desc = "Reset button",
		.type = EV_KEY,
		.code = KEY_RESTART,
		.debounce_interval = RBSPI_KEYS_DEBOUNCE_INTERVAL,
		.gpio = RBLHG_GPIO_BTN_RESET,
		.active_low = 1,
	},
};


static struct gen_74x164_chip_platform_data rbspi_ssr_data = {
	.base = RBSPI_SSR_GPIO_BASE,
};

/* the spi-ath79 driver can only natively handle CS0. Other CS are bit-banged */
static int rbspi_spi_cs_gpios[] = {
	-ENOENT,	/* CS0 is always -ENOENT: natively handled */
	-ENOENT,	/* CS1 can be updated by the code as necessary */
};

static struct ath79_spi_platform_data rbspi_ath79_spi_data = {
	.bus_num = 0,
	.cs_gpios = rbspi_spi_cs_gpios,
};

/*
 * Global spi_board_info: devices that don't have an SSR only have the SPI NOR
 * flash on bus0 CS0, while devices that have an SSR add it on the same bus CS1
 */
static struct spi_board_info rbspi_spi_info[] = {
	{
		.bus_num	= 0,
		.chip_select	= 0,
		.max_speed_hz	= 25000000,
		.modalias	= "m25p80",
		.platform_data	= &rbspi_spi_flash_data,
	}, {
		.bus_num	= 0,
		.chip_select	= 1,
		.max_speed_hz	= 25000000,
		.modalias	= "74x164",
		.platform_data	= &rbspi_ssr_data,
	}
};

void __init rbspi_wlan_init(u16 id, int wmac_offset)
{
	char *art_buf;
	u8 wlan_mac[ETH_ALEN];

	art_buf = rb_get_ext_wlan_data(id);
	if (!art_buf)
		return;

	ath79_init_mac(wlan_mac, ath79_mac_base, wmac_offset);
	ath79_register_wmac(art_buf + 0x1000, wlan_mac);

	kfree(art_buf);
}

/* 
 * Common platform init routine for all SPI NOR devices.
 */
static int __init rbspi_platform_setup(void)
{
	const struct rb_info *info;
	char buf[64];

	info = rb_init_info((void *)(KSEG1ADDR(AR71XX_SPI_BASE)), 0x20000);
	if (!info)
		return -ENODEV;

	scnprintf(buf, sizeof(buf), "MikroTik %s",
		(info->board_name) ? info->board_name : "");
	mips_set_machine_name(buf);

	/* fix partitions based on flash parsing */
	rbspi_init_partitions(info);

	return 0;
}

/*
 * Common peripherals init routine for all SPI NOR devices.
 * Sets SPI and USB.
 */
static void __init rbspi_peripherals_setup(u32 flags)
{
	unsigned spi_n;

	if (flags & RBSPI_HAS_SSR)
		spi_n = ARRAY_SIZE(rbspi_spi_info);
	else
		spi_n = 1;     /* only one device on bus0 */

	rbspi_ath79_spi_data.num_chipselect = spi_n;
	rbspi_ath79_spi_data.cs_gpios = rbspi_spi_cs_gpios;
	ath79_register_spi(&rbspi_ath79_spi_data, rbspi_spi_info, spi_n);

	if (flags & RBSPI_HAS_USB)
		ath79_register_usb();
}

/*
 * Common network init routine for all SPI NOR devices.
 * Sets LAN/WAN/WLAN.
 */
static void __init rbspi_network_setup(u32 flags, int gmac1_offset,
					int wmac0_offset, int wmac1_offset)
{
	/* for QCA953x that will init mdio1_device/data */
	ath79_register_mdio(0, 0x0);
	if (flags & RBSPI_HAS_MDIO1)
		ath79_register_mdio(1, 0x0);

	if (flags & RBSPI_HAS_WAN4) {
		ath79_setup_ar934x_eth_cfg(0);

		/* set switch to oper mode 1, PHY4 connected to CPU */
		ath79_switch_data.phy4_mii_en = 1;
		ath79_switch_data.phy_poll_mask |= BIT(4);

		/* init GMAC0 connected to PHY4 at 100M */
		ath79_eth0_data.phy_if_mode = PHY_INTERFACE_MODE_MII;
		ath79_eth0_data.phy_mask = BIT(4);
		ath79_init_mac(ath79_eth0_data.mac_addr, ath79_mac_base, 0);
		ath79_register_eth(0);
	} else {
		/* set the SoC to SW_ONLY_MODE, which connects all PHYs
		 * to the internal switch.
		 * We hijack ath79_setup_ar934x_eth_cfg() to set the switch in
		 * the QCA953x, this works because this configuration bit is
		 * the same as the AR934x. There's no equivalent function for
		 * QCA953x for now. */
		ath79_setup_ar934x_eth_cfg(AR934X_ETH_CFG_SW_ONLY_MODE);
	}

	/* init GMAC1 */
	ath79_init_mac(ath79_eth1_data.mac_addr, ath79_mac_base, gmac1_offset);
	ath79_eth1_data.phy_if_mode = PHY_INTERFACE_MODE_GMII;
	ath79_register_eth(1);

	if (flags & RBSPI_HAS_WLAN0)
		rbspi_wlan_init(0, wmac0_offset);

	if (flags & RBSPI_HAS_WLAN1)
		rbspi_wlan_init(1, wmac1_offset);
}

/* 
 * Init the mAP lite hardware (QCA953x).
 * The mAP L-2nD (mAP lite) has a single ethernet port, connected to PHY0.
 * Trying to use GMAC0 in direct mode was unsucessful, so we're
 * using SW_ONLY_MODE, which connects PHY0 to MAC1 on the internal
 * switch, which is connected to GMAC1 on the SoC. GMAC0 is unused.
 */
static void __init rbmapl_setup(void)
{
	u32 flags = RBSPI_HAS_WLAN0;

	if (rbspi_platform_setup())
		return;

	rbspi_peripherals_setup(flags);

	/* GMAC1 is HW MAC, WLAN0 MAC is HW MAC + 1 */
	rbspi_network_setup(flags, 0, 1, 0);

	ath79_register_leds_gpio(-1, ARRAY_SIZE(rbmapl_leds), rbmapl_leds);

	/* mAP lite has a single reset button as gpio 16 */
	ath79_register_gpio_keys_polled(-1, RBSPI_KEYS_POLL_INTERVAL,
					ARRAY_SIZE(rbspi_gpio_keys_reset16),
					rbspi_gpio_keys_reset16);

	/* clear internal multiplexing */
	ath79_gpio_output_select(RBMAPL_GPIO_LED_ETH, AR934X_GPIO_OUT_GPIO);
	ath79_gpio_output_select(RBMAPL_GPIO_LED_POWER, AR934X_GPIO_OUT_GPIO);
}

/*
 * Init the hAP lite hardware (QCA953x).
 * The 941-2nD (hAP lite) has 4 ethernet ports, with port 2-4
 * being assigned to LAN on the casing, and port 1 being assigned
 * to "internet" (WAN) on the casing. Port 1 is connected to PHY3.
 * Since WAN is neither PHY0 nor PHY4, we cannot use GMAC0 with this device.
 */
static void __init rbhapl_setup(void)
{
	u32 flags = RBSPI_HAS_WLAN0;

	if (rbspi_platform_setup())
		return;

	rbspi_peripherals_setup(flags);

	/* GMAC1 is HW MAC, WLAN0 MAC is HW MAC + 4 */
	rbspi_network_setup(flags, 0, 4, 0);

	ath79_register_leds_gpio(-1, ARRAY_SIZE(rbhapl_leds), rbhapl_leds);

	/* hAP lite has a single reset button as gpio 16 */
	ath79_register_gpio_keys_polled(-1, RBSPI_KEYS_POLL_INTERVAL,
					ARRAY_SIZE(rbspi_gpio_keys_reset16),
					rbspi_gpio_keys_reset16);
}

/*
 * The hAP, hEX lite and hEX PoE lite share the same platform
 */
static void __init rbspi_952_750r2_setup(u32 flags)
{
	if (flags & RBSPI_HAS_SSR)
		rbspi_spi_cs_gpios[1] = RB952_GPIO_SSR_CS;

	rbspi_peripherals_setup(flags);

	/* GMAC1 is HW MAC + 1, WLAN0 MAC IS HW MAC + 5 */
	rbspi_network_setup(flags, 1, 5, 0);

	if (flags & RBSPI_HAS_USB)
		gpio_request_one(RB952_GPIO_USB_POWER,
				GPIOF_OUT_INIT_HIGH | GPIOF_EXPORT_DIR_FIXED,
				"USB power");

	if (flags & RBSPI_HAS_POE)
		gpio_request_one(RB952_GPIO_POE_POWER,
				GPIOF_OUT_INIT_HIGH | GPIOF_EXPORT_DIR_FIXED,
				"POE power");

	ath79_register_leds_gpio(-1, ARRAY_SIZE(rb952_leds), rb952_leds);

	/* These devices have a single reset button as gpio 16 */
	ath79_register_gpio_keys_polled(-1, RBSPI_KEYS_POLL_INTERVAL,
					ARRAY_SIZE(rbspi_gpio_keys_reset16),
					rbspi_gpio_keys_reset16);
}

/*
 * Init the hAP hardware (QCA953x).
 * The 951Ui-2nD (hAP) has 5 ethernet ports, with ports 2-5 being assigned
 * to LAN on the casing, and port 1 being assigned to "internet" (WAN).
 * Port 1 is connected to PHY4 (the ports are labelled in reverse physical
 * number), so the SoC can be set to connect GMAC0 to PHY4 and GMAC1 to the
 * internal switch for the LAN ports.
 * The device also has USB, PoE output and an SSR used for LED multiplexing.
 */
static void __init rb952_setup(void)
{
	u32 flags = RBSPI_HAS_WLAN0 | RBSPI_HAS_WAN4 | RBSPI_HAS_USB |
			RBSPI_HAS_SSR | RBSPI_HAS_POE;

	if (rbspi_platform_setup())
		return;

	rbspi_952_750r2_setup(flags);
}

/*
 * Init the hEX (PoE) lite hardware (QCA953x).
 * The 750UP r2 (hEX PoE lite) is nearly identical to the hAP, only without
 * WLAN. The 750 r2 (hEX lite) is nearly identical to the 750UP r2, only
 * without USB and POE. It shares the same bootloader board identifier.
 */
static void __init rb750upr2_setup(void)
{
	u32 flags = RBSPI_HAS_WAN4 | RBSPI_HAS_SSR;

	if (rbspi_platform_setup())
		return;

	/* differentiate the hEX lite from the hEX PoE lite */
	if (strstr(mips_get_machine_name(), "750UP r2"))
		flags |= RBSPI_HAS_USB | RBSPI_HAS_POE;

	rbspi_952_750r2_setup(flags);
}

/*
 * Init the LHG hardware (AR9344).
 * The LHG 5nD has a single ethernet port connected to PHY0.
 * Wireless is provided via 5GHz WLAN1.
 */
static void __init rblhg_setup(void)
{
	u32 flags = RBSPI_HAS_WLAN1 | RBSPI_HAS_MDIO1;

	if (rbspi_platform_setup())
		return;

	rbspi_peripherals_setup(flags);

	/* GMAC1 is HW MAC, WLAN1 MAC is HW MAC + 1 */
	rbspi_network_setup(flags, 0, 0, 1);

	ath79_register_leds_gpio(-1, ARRAY_SIZE(rblhg_leds), rblhg_leds);

	ath79_register_gpio_keys_polled(-1, RBSPI_KEYS_POLL_INTERVAL,
					ARRAY_SIZE(rblhg_gpio_keys),
					rblhg_gpio_keys);
}

/*
 * Init the wAP hardware (EXPERIMENTAL).
 * The wAP 2nD has a single ethernet port.
 */
static void __init rbwap_setup(void)
{
	u32 flags = RBSPI_HAS_WLAN0;

	if (rbspi_platform_setup())
		return;

	rbspi_peripherals_setup(flags);

	/* GMAC1 is HW MAC, WLAN0 MAC is HW MAC + 1 */
	rbspi_network_setup(flags, 0, 1, 0);

	ath79_register_leds_gpio(-1, ARRAY_SIZE(rbwap_leds), rbwap_leds);
}

/*
 * Init the cAP hardware (EXPERIMENTAL).
 * The cAP 2nD has a single ethernet port, and a global LED switch.
 */
static void __init rbcap_setup(void)
{
	u32 flags = RBSPI_HAS_WLAN0;

	if (rbspi_platform_setup())
		return;

	rbspi_peripherals_setup(flags);

	/* GMAC1 is HW MAC, WLAN0 MAC is HW MAC + 1 */
	rbspi_network_setup(flags, 0, 1, 0);

	gpio_request_one(RBCAP_GPIO_LED_ALL,
			 GPIOF_OUT_INIT_HIGH | GPIOF_EXPORT_DIR_FIXED,
			 "LEDs enable");

	ath79_register_leds_gpio(-1, ARRAY_SIZE(rbcap_leds), rbcap_leds);
}

/*
 * Init the mAP hardware (EXPERIMENTAL).
 * The mAP 2nD has two ethernet ports, PoE output and an SSR for LED
 * multiplexing.
 */
static void __init rbmap_setup(void)
{
	u32 flags = RBSPI_HAS_WLAN0 | RBSPI_HAS_SSR | RBSPI_HAS_POE;

	if (rbspi_platform_setup())
		return;

	rbspi_spi_cs_gpios[1] = RBMAP_GPIO_SSR_CS;
	rbspi_peripherals_setup(flags);

	/* GMAC1 is HW MAC, WLAN0 MAC is HW MAC + 2 */
	rbspi_network_setup(flags, 0, 2, 0);

	if (flags & RBSPI_HAS_POE)
		gpio_request_one(RBMAP_GPIO_POE_POWER,
				GPIOF_OUT_INIT_HIGH | GPIOF_EXPORT_DIR_FIXED,
				"POE power");

	ath79_register_leds_gpio(-1, ARRAY_SIZE(rbmap_leds), rbmap_leds);
}


MIPS_MACHINE_NONAME(ATH79_MACH_RB_MAPL, "map-hb", rbmapl_setup);
MIPS_MACHINE_NONAME(ATH79_MACH_RB_941, "H951L", rbhapl_setup);
MIPS_MACHINE_NONAME(ATH79_MACH_RB_952, "952-hb", rb952_setup);
MIPS_MACHINE_NONAME(ATH79_MACH_RB_750UPR2, "750-hb", rb750upr2_setup);
MIPS_MACHINE_NONAME(ATH79_MACH_RB_LHG5, "lhg", rblhg_setup);
MIPS_MACHINE_NONAME(ATH79_MACH_RB_WAP, "wap-hb", rbwap_setup);
MIPS_MACHINE_NONAME(ATH79_MACH_RB_CAP, "cap-hb", rbcap_setup);
MIPS_MACHINE_NONAME(ATH79_MACH_RB_MAP, "map2-hb", rbmap_setup);