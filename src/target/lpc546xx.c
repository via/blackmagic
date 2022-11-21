/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2014 Allen Ibara <aibara>
 * Copyright (C) 2015 Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2020 Eivind Bergem <eivindbergem>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "general.h"
#include "target.h"
#include "target_internal.h"
#include "cortexm.h"
#include "lpc_common.h"

#define LPC546XX_CHIPID 0x40000ff8U

#define IAP_ENTRYPOINT_LOCATION 0x03000204U

#define LPC546XX_ETBAHB_SRAM_BASE 0x20000000U

/* Only SRAM0 bank is enabled after reset */
#define LPC546XX_ETBAHB_SRAM_SIZE (64U * 1024U)

#define LPC546XX_WDT_MODE       0x4000c000U
#define LPC546XX_WDT_CNT        0x4000c004U
#define LPC546XX_WDT_FEED       0x4000c008U
#define LPC546XX_WDT_PERIOD_MAX 0xffffffU
#define LPC546XX_WDT_PROTECT    (1U << 4U)

#define LPC546XX_MAINCLKSELA 0x40000280U
#define LPC546XX_MAINCLKSELB 0x40000284U
#define LPC546XX_AHBCLKDIV   0x40000380U
#define LPC546XX_FLASHCFG    0x40000400U

#define IAP_RAM_SIZE LPC546XX_ETBAHB_SRAM_SIZE
#define IAP_RAM_BASE LPC546XX_ETBAHB_SRAM_BASE

#define IAP_PGM_CHUNKSIZE 4096U

static bool lpc546xx_cmd_erase_sector(target_s *t, int argc, const char **argv);
static bool lpc546xx_cmd_read_partid(target_s *t, int argc, const char **argv);
static bool lpc546xx_cmd_read_uid(target_s *t, int argc, const char **argv);
static bool lpc546xx_cmd_reset_attach(target_s *t, int argc, const char **argv);
static bool lpc546xx_cmd_reset(target_s *t, int argc, const char **argv);
static bool lpc546xx_cmd_write_sector(target_s *t, int argc, const char **argv);

static void lpc546xx_reset_attach(target_s *t);
static bool lpc546xx_flash_init(target_s *t);
static bool lpc546xx_flash_erase(target_flash_s *f, target_addr_t addr, size_t len);
static bool lpc546xx_mass_erase(target_s *t);
static void lpc546xx_wdt_set_period(target_s *t);
static void lpc546xx_wdt_kick(target_s *t);

const command_s lpc546xx_cmd_list[] = {
	{"erase_sector", lpc546xx_cmd_erase_sector, "Erase a sector by number"},
	{"read_partid", lpc546xx_cmd_read_partid, "Read out the 32-bit part ID using IAP."},
	{"read_uid", lpc546xx_cmd_read_uid, "Read out the 16-byte UID."},
	{"reset_attach", lpc546xx_cmd_reset_attach,
		"Reset target. Reset debug registers. Re-attach debugger. This restores "
		"the chip to the very start of program execution, after the ROM bootloader."},
	{"reset", lpc546xx_cmd_reset, "Reset target"},
	{"write_sector", lpc546xx_cmd_write_sector,
		"Write incrementing data 8-bit values across a previously erased sector"},
	{NULL, NULL, NULL},
};

static void lpc546xx_add_flash(
	target_s *t, uint32_t iap_entry, uint8_t base_sector, uint32_t addr, size_t len, size_t erasesize)
{
	struct lpc_flash *lf = lpc_add_flash(t, addr, len);
	lf->f.erase = lpc546xx_flash_erase;

	/* LPC546xx devices require the checksum value written into the vector table in sector 0 */
	lf->f.write = lpc_flash_write_magic_vect;

	lf->f.blocksize = erasesize;
	lf->f.writesize = IAP_PGM_CHUNKSIZE;
	lf->bank = 0;
	lf->base_sector = base_sector;
	lf->iap_entry = iap_entry;
	lf->iap_ram = IAP_RAM_BASE;
	lf->iap_msp = IAP_RAM_BASE + IAP_RAM_SIZE;
	lf->wdt_kick = lpc546xx_wdt_kick;
}

bool lpc546xx_probe(target_s *t)
{
	const uint32_t chipid = target_mem_read32(t, LPC546XX_CHIPID);
	uint32_t flash_size = 0;

	switch (chipid) {
	case 0x7f954605U:
		t->driver = "LPC54605J256";
		flash_size = 0x40000;
		break;
	case 0x7f954606U:
		t->driver = "LPC54606J256";
		flash_size = 0x40000;
		break;
	case 0x7f954607U:
		t->driver = "LPC54607J256";
		flash_size = 0x40000;
		break;
	case 0x7f954616U:
		t->driver = "LPC54616J256";
		flash_size = 0x40000;
		break;
	case 0xfff54605U:
		t->driver = "LPC54605J512";
		flash_size = 0x80000;
		break;
	case 0xfff54606U:
		t->driver = "LPC54606J512";
		flash_size = 0x80000;
		break;
	case 0xfff54607U:
		t->driver = "LPC54607J512";
		flash_size = 0x80000;
		break;
	case 0xfff54608U:
		t->driver = "LPC54608J512";
		flash_size = 0x80000;
		break;
	case 0xfff54616U:
		t->driver = "LPC54616J512";
		flash_size = 0x80000;
		break;
	case 0xfff54618U:
		t->driver = "LPC54618J512";
		flash_size = 0x80000;
		break;
	case 0xfff54628U:
		t->driver = "LPC54628J512";
		flash_size = 0x80000;
		break;
	default:
		return false;
	}

	t->mass_erase = lpc546xx_mass_erase;
	lpc546xx_add_flash(t, IAP_ENTRYPOINT_LOCATION, 0, 0x0, flash_size, 0x8000);

	/*
	 * Note: upper 96kiB is only usable after enabling the appropriate control
	 * register bits, see LPC546xx User Manual: §7.5.19 AHB Clock Control register 0
	 */
	target_add_ram(t, 0x20000000, 0x28000);
	target_add_commands(t, lpc546xx_cmd_list, "Lpc546xx");
	t->target_options |= CORTEXM_TOPT_INHIBIT_NRST;
	return true;
}

static void lpc546xx_reset_attach(target_s *t)
{
	/*
	 * To reset the LPC546xx into a usable state, we need to reset and let it
	 * step once, then attach the debug probe again. Otherwise the ROM bootloader
	 * is mapped to address 0x0, we can't perform flash operations on sector 0,
	 * and reading memory from sector 0 will return the contents of the ROM
	 * bootloader, not the flash
	 */
	target_reset(t);
	target_halt_resume(t, false);
	cortexm_attach(t);
}

static bool lpc546xx_mass_erase(target_s *t)
{
	const int result = lpc546xx_flash_erase(t->flash, t->flash->start, t->flash->length);
	if (result != 0)
		tc_printf(t, "Error erasing flash: %d\n", result);
	return result == 0;
}

static bool lpc546xx_cmd_erase_sector(target_s *t, int argc, const char **argv)
{
	if (argc > 1) {
		uint32_t sector_addr = strtoul(argv[1], NULL, 0);
		sector_addr *= t->flash->blocksize;
		return lpc546xx_flash_erase(t->flash, sector_addr, 1U);
	}
	return true;
}

static bool lpc546xx_cmd_read_partid(target_s *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	struct lpc_flash *f = (struct lpc_flash *)t->flash;
	uint32_t partid[4];
	if (lpc_iap_call(f, partid, IAP_CMD_PARTID))
		return false;
	tc_printf(t, "PART ID: 0x%08x\n", partid[0]);
	return true;
}

static bool lpc546xx_cmd_read_uid(target_s *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	struct lpc_flash *f = (struct lpc_flash *)t->flash;
	uint8_t uid[16];
	if (lpc_iap_call(f, uid, IAP_CMD_READUID))
		return false;
	tc_printf(t, "UID: 0x");
	for (uint32_t i = 0; i < sizeof(uid); ++i)
		tc_printf(t, "%02x", uid[i]);
	tc_printf(t, "\n");
	return true;
}

/* Reset everything, including debug; single step past the ROM bootloader so the system is in a sane state */
static bool lpc546xx_cmd_reset_attach(target_s *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	lpc546xx_reset_attach(t);

	return true;
}

/* Reset all major systems _except_ debug. Note that this will leave the system with the ROM bootloader mapped to 0x0 */
static bool lpc546xx_cmd_reset(target_s *t, int argc, const char **argv)
{
	(void)argc;
	(void)argv;

	/* Cortex-M4 Application Interrupt and Reset Control Register */
	static const uint32_t AIRCR = 0xe000ed0cU;
	/* Magic value key */
	static const uint32_t reset_val = 0x05fa0004U;

	/* System reset on target */
	target_mem_write(t, AIRCR, &reset_val, sizeof(reset_val));
	return true;
}

static bool lpc546xx_cmd_write_sector(target_s *t, int argc, const char **argv)
{
	if (argc > 1) {
		const uint32_t sector_size = t->flash->blocksize;
		uint32_t sector_addr = strtoul(argv[1], NULL, 0);
		sector_addr *= sector_size;

		if (!lpc546xx_flash_erase(t->flash, sector_addr, 1U))
			return false;

		uint8_t *buf = calloc(1, sector_size);
		for (uint32_t i = 0; i < sector_size; i++)
			buf[i] = i & 0xffU;

		const bool result = lpc_flash_write_magic_vect(t->flash, sector_addr, buf, sector_size);
		free(buf);
		return result;
	}
	return true;
}

static bool lpc546xx_flash_init(target_s *t)
{
	/*
	 * Reset the chip. It's unfortunate but we need to make sure the ROM
	 * bootloader is no longer mapped to 0x0 or flash blank check won't work after
	 * erasing that sector. Additionally, the ROM itself may increase the
	 * main clock frequency during its own operation, so we need to force
	 * it back to the 12MHz FRO to guarantee correct flash timing for the IAP API
	 */
	lpc546xx_reset_attach(t);

	/* Deal with WDT */
	lpc546xx_wdt_set_period(t);

	target_mem_write32(t, LPC546XX_MAINCLKSELA, 0);  // 12MHz FRO
	target_mem_write32(t, LPC546XX_MAINCLKSELB, 0);  // Use MAINCLKSELA
	target_mem_write32(t, LPC546XX_AHBCLKDIV, 0);    // Divide by 1
	target_mem_write32(t, LPC546XX_FLASHCFG, 0x1aU); // Recommended default
	return true;
}

static bool lpc546xx_flash_erase(target_flash_s *tf, target_addr_t addr, size_t len)
{
	if (!lpc546xx_flash_init(tf->t))
		return false;
	return lpc_flash_erase(tf, addr, len);
}

static void lpc546xx_wdt_set_period(target_s *t)
{
	/* Check if WDT is on */
	uint32_t wdt_mode = target_mem_read32(t, LPC546XX_WDT_MODE);

	/* If WDT on, we can't disable it, but we may be able to set a long period */
	if (wdt_mode && !(wdt_mode & LPC546XX_WDT_PROTECT))
		target_mem_write32(t, LPC546XX_WDT_CNT, LPC546XX_WDT_PERIOD_MAX);
}

static void lpc546xx_wdt_kick(target_s *t)
{
	/* Check if WDT is on */
	uint32_t wdt_mode = target_mem_read32(t, LPC546XX_WDT_MODE);

	/* If WDT on, poke it to reset it */
	if (wdt_mode) {
		target_mem_write32(t, LPC546XX_WDT_FEED, 0xaa);
		target_mem_write32(t, LPC546XX_WDT_FEED, 0xff);
	}
}
