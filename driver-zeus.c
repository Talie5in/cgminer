/*
 * Copyright 2013-2014 Con Kolivas <kernel@kolivas.org>
 * Copyright 2014 Zeus Integrated Systems Limited
 * Copyright 2014 Dominik Lehner
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <unistd.h>
#include <poll.h>
#ifndef WIN32
  #include <termios.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #ifndef O_CLOEXEC
    #define O_CLOEXEC 0
  #endif
#else
  #include <windows.h>
  #include <io.h>
#endif

#define ZEUS_USE_LIBUSB

#include "miner.h"
#ifdef ZEUS_USE_LIBUSB
  #include "usbutils.h"
#endif
#include "fpgautils.h"
#include "elist.h"
#include "util.h"
#include "driver-zeus.h"

#define using_libusb(info) ((info)->using_libusb > 0)
#define using_serial(info) ((info)->using_libusb == 0)

// Configuration options
extern bool opt_zeus_debug;
extern int opt_zeus_chips_count;		// number of Zeus chips chained together
extern int opt_zeus_chip_clk;			// frequency to run chips with
extern bool opt_zeus_nocheck_golden;		// bypass hashrate check

//static int opt_zeus_chips_count_max = 1;	// smallest power of 2 >= opt_zeus_chips_count
						// is currently auto-calculated

// Index for device-specific options
//static int option_offset = -1;

// Unset upon first hotplug check
static bool initial_startup_phase = true;

static struct name_chip_map {
	char	*model_name;
	int	chips_count;
} zeus_models[] = {
	{ "Blizzard",		6  },
	//{ "Cyclone",		96 },	// model renamed??
	{ "Hurricane X2",	48 },
	{ "Hurricane X3",	64 },
	{ "Thunder X2",		96 },
	{ "Thunder X3",		128 },
	{ NULL, 0 }
};

/************************************************************
 * Utility Functions
 ************************************************************/

static void flush_uart(int fd)
{
#ifdef WIN32
	const HANDLE fh = (HANDLE)_get_osfhandle(fd);
	PurgeComm(fh, PURGE_RXCLEAR);
#else
	tcflush(fd, TCIFLUSH);
#endif
}

static int flush_fd(int fd)
{
	static char discard[10];
	return read(fd, discard, sizeof(discard));
}

static void rev(unsigned char *s, size_t l)
{
	size_t i, j;
	unsigned char t;

	for (i = 0, j = l - 1; i < j; i++, j--) {
		t = s[i];
		s[i] = s[j];
		s[j] = t;
	}
}

static int log_2(int value)
{
	int x = 0;
	while (value > 1) {
		value >>= 1;
		x++;
	}
	return x;
}

static uint32_t __maybe_unused chip_index(uint32_t value, int bit_num)
{
	uint32_t newvalue = 0;
	int i;

	// isolate bits 19-28, then shift right to get the
	// highest bits that distinguish multiple chips
	value = (value & 0x1ff80000) >> (29 - bit_num);

	for (i = 0; i < bit_num; i++) {
		newvalue = newvalue << 1;
		newvalue += value & 0x01;
		value = value >> 1;
	}

	return newvalue;
}

static int lowest_pow2(int min)
{
	int i;
	for (i = 1; i < 1024; i = i * 2) {
		if (min <= i){
			return i;
		}
	}
	return 1024;
}

static void notify_io_thread(struct cgpu_info *zeus)
{
	struct ZEUS_INFO *info = zeus->device_data;
	static char tickle = 'W';
	(void)write(info->wu_pipefd[PIPE_W], &tickle, 1);
}

static const uint64_t diff_code = 0xFFFF000000000000ull;
static double target_diff(const unsigned char *target)
{
        uint64_t *data64, d64;
        char rtarget[32];

        swab256(rtarget, target);
        data64=(uint64_t *)(rtarget + 2);
        d64=be64toh(*data64);
        if(unlikely(!d64))
        d64=1;

        return diff_code/d64;
}

/************************************************************
 * I/O helper functions
 ************************************************************/

#define zeus_serial_open_detect(devpath, baud, purge) serial_open_ex(devpath, baud, ZEUS_READ_FAULT_DECISECONDS, 0, purge)
#define zeus_serial_open(devpath, baud, purge) serial_open_ex(devpath, baud, ZEUS_READ_FAULT_DECISECONDS, 1, purge)
#define zeus_serial_close(fd) close(fd)

static bool zeus_reopen(struct cgpu_info *zeus)
{
	struct ZEUS_INFO *info = zeus->device_data;
	int try, fd = -1;

	if (using_libusb(info))		// with libusb let hotplog take care of reopening
		return false;

	if (info->device_fd != -1) {
		applog(LOG_DEBUG, "Closing %s%d on %s (fd=%d)",
			zeus->drv->name, zeus->device_id, zeus->device_path, info->device_fd);
		zeus_serial_close(info->device_fd);
		info->device_fd = -1;
		cgsleep_ms(2000);
	}

	applog(LOG_DEBUG, "Attempting to open %s%d on %s",
		zeus->drv->name, zeus->device_id, zeus->device_path);

	for (try = 0; try < 3; ++try) {
		fd = zeus_serial_open(zeus->device_path, info->baud, true);
		if (likely(fd > -1))
			break;
		cgsleep_ms(3000);
	}

	if (unlikely(fd < 0)) {
		applog(LOG_ERR, "Failed to open %s%d on %s (%d attempts)",
			zeus->drv->name, zeus->device_id, zeus->device_path, try);
		return false;
	}

	info->device_fd = fd;

	applog(LOG_DEBUG, "Successfully opened %s%d on %s (%d attempts, fd=%d)",
		zeus->drv->name, zeus->device_id, zeus->device_path, try, info->device_fd);

	return true;
}

static int zeus_serial_write(int fd, const void *buf, size_t len)
{
	ssize_t ret;
	size_t total = 0;

#if ZEUS_PROTOCOL_DEBUG
	if (opt_zeus_debug) {
		char *hexstr;
		hexstr = bin2hex(buf, len);
		applog(LOG_DEBUG, "> %s", hexstr);
		free(hexstr);
	}
#endif

	while (total < len) {
		ret = write(fd, buf, len);
		if (ret < 0) {
			applog(LOG_ERR, "zeus_serial_write (%d): error on write: %s", fd, strerror(errno));
			return -1;
		}
		total += (size_t)ret;
	}

	return total;
}

static int zeus_serial_read(int fd, void *buf, size_t len, int read_count, struct timeval *tv_firstbyte)
{
	ssize_t ret;
	size_t total = 0;
	int rc = 0;

	while (total < len) {
		ret = read(fd, buf + total, len - total);
		if (ret < 0) {
			applog(LOG_ERR, "zeus_serial_read (%d): error on read: %s", fd, strerror(errno));
			return -1;
		}

		if (tv_firstbyte != NULL && total == 0)
			cgtime(tv_firstbyte);

		applog(LOG_DEBUG, "zeus_serial_read: read returned %d", (int)ret);

		if (ret == 0 && ++rc >= read_count)
			break;

		total += (size_t)ret;
	}

#if ZEUS_PROTOCOL_DEBUG
	if (opt_zeus_debug) {
		char *hexstr;
		if (total > 0) {
			hexstr = bin2hex(buf, total);
			applog(LOG_DEBUG, "< %s", hexstr);
			free(hexstr);
		} else {
			applog(LOG_DEBUG, "< (no data)");
		}
	}
#endif

	return total;
}

/************************************************************
 * Detection and setup
 ************************************************************/

static unsigned char zeus_clk_to_freqcode(int clkfreq)
{
	if (clkfreq > ZEUS_CLK_MAX) {
		applog(LOG_WARNING, "Clock frequency %d too high, resetting to %d",
								clkfreq, ZEUS_CLK_MAX);
		clkfreq = ZEUS_CLK_MAX;
	}

	if (clkfreq < ZEUS_CLK_MIN) {
		applog(LOG_WARNING, "Clock frequency %d too low, resetting to %d",
								clkfreq, ZEUS_CLK_MIN);
		clkfreq = ZEUS_CLK_MIN;
	}

	return (unsigned char)((double)clkfreq * 2. / 3.);
}

static void zeus_get_device_options(const char *devid, int *chips_count, int *chip_clk, const char *options)
{
	char *p, *all, *found = NULL;
	long lval;
	int index = 0;

	// set global default options
	*chips_count = (opt_zeus_chips_count) ? opt_zeus_chips_count : ZEUS_MIN_CHIPS;
	*chip_clk = (opt_zeus_chip_clk) ? opt_zeus_chip_clk : ZEUS_CLK_MIN;

	if (options == NULL)
		return;

	all = strdup(options);

	for (p = strtok(all, ";"); p != NULL; p = strtok(NULL, ";")) {
		if (strncmp(p, devid, strlen(devid)) == 0) {
			found = p;
			break;
		}
	}

	if (found == NULL) {
		free(all);
		return;
	}

	for (p = strtok(found, ","); p != NULL; p = strtok(NULL, ",")) {
		lval = strtol(p, NULL, 10);

		switch (index++) {
			case 1:			// chip count
				if (lval < ZEUS_MIN_CHIPS || lval > ZEUS_MAX_CHIPS) {
					applog(LOG_ERR, "Invalid chip count %ld for Zeus device %s",
					       lval, devid);
					break;
				}
				*chips_count = (int)lval;
				break;
			case 2:			// clock
				if (lval < ZEUS_CLK_MIN || lval > ZEUS_CLK_MAX) {
					applog(LOG_ERR, "Invalid clock speed %ld for Zeus device %s",
					       lval, devid);
					break;
				}
				*chip_clk = (int)lval;
				break;
			default:
				break;
		}
	}

	free(all);
	return;
}

static char *zeus_device_name(int chips_count)
{
	struct name_chip_map *p;

	for (p = zeus_models; p->model_name != NULL; ++p) {
		if (p->chips_count == chips_count)
			return p->model_name;
	}

	return NULL;
}

static int zeus_usb_control_transfer(struct cgpu_info *zeus, uint8_t request_type, uint8_t bRequest,
		uint16_t wValue, uint16_t wIndex, uint32_t *data, int siz, enum usb_cmds cmd)
{
	int err = usb_transfer_data(zeus, request_type, bRequest, wValue, wIndex, data, siz, cmd);
	if (err)
		applog(LOG_DEBUG, "%s%d: error %d on USB control transfer %s",
			zeus->drv->name, zeus->cgminer_id, err, usb_cmdname(cmd));
	return err;
}

static bool zeus_initialize_usbuart(struct cgpu_info *zeus)
{
	int interface = usb_interface(zeus);
	//uint32_t baudrate = CP210X_DATA_BAUD;

	// Enable the UART
	if (zeus_usb_control_transfer(zeus, CP210X_TYPE_OUT, CP210X_REQUEST_IFC_ENABLE,
			CP210X_VALUE_UART_ENABLE, interface, NULL, 0, C_ENABLE_UART))
		return false;

	// Set data control
	if (zeus_usb_control_transfer(zeus, CP210X_TYPE_OUT, CP210X_REQUEST_DATA,
			CP210X_VALUE_DATA, interface, NULL, 0, C_SETDATA))
		return false;

	// Zeusminers have baud hardcoded to 115200, and reject baud commands, even to same value
	// Set the baud
	//if (zeus_usb_control_transfer(zeus, CP210X_TYPE_OUT, CP210X_REQUEST_BAUD,
	//		0, interface, &baudrate, sizeof(baudrate), C_SETBAUD))
	//	return false;

	return true;
}

static struct cgpu_info *zeus_detect_one_usb(struct libusb_device *dev, struct usb_find_devices *found)
{
	struct cgpu_info *zeus;
	struct ZEUS_INFO *info;

	zeus = usb_alloc_cgpu(&zeus_drv, 1);
	if (!usb_init(zeus, dev, found))
		goto usbdealloc;
	info = calloc(1, sizeof(struct ZEUS_INFO));
	if (unlikely(!info))
		goto usbdealloc;

	zeus->device_data = info;
	zeus->deven = DEV_ENABLED;
	zeus->threads = 1;

	info->device_fd = -1;
	info->using_libusb = 1;
	zeus->unique_id = zeus->device_path;
	strncpy(info->device_name, zeus->unique_id, sizeof(info->device_name) - 1);
	info->device_name[sizeof(info->device_name) - 1] = '\0';

	zeus_get_device_options(zeus->device_path, &info->chips_count, &info->chip_clk, opt_zeus_options);
	zeus->name = zeus_device_name(info->chips_count);
	info->freqcode = zeus_clk_to_freqcode(info->chip_clk);
	info->baud = ZEUS_IO_SPEED;
	info->cores_per_chip = ZEUS_CHIP_CORES;
	info->chips_count_max = lowest_pow2(info->chips_count);
	info->chips_bit_num = log_2(info->chips_count_max);
	info->next_chip_clk = -1;

	libusb_reset_device(zeus->usbdev->handle);
	update_usb_stats(zeus);

	zeus->usbdev->usb_type = USB_TYPE_STD;
	if (!zeus_initialize_usbuart(zeus)) {
		applog(LOG_ERR, "Failed to initialize Zeus USB-UART interface");
		goto alldealloc;
	}

	info->golden_speed_per_core = (((info->chip_clk * 2.) / 3.) * 1024.) / 8.;
	info->work_timeout.tv_sec = 4294967296LL / (info->golden_speed_per_core * info->cores_per_chip * info->chips_count_max) * 0.9;
	info->work_timeout.tv_usec = 0;
	info->read_count = (uint32_t)((4294967296LL*10)/(info->cores_per_chip*info->chips_count_max*info->golden_speed_per_core*2));
	info->read_count = info->read_count*3/4;

	if (!add_cgpu(zeus))
		goto alldealloc;

	return zeus;

alldealloc:
	usb_uninit(zeus);
	free(zeus->device_data);
	zeus->device_data = NULL;

usbdealloc:
	zeus = usb_free_cgpu(zeus);
	return NULL;
}

static bool zeus_detect_one_serial(const char *devpath)
{
	struct timeval tv_start, tv_finish;
	int i, fd, baud, cores_per_chip, chips_count_max, chips_count, chip_clk;
	//int this_option_offset = ++option_offset;
	unsigned char freqcode_init, freqcode;
	uint32_t nonce;
	uint64_t golden_speed_per_core;

	/* this check here is needed as a failsafe because the serial_detect
	 * functions do not keep track of devices already opened */
	for (i = 0; i < total_devices; ++i) {
		if (devices[i]->device_path && !strcasecmp(devices[i]->device_path, devpath))
			return false;
	}

	uint32_t golden_nonce_val = be32toh(0x268d0300); // 0xd26 = 3366
	unsigned char ob_bin[ZEUS_COMMAND_PKT_LEN], nonce_bin[ZEUS_EVENT_PKT_LEN];

	static const char golden_ob[] =
			"55aa0001"
			"00038000063b0b1b028f32535e900609c15dc49a42b1d8492a6dd4f8f15295c989a1decf584a6aa93be26066d3185f55ef635b5865a7a79b7fa74121a6bb819da416328a9bd2f8cef72794bf02000000";

	static const char golden_ob2[] =
			"55aa00ff"
			"c00278894532091be6f16a5381ad33619dacb9e6a4a6e79956aac97b51112bfb93dc450b8fc765181a344b6244d42d78625f5c39463bbfdc10405ff711dc1222dd065b015ac9c2c66e28da7202000000";

	zeus_get_device_options(devpath, &chips_count, &chip_clk, opt_zeus_options);
	baud = ZEUS_IO_SPEED;				// baud rate is fixed
	cores_per_chip = ZEUS_CHIP_CORES;		// cores/chip also fixed
	chips_count_max = lowest_pow2(chips_count);
	//if (chips_count > opt_zeus_chips_count_max)
	//	opt_zeus_chips_count_max = lowest_pow2(chips_count);
	//chips_count_max = opt_zeus_chips_count_max;

	if (initial_startup_phase)
		applog(LOG_INFO, "Zeus Detect: Attempting to open %s", devpath);

	fd = zeus_serial_open_detect(devpath, baud, true);
	if (unlikely(fd == -1)) {
		if (initial_startup_phase)
			applog(LOG_ERR, "Zeus Detect: Failed to open %s", devpath);
		return false;
	}

	freqcode = zeus_clk_to_freqcode(chip_clk);

	// from 150M step to the high or low speed. we need to add delay and resend to init chip
	if (chip_clk > 150)
		freqcode_init = zeus_clk_to_freqcode(165);
	else
		freqcode_init = zeus_clk_to_freqcode(139);

	flush_uart(fd);

	hex2bin(ob_bin, golden_ob2, sizeof(ob_bin));
	ob_bin[0] = freqcode_init;
	ob_bin[1] = ~freqcode_init;
	ob_bin[2] = 0x00;
	ob_bin[3] = 0x01;
	for (i = 0; i < 2; ++i) {
		zeus_serial_write(fd, ob_bin, sizeof(ob_bin));
		cgsleep_ms(500);	// what is the minimum the miners need/will accept?
		flush_uart(fd);
	}

	hex2bin(ob_bin, golden_ob2, sizeof(ob_bin));
	ob_bin[0] = freqcode;
	ob_bin[1] = ~freqcode;
	ob_bin[2] = 0x00;
	ob_bin[3] = 0x01;
	for (i = 0; i < 2; ++i) {
		zeus_serial_write(fd, ob_bin, sizeof(ob_bin));
		cgsleep_ms(500);
		flush_uart(fd);
	}

	if (!opt_zeus_nocheck_golden) {
		memset(nonce_bin, 0, sizeof(nonce_bin));

		hex2bin(ob_bin, golden_ob, sizeof(ob_bin));
		ob_bin[0] = freqcode;
		ob_bin[1] = ~freqcode;
		ob_bin[2] = 0x00;
		ob_bin[3] = 0x01;

		for (i = 0; i < 2; ++i) {
			zeus_serial_write(fd, ob_bin, sizeof(ob_bin));
			cgtime(&tv_start);
			if (zeus_serial_read(fd, nonce_bin, sizeof(nonce_bin), 25, &tv_finish) == sizeof(nonce_bin))
				break;
		}

		zeus_serial_close(fd);

		memcpy(&nonce, nonce_bin, sizeof(nonce_bin));
		nonce = be32toh(nonce);

		if (nonce != golden_nonce_val) {
			applog(LOG_ERR, "Zeus Detect: "
					"Test failed at %s: got %08x, should be: %08x",
					devpath, nonce, golden_nonce_val);
			return false;
		}

		golden_speed_per_core = (uint64_t)((double)0xd26 / tdiff(&tv_finish, &tv_start));

		if (opt_zeus_debug)
			applog(LOG_INFO, "Test succeeded at %s: got %08x",
					devpath, nonce);
	} else {
		zeus_serial_close(fd);
		golden_speed_per_core = (((chip_clk * 2.) / 3.) * 1024.) / 8.;
	}

	/* We have a real Zeus miner! */
	struct cgpu_info *zeus;
	struct ZEUS_INFO *info;

	zeus = calloc(1, sizeof(struct cgpu_info));
	if (unlikely(!zeus))
		quit(1, "Failed to malloc struct cgpu_info");
	info = calloc(1, sizeof(struct ZEUS_INFO));
	if (unlikely(!info))
		quit(1, "Failed to malloc struct ZEUS_INFO");

	zeus->drv = &zeus_drv;
	zeus->name = zeus_device_name(chips_count);
	zeus->device_path = strdup(devpath);
	zeus->device_data = info;
	zeus->deven = DEV_ENABLED;
	zeus->threads = 1;

	applog(LOG_NOTICE, "Found Zeus at %s, mark as %d",
			devpath, zeus->device_id);

	applog(LOG_INFO, "Zeus: Init: %d baud=%d cores_per_chip=%d chips_count=%d",
			zeus->device_id, baud, cores_per_chip, chips_count);

	info->device_fd = -1;
	info->using_libusb = 0;
	zeus->unique_id = strrchr(zeus->device_path, '/');
	if (zeus->unique_id == NULL)
		zeus->unique_id = zeus->device_path;
	else
		++zeus->unique_id;
	strncpy(info->device_name, zeus->unique_id, sizeof(info->device_name) - 1);
	info->device_name[sizeof(info->device_name) - 1] = '\0';

	info->work_timeout.tv_sec = 4294967296LL / (golden_speed_per_core * cores_per_chip * chips_count_max) * 0.9;
	info->work_timeout.tv_usec = 0;
	info->golden_speed_per_core = golden_speed_per_core;
	info->read_count = (uint32_t)((4294967296LL*10)/(cores_per_chip*chips_count_max*golden_speed_per_core*2));
	info->read_count = info->read_count*3/4;
	info->next_chip_clk = -1;

	info->freqcode = freqcode;

	info->baud = baud;
	info->cores_per_chip = cores_per_chip;
	info->chips_count = chips_count;
	info->chips_count_max = chips_count_max;
	if ((chips_count_max & (chips_count_max - 1)) != 0)
		quit(1, "chips_count_max must be a power of 2");
	info->chip_clk = chip_clk;
	info->chips_bit_num = log_2(chips_count_max);

	if (!add_cgpu(zeus))
		quit(1, "Failed to add_cgpu");

	return true;
}

/************************************************************
 * Host <-> ASIC protocol implementation
 ************************************************************/

static void zeus_purge_work(struct cgpu_info *zeus)
{
	struct ZEUS_INFO *info = zeus->device_data;
	if (info->current_work != NULL) {
		free_work(info->current_work);
		info->current_work = NULL;
	}
}

#define nonce_range_start(cperc, cmax, core, chip) \
	(((0xffffffff / cperc + 1) * core) + ((0x1fffffff / cmax + 1) * chip))
static bool zeus_read_response(struct cgpu_info *zeus)
{
	struct ZEUS_INFO *info = zeus->device_data;
	unsigned char evtpkt[ZEUS_EVENT_PKT_LEN];
	int ret, duration_ms;
	uint32_t nonce, chip, core;
	bool valid;

	if (using_libusb(info)) {	// in libusb mode data comes to us via the inter-thread pipe
		ret = read(info->zm_pipefd[PIPE_R], evtpkt, sizeof(evtpkt));
		if (ret <= 0)
			return false;
	} else {			// in serial mode we read directly
		ret = zeus_serial_read(info->device_fd, evtpkt, sizeof(evtpkt), 1, NULL);
		if (ret <= 0)
			return false;
		flush_uart(info->device_fd);
	}

	memcpy(&nonce, evtpkt, sizeof(evtpkt));
	nonce = be32toh(nonce);

	if (info->current_work == NULL) {	// work was flushed before we read response
		applog(LOG_DEBUG, "%s%d: Received nonce for flushed work",
			zeus->drv->name, zeus->device_id);
		return true;
	}

	valid = submit_nonce(info->thr, info->current_work, nonce);

	//info->hashes_per_ms = (nonce % (0xffffffff / info->cores_per_chip / info->chips_count)) * info->cores_per_chip * info->chips_count / ms_tdiff(&info->workend, &info->workstart);
	//applog(LOG_INFO, "hashes_per_ms: %d", info->hashes_per_ms);

	++info->workdone;

	//chip = chip_index(nonce, info->chips_bit_num);
	core = (nonce & 0xe0000000) >> 29;		// core indicated by 3 highest bits
	chip = (nonce & 0x1ff80000) >> (29 - info->chips_bit_num);
	duration_ms = ms_tdiff(&info->workend, &info->workstart);

	if (chip < ZEUS_MAX_CHIPS && core < ZEUS_CHIP_CORES) {
		++info->nonce_count[chip][core];
		if (!valid)
			++info->error_count[chip][core];

		if (valid && duration_ms > 0) {
			info->hashes_per_ms = (nonce - nonce_range_start(info->cores_per_chip, info->chips_count_max, core, chip)) / duration_ms * info->cores_per_chip * info->chips_count;
			info->last_nonce = nonce;
		}
	} else {
		applog(LOG_INFO, "%s%d: Corrupt nonce message received, cannot determine chip and core",
			zeus->drv->name, zeus->device_id);
	}

	return true;
}

static bool zeus_check_need_work(struct cgpu_info *zeus)
{
	struct ZEUS_INFO *info = zeus->device_data;
	struct thr_info *thr = info->thr;
	struct work *work;
	bool need_work;

	need_work = (info->current_work == NULL);

	if (need_work) {
		work = get_work(thr, thr->id);  // get_work can block, so done outside mutex_lock

		mutex_lock(&info->lock);
		if (info->current_work == NULL) {  // verify still NULL
			work->devflag = false;
			info->current_work = work;
		} else {
			need_work = false;
		}
		mutex_unlock(&info->lock);

		if (!need_work)
			discard_work(work);
	}

	return need_work;
}

static bool zeus_send_work(struct cgpu_info *zeus, struct work *work)
{
	struct ZEUS_INFO *info = zeus->device_data;
	unsigned char cmdpkt[ZEUS_COMMAND_PKT_LEN];
	int ret;

	uint32_t diff = target_diff(work->target);
	uint32_t diffinv = 0xffff/diff;

	applog(LOG_DEBUG, "zeus_send_work: diff=%d diffinv=%04x", diff, diffinv);

	cmdpkt[0] = info->freqcode;
	cmdpkt[1] = ~(info->freqcode);
	cmdpkt[2] = (diffinv & 0xff00) >> 8;
	cmdpkt[3] = (diffinv & 0x00ff);

	memcpy(cmdpkt + 4, work->data, 80);
	rev(cmdpkt + 4, 80);

	if (using_libusb(info)) {	// in libusb mode we send via usb ;)
		if (usb_write(zeus, (char *)cmdpkt, sizeof(cmdpkt), &ret, C_SENDWORK) != LIBUSB_SUCCESS ||
			ret != sizeof(cmdpkt))
			return false;
	} else {			// otherwise direct via serial port
		ret = zeus_serial_write(info->device_fd, cmdpkt, sizeof(cmdpkt));
		if (ret < 0)
			return false;
	}

	return true;
}

static void *zeus_io_thread(void *data)
{
	struct cgpu_info *zeus = (struct cgpu_info *)data;
	struct ZEUS_INFO *info = zeus->device_data;
	char threadname[24];
	struct pollfd pfds[2];
	struct timeval tv_now, tv_spent, tv_rem;
	int retval;
	bool reopen_device = (using_serial(info) && info->device_fd == -1) ? true : false;

	snprintf(threadname, sizeof(threadname), "Zeus/%d", zeus->device_id);
	RenameThread(threadname);
	applog(LOG_INFO, "%s%d: serial I/O thread running, %s",
						zeus->drv->name, zeus->device_id, threadname);

	if (using_libusb(info))	// in libusb mode get nonces from another thread via pipe
		pfds[0].fd = info->zm_pipefd[PIPE_R];
				// in serial mode pfds[0].fd is set in the while loop
	pfds[0].events = POLLIN;
	pfds[0].revents = 0;
	pfds[1].fd = info->wu_pipefd[PIPE_R];
	pfds[1].events = POLLIN;
	pfds[1].revents = 0;

	while (likely(!zeus->shutdown)) {
		mutex_lock(&info->lock);
		if (unlikely(reopen_device)) {
			if (!zeus_reopen(zeus)) {
				applog(LOG_ERR, "Failed to reopen %s%d on %s, shutting down",
					zeus->drv->name, zeus->device_id, zeus->device_path);
				mutex_unlock(&info->lock);
				break;
			}
			zeus_purge_work(zeus);
			reopen_device = false;
		}
		if (using_serial(info))
			pfds[0].fd = info->device_fd;
		mutex_unlock(&info->lock);

		zeus_check_need_work(zeus);

		mutex_lock(&info->lock);
		if (info->current_work && !info->current_work->devflag) {
			/* send task to device */
			if (opt_zeus_debug)
				applog(LOG_INFO, "Sending work");

			if (zeus_send_work(zeus, info->current_work)) {
				info->current_work->devflag = true;
				cgtime(&info->workstart);
				if (info->next_chip_clk != -1) {
					info->chip_clk = info->next_chip_clk;
					info->next_chip_clk = -1;
				}
			} else {
				mutex_unlock(&info->lock);
				applog(LOG_NOTICE, "%s%d: I/O error while sending work, will attempt to reopen device",
					zeus->drv->name, zeus->device_id);
				reopen_device = true;
				continue;
			}
		}
		mutex_unlock(&info->lock);

		cgtime(&tv_now);
		timersub(&tv_now, &info->workstart, &tv_spent);
		timersub(&info->work_timeout, &tv_spent, &tv_rem);

		if (opt_zeus_debug) {
			applog(LOG_DEBUG, "Workstart: %d.%06d", (int)info->workstart.tv_sec, (int)info->workstart.tv_usec);
			applog(LOG_DEBUG, "Spent: %d.%06d", (int)tv_spent.tv_sec, (int)tv_spent.tv_usec);
			applog(LOG_DEBUG, "select timeout: %d.%06d", (int)tv_rem.tv_sec, (int)tv_rem.tv_usec);
		}

		retval = poll(pfds, 2, (tv_rem.tv_sec * 1000) + (tv_rem.tv_usec / 1000));

		if (retval < 0) {				// error
			if (errno == EINTR)
				continue;
			applog(LOG_NOTICE, "%s%d: Error on poll: %s, shutting down",
				zeus->drv->name, zeus->device_id, strerror(errno));
			break;
		} else if (retval > 0) {
			if (pfds[0].revents & (POLLERR | POLLNVAL)) {	// low level I/O error (usually hardware)
				pfds[0].revents = 0;
				if (opt_zeus_debug) {
					if (pfds[0].revents & POLLNVAL)
						applog(LOG_DEBUG, "%s%d: Device file descriptor %d invalid",
						       zeus->drv->name, zeus->device_id, pfds[0].fd);
					else
						applog(LOG_DEBUG, "%s%d: Error on file descriptor %d",
						       zeus->drv->name, zeus->device_id, pfds[0].fd);
				}

				reopen_device = true;
				continue;
			}

			if (pfds[0].revents & POLLIN) {		// event packet
				pfds[0].revents = 0;
				mutex_lock(&info->lock);
				cgtime(&info->workend);
				if (!zeus_read_response(zeus)) {
					applog(LOG_NOTICE, "%s%d: I/O error while reading response, will attempt to reopen device",
						zeus->drv->name, zeus->device_id);
					reopen_device = true;
				}
				mutex_unlock(&info->lock);
			}

			if (pfds[1].revents & POLLIN) {		// miner thread woke us up
				pfds[1].revents = 0;
				if (!flush_fd(info->wu_pipefd[PIPE_R])) {
					// this should never happen, only here in case
					// an internal error causes pipe to be closed
					applog(LOG_ERR, "%s%d: Inter-thread pipe closed, miner thread dead?",
							zeus->drv->name, zeus->device_id);
					break;
				}
			}
		} else {					// timeout
			mutex_lock(&info->lock);
			zeus_purge_work(zeus);			// abandon current work
			mutex_unlock(&info->lock);
		}

		if (opt_zeus_debug)
			applog(LOG_DEBUG, "poll returned %d", retval);
	}

	zeus->shutdown = true;
	return NULL;
}

/************************************************************
 * CGMiner Interface functions
 ************************************************************/

static int zeus_autoscan()
{
	return serial_autodetect_udev(zeus_detect_one_serial, ZEUS_USB_ID_MODEL_STR);
}

static void zeus_detect(bool __maybe_unused hotplug)
{
	static int serial_usb = 0;

	if (initial_startup_phase && hotplug)
		initial_startup_phase = false;

	if (serial_usb == 0)
		serial_usb = (list_empty(&scan_devices)) ? -1 : 1;

	if (serial_usb < 0)
		usb_detect(&zeus_drv, zeus_detect_one_usb);
	else
		serial_detect_auto(&zeus_drv, zeus_detect_one_serial, zeus_autoscan);
}

static bool zeus_prepare(struct thr_info *thr)
{
	struct cgpu_info *zeus = thr->cgpu;
	struct ZEUS_INFO *info = zeus->device_data;

	applog(LOG_NOTICE, "%s%d opened on %s",
			zeus->drv->name, zeus->device_id, zeus->device_path);

	info->thr = thr;
	mutex_init(&info->lock);

	if (pipe(info->wu_pipefd) < 0 || (using_libusb(info) && pipe(info->zm_pipefd) < 0)) {
		applog(LOG_ERR, "zeus_prepare: error on pipe: %s", strerror(errno));
		return false;
	}

	return true;
}

static bool zeus_thread_init(struct thr_info *thr)
{
	struct cgpu_info *zeus = thr->cgpu;
	struct ZEUS_INFO *info = zeus->device_data;

	if (pthread_create(&info->pth_io, NULL, zeus_io_thread, zeus)) {
		applog(LOG_ERR, "%s%d: Failed to create I/O thread",
				zeus->drv->name, zeus->device_id);
		return false;
	}

	return true;
}

#define ZEUS_LIVE_HASHRATE 1
static int64_t zeus_scanwork(struct thr_info *thr)
{
	struct cgpu_info *zeus = thr->cgpu;
	struct ZEUS_INFO *info = zeus->device_data;
	struct timeval old_scanwork_time;
	unsigned char evtpkt[ZEUS_EVENT_PKT_LEN];
	int err, ret;
	double elapsed_s;
	int64_t estimate_hashes;

	/* in libusb mode, we use this opportunity to check if the miner has a
	 * new nonce value for us; if so we relay it to the I/O thread via pipe */
	if (using_libusb(info)) {
		err = usb_read_timeout(zeus, (char *)evtpkt, sizeof(evtpkt), &ret, 250, C_GETRESULTS);
		if (err && err != LIBUSB_ERROR_TIMEOUT) {
			applog(LOG_ERR, "%s%d: USB read error: %s",
				zeus->drv->name, zeus->device_id, libusb_error_name(err));
			close(info->zm_pipefd[PIPE_W]);  // this will cause the I/O thread to shutdown also
		}
		if (ret == sizeof(evtpkt))
			write(info->zm_pipefd[PIPE_W], evtpkt, sizeof(evtpkt));
	} else {
		/* in serial mode we have nothing to so, so sleep for a bit
		 * to prevent this from becoming a busy-loop */
		cgsleep_ms(100);
	}

	mutex_lock(&info->lock);
	old_scanwork_time = info->scanwork_time;
	cgtime(&info->scanwork_time);
	elapsed_s = tdiff(&info->scanwork_time, &old_scanwork_time);
#ifdef ZEUS_LIVE_HASHRATE
	estimate_hashes = elapsed_s * info->hashes_per_ms * 1000;
#else
	estimate_hashes = elapsed_s * info->golden_speed_per_core *
				info->cores_per_chip * info->chips_count;
#endif
	mutex_unlock(&info->lock);

	if (unlikely(estimate_hashes > 0xffffffff))
		estimate_hashes = 0xffffffff;

	return estimate_hashes;
}

#define zeus_update_work zeus_flush_work
static void zeus_flush_work(struct cgpu_info *zeus)
{
	struct ZEUS_INFO *info = zeus->device_data;
	mutex_lock(&info->lock);
	zeus_purge_work(zeus);
	notify_io_thread(zeus);
	mutex_unlock(&info->lock);
	if (opt_zeus_debug)
		applog(LOG_INFO, "zeus_flush_work: Tickling I/O thread");
}

static struct api_data *zeus_api_stats(struct cgpu_info *zeus)
{
	struct ZEUS_INFO *info = zeus->device_data;
	struct api_data *root = NULL;
	static struct timeval tv_now, tv_diff, tv_diff2;
	static double khs_core, khs_chip, khs_board;

	cgtime(&tv_now);
	timersub(&tv_now, &(info->workstart), &tv_diff);
	timersub(&(info->workend), &(info->workstart), &tv_diff2);

	root = api_add_string(root, "Device Name", zeus->unique_id, false);
	khs_core = (double)info->golden_speed_per_core / 1000.;
	khs_chip = (double)info->golden_speed_per_core * (double)info->cores_per_chip / 1000.;
	khs_board = (double)info->golden_speed_per_core * (double)info->cores_per_chip * (double)info->chips_count / 1000.;
	root = api_add_khs(root, "KHS/Core", &khs_core, false);
	root = api_add_khs(root, "KHS/Chip", &khs_chip, false);
	root = api_add_khs(root, "KHS/Board", &khs_board, false);
	root = api_add_int(root, "Frequency", &(info->chip_clk), false);
	root = api_add_int(root, "Cores/Chip", &(info->cores_per_chip), false);
	root = api_add_int(root, "Chips Count", &(info->chips_count), false);
	root = api_add_timeval(root, "Time Spent Current Work", &tv_diff, false);
	root = api_add_timeval(root, "Work Timeout", &(info->work_timeout), false);
	/* It would be nice to report per chip/core nonce and error counts,
	 * but with more powerful miners with > 100 chips each with 8 cores
	 * there is too much information and we'd overflow the api buffer.
	 * Perhaps another api command to query individual chips? */

	/* these values are more for diagnostic and debugging */
	if (opt_zeus_debug) {
		root = api_add_int(root, "chips_count_max", &(info->chips_count_max), false);
		root = api_add_int(root, "chips_bit_num", &(info->chips_bit_num), false);
		root = api_add_uint32(root, "read_count", &(info->read_count), false);

		root = api_add_uint32(root, "hashes_per_ms", &(info->hashes_per_ms), false);
		root = api_add_uint32(root, "last_nonce", &(info->last_nonce), false);
		root = api_add_timeval(root, "last_nonce_time", &tv_diff2, false);
	}

	return root;
}

static void zeus_get_statline_before(char *buf, size_t bufsiz, struct cgpu_info *zeus)
{
	struct ZEUS_INFO *info = zeus->device_data;
	if (zeus->name)
		tailsprintf(buf, bufsiz, "%-12s  %4d MHz  ", zeus->name, info->chip_clk);
	else
		tailsprintf(buf, bufsiz, "%4d chips  %4d MHz  ", info->chips_count, info->chip_clk);
}

static char *zeus_set_device(struct cgpu_info *zeus, char *option, char *setting, char *replybuf)
{
	struct ZEUS_INFO *info = zeus->device_data;
	int val;

	if (strcasecmp(option, "help") == 0) {
		sprintf(replybuf, "freq: range %d-%d, abortwork: true/false",
				ZEUS_CLK_MIN, ZEUS_CLK_MAX);
		return replybuf;
	}

	if (strcasecmp(option, "freq") == 0) {
		if (!setting || !*setting) {
			sprintf(replybuf, "missing freq setting");
			return replybuf;
		}

		val = atoi(setting);
		if (val < ZEUS_CLK_MIN || val > ZEUS_CLK_MAX) {
			sprintf(replybuf, "invalid freq: '%s' valid range %d-%d",
					setting, ZEUS_CLK_MIN, ZEUS_CLK_MAX);
			return replybuf;
		}

		mutex_lock(&info->lock);
		info->next_chip_clk = val;
		info->freqcode = zeus_clk_to_freqcode(val);
		mutex_unlock(&info->lock);
		return NULL;
	}

	if (strcasecmp(option, "abortwork") == 0) {
		if (!setting || !*setting) {
			sprintf(replybuf, "missing true/false");
			return replybuf;
		}

		if (strcasecmp(setting, "true") != 0) {
			sprintf(replybuf, "not aborting current work");
			return replybuf;
		}

		mutex_lock(&info->lock);
		zeus_purge_work(zeus);
		notify_io_thread(zeus);
		mutex_unlock(&info->lock);
		return NULL;
	}

	sprintf(replybuf, "Unknown option: %s", option);
	return replybuf;
}

static void zeus_shutdown(struct thr_info *thr)
{
	struct cgpu_info *zeus = thr->cgpu;
	struct ZEUS_INFO *info = zeus->device_data;

	applog(LOG_NOTICE, "%s%d: Shutting down", zeus->drv->name, zeus->device_id);

	pthread_join(info->pth_io, NULL);
	mutex_destroy(&info->lock);
	close(info->wu_pipefd[PIPE_R]);
	close(info->wu_pipefd[PIPE_W]);

	if (using_libusb(info)) {
		close(info->zm_pipefd[PIPE_R]);
		close(info->zm_pipefd[PIPE_W]);
	}

	if (info->device_fd != -1) {
		zeus_serial_close(info->device_fd);
		info->device_fd = -1;
	}
}

struct device_drv zeus_drv = {
		.drv_id = DRIVER_zeus,
		.dname = "Zeus",
		.name = "ZUS",
		.max_diff = 32768,
		.drv_detect = zeus_detect,
		.thread_prepare = zeus_prepare,
		.thread_init = zeus_thread_init,
		.hash_work = hash_driver_work,
		.scanwork = zeus_scanwork,
		.flush_work = zeus_flush_work,
		//.update_work = zeus_update_work,	// redundant, always seems to be called together with flush_work ??
		.get_api_stats = zeus_api_stats,
		.get_statline_before = zeus_get_statline_before,
		.set_device = zeus_set_device,
		.thread_shutdown = zeus_shutdown,
};
