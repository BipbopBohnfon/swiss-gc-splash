/* 
 * Copyright (c) 2017-2020, Extrems <extrems@extremscorner.org>
 * 
 * This file is part of Swiss.
 * 
 * Swiss is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * Swiss is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * with Swiss.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "../base/common.h"
#include "../base/dolphin/exi.h"
#include "../base/dolphin/os.h"
#include "../base/emulator.h"
#include "bba.h"
#include "globals.h"

static struct {
	void *buffer;
	uint32_t length;
	uint32_t offset;
	bool frag;
} dvd = {0};

OSAlarm bba_alarm = {0};
OSAlarm read_alarm = {0};

void schedule_read(OSTick ticks, bool lock);
void retry_read(void);

#include "tcpip.c"

#define BBA_CMD_IRMASKALL		0x00
#define BBA_CMD_IRMASKNONE		0xF8

#define BBA_NCRA				0x00		/* Network Control Register A, RW */
#define BBA_NCRA_RESET			(1<<0)	/* RESET */
#define BBA_NCRA_ST0			(1<<1)	/* ST0, Start transmit command/status */
#define BBA_NCRA_ST1			(1<<2)	/* ST1,  " */
#define BBA_NCRA_SR				(1<<3)	/* SR, Start Receive */

#define BBA_IR 0x09		/* Interrupt Register, RW, 00h */
#define BBA_IR_FRAGI       (1<<0)	/* FRAGI, Fragment Counter Interrupt */
#define BBA_IR_RI          (1<<1)	/* RI, Receive Interrupt */
#define BBA_IR_TI          (1<<2)	/* TI, Transmit Interrupt */
#define BBA_IR_REI         (1<<3)	/* REI, Receive Error Interrupt */
#define BBA_IR_TEI         (1<<4)	/* TEI, Transmit Error Interrupt */
#define BBA_IR_FIFOEI      (1<<5)	/* FIFOEI, FIFO Error Interrupt */
#define BBA_IR_BUSEI       (1<<6)	/* BUSEI, BUS Error Interrupt */
#define BBA_IR_RBFI        (1<<7)	/* RBFI, RX Buffer Full Interrupt */

#define BBA_RWP  0x16/*+0x17*/	/* Receive Buffer Write Page Pointer Register */
#define BBA_RRP  0x18/*+0x19*/	/* Receive Buffer Read Page Pointer Register */

#define BBA_WRTXFIFOD 0x48/*-0x4b*/	/* Write TX FIFO Data Port Register */

#define BBA_RX_STATUS_BF      (1<<0)
#define BBA_RX_STATUS_CRC     (1<<1)
#define BBA_RX_STATUS_FAE     (1<<2)
#define BBA_RX_STATUS_FO      (1<<3)
#define BBA_RX_STATUS_RW      (1<<4)
#define BBA_RX_STATUS_MF      (1<<5)
#define BBA_RX_STATUS_RF      (1<<6)
#define BBA_RX_STATUS_RERR    (1<<7)

#define BBA_INIT_TLBP	0x00
#define BBA_INIT_BP		0x01
#define BBA_INIT_RHBP	0x0f
#define BBA_INIT_RWP	BBA_INIT_BP
#define BBA_INIT_RRP	BBA_INIT_BP

static void exi_clear_interrupts(int32_t chan, bool exi, bool tc, bool ext)
{
	EXI[chan][0] = (EXI[chan][0] & ~0x80A) | (ext << 11) | (tc << 3) | (exi << 1);
}

static void exi_select(void)
{
	EXI[EXI_CHANNEL_0][0] = (EXI[EXI_CHANNEL_0][0] & 0x405) | ((1 << EXI_DEVICE_2) << 7) | (EXI_SPEED_32MHZ << 4);
}

static void exi_deselect(void)
{
	EXI[EXI_CHANNEL_0][0] &= 0x405;
}

static void exi_imm_write(uint32_t data, uint32_t len)
{
	EXI[EXI_CHANNEL_0][4] = data;
	EXI[EXI_CHANNEL_0][3] = ((len - 1) << 4) | (EXI_WRITE << 2) | 0b01;
	while (EXI[EXI_CHANNEL_0][3] & 0b01);
}

static uint32_t exi_imm_read(uint32_t len)
{
	EXI[EXI_CHANNEL_0][3] = ((len - 1) << 4) | (EXI_READ << 2) | 0b01;
	while (EXI[EXI_CHANNEL_0][3] & 0b01);
	return EXI[EXI_CHANNEL_0][4] >> ((4 - len) * 8);
}

static uint32_t exi_imm_read_write(uint32_t data, uint32_t len)
{
	EXI[EXI_CHANNEL_0][4] = data;
	EXI[EXI_CHANNEL_0][3] = ((len - 1) << 4) | (EXI_READ_WRITE << 2) | 0b01;
	while (EXI[EXI_CHANNEL_0][3] & 0b01);
	return EXI[EXI_CHANNEL_0][4] >> ((4 - len) * 8);
}

static void exi_immex_write(const void *buf, uint32_t len)
{
	do {
		uint32_t xlen = MIN(len, 4);
		exi_imm_write(*(uint32_t *)buf, xlen);
		buf += xlen;
		len -= xlen;
	} while (len);
}

static void exi_dma_read(void *buf, uint32_t len)
{
	EXI[EXI_CHANNEL_0][1] = (uint32_t)buf;
	EXI[EXI_CHANNEL_0][2] = (len + 31) & ~31;
	EXI[EXI_CHANNEL_0][3] = (EXI_READ << 2) | 0b11;
	while (EXI[EXI_CHANNEL_0][3] & 0b01);
}

static uint8_t bba_in8(uint16_t reg)
{
	uint8_t val;

	exi_select();
	exi_imm_write(0x80 << 24 | reg << 8, 4);
	val = exi_imm_read(1);
	exi_deselect();

	return val;
}

static void bba_out8(uint16_t reg, uint8_t val)
{
	exi_select();
	exi_imm_write(0xC0 << 24 | reg << 8, 4);
	exi_imm_write(val << 24, 1);
	exi_deselect();
}

static uint8_t bba_cmd_in8(uint8_t reg)
{
	uint8_t val;

	exi_select();
	val = exi_imm_read_write(0x00 << 24 | reg << 24, 4);
	exi_deselect();

	return val;
}

static void bba_cmd_out8(uint8_t reg, uint8_t val)
{
	exi_select();
	exi_imm_write(0x40 << 24 | reg << 24 | val, 4);
	exi_deselect();
}

static void bba_ins(uint16_t reg, void *val, uint32_t len)
{
	exi_select();
	exi_imm_write(0x80 << 24 | reg << 8, 4);
	exi_dma_read(val, len);
	exi_deselect();
}

static void bba_outs(uint16_t reg, const void *val, uint32_t len)
{
	exi_select();
	exi_imm_write(0xC0 << 24 | reg << 8, 4);
	exi_immex_write(val, len);
	exi_deselect();
}

void bba_transmit(const void *data, size_t size)
{
	while (bba_in8(BBA_NCRA) & (BBA_NCRA_ST0 | BBA_NCRA_ST1));
	bba_outs(BBA_WRTXFIFOD, data, size);
	bba_out8(BBA_NCRA, (bba_in8(BBA_NCRA) & ~BBA_NCRA_ST0) | BBA_NCRA_ST1);
}

void bba_receive_end(bba_page_t page, void *data, size_t size)
{
	uint8_t rrp;

	page = OSCachedToUncached(page);

	while (size) {
		int page_size = MIN(size, sizeof(bba_page_t));

		rrp = bba_in8(BBA_RRP) % BBA_INIT_RHBP + 1;
		bba_out8(BBA_RRP, rrp);
		bba_ins(rrp << 8, page, page_size);

		memcpy(data, page, page_size);
		data += page_size;
		size -= page_size;
	}
}

static bool bba_receive(void)
{
	uint8_t rwp = bba_in8(BBA_RWP);
	uint8_t rrp = bba_in8(BBA_RRP);

	if (rrp != rwp) {
		bba_page_t page;
		bba_header_t *bba = (bba_header_t *)page;
		size_t size = sizeof(bba_page_t);

		dcache_flush_icache_inv(page, size);
		bba_ins(rrp << 8, page, size);

		size = bba->length - sizeof(*bba);

		eth_input(page, (void *)bba->data, size);
		bba_out8(BBA_RRP, rrp = bba->next);
		rwp = bba_in8(BBA_RWP);
	}

	return rrp != rwp;
}

static void bba_interrupt(void)
{
	uint8_t ir = bba_in8(BBA_IR);

	if ((ir & BBA_IR_RI) && bba_receive())
		ir &= ~BBA_IR_RI;

	bba_out8(BBA_IR, ir);
}

static void exi_callback(int32_t chan, uint32_t dev)
{
	void alarm_handler(OSAlarm *alarm, OSContext *context)
	{
		OSUnmaskInterrupts(OS_INTERRUPTMASK_EXI_2_EXI);
	}

	if (EXILock(EXI_CHANNEL_0, EXI_DEVICE_2, exi_callback)) {
		OSCancelAlarm(&bba_alarm);
		OSTick start = OSGetTick();

		uint8_t status = bba_cmd_in8(0x03);
		bba_cmd_out8(0x02, BBA_CMD_IRMASKALL);

		if (status & 0x80) bba_interrupt();

		bba_cmd_out8(0x03, status);
		bba_cmd_out8(0x02, BBA_CMD_IRMASKNONE);

		OSTick end = OSGetTick();
		OSSetAlarm(&bba_alarm, OSDiffTick(end, start), alarm_handler);

		OSMaskInterrupts(OS_INTERRUPTMASK_EXI_2_EXI);
		EXIUnlock(EXI_CHANNEL_0);
	}
}

void exi_interrupt_handler(OSInterrupt interrupt, OSContext *context)
{
	exi_clear_interrupts(EXI_CHANNEL_2, 1, 0, 0);
	exi_callback(EXI_CHANNEL_0, EXI_DEVICE_2);
}

bool exi_probe(int32_t chan)
{
	if (chan == EXI_CHANNEL_2)
		return false;
	if (chan == *VAR_EXI_SLOT)
		return false;

	return true;
}

bool exi_try_lock(int32_t chan, uint32_t dev, EXIControl *exi)
{
	if (!(exi->state & EXI_STATE_LOCKED) || exi->dev != dev)
		return false;
	if (chan == EXI_CHANNEL_0 && dev == EXI_DEVICE_2)
		return false;
	if (chan == *VAR_EXI_SLOT && dev == EXI_DEVICE_0)
		return false;

	return true;
}

void schedule_read(OSTick ticks, bool lock)
{
	OSCancelAlarm(&read_alarm);

	if (!dvd.length) {
		di_complete_transfer();
		return;
	}

	dvd.frag = is_frag_read(dvd.offset, dvd.length);

	if (!dvd.frag)
		fsp_get_file(dvd.offset, dvd.length, lock);
	else
		OSSetAlarm(&read_alarm, ticks, (OSAlarmHandler)trickle_read);
}

void retry_read(void)
{
	fsp_get_file(dvd.offset, dvd.length, true);
}

void perform_read(uint32_t address, uint32_t length, uint32_t offset)
{
	dvd.buffer = OSPhysicalToCached(address);
	dvd.length = length;
	dvd.offset = offset | *_disc2 << 31;

	schedule_read(OSMicrosecondsToTicks(300), true);
}

void trickle_read(void)
{
	if (dvd.length && dvd.frag) {
		OSTick start = OSGetTick();
		int size = read_frag(dvd.buffer, dvd.length, dvd.offset);
		dcache_store(dvd.buffer, size);
		OSTick end = OSGetTick();

		dvd.buffer += size;
		dvd.length -= size;
		dvd.offset += size;

		schedule_read(OSDiffTick(end, start), true);
	}
}

void device_reset(void)
{
	end_read();
}

void change_disc(void)
{
	*_disc2 = !*_disc2;

	(*DI_EMU)[1] &= ~0b001;
	(*DI_EMU)[1] |=  0b100;
	di_update_interrupts();
}
