// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "ACSTATE.h"
#include "ACATA.h"
#include "ACATAPI.h"
#include "ACCORE.h"
#include "ACJV.h"
#include "ACRAM.h"
#include "ACUART.h"
#include "DEV9.h"
#include "SIO/Sio.h"

#include "common/Console.h"
#include "common/Error.h"

#include <algorithm>
#include <cstring>
#include <memory>

// Device register kept outside a namespace by its owner.
extern u16 INTR_REG;

namespace
{
	constexpr u32 STATE_MAGIC = 0x53434158; // "XACS"
	constexpr u32 STATE_VERSION = 1;

	// Memory card protocol state that lasts from one command to the next. The command buffer
	// only lives within one transfer, which never spans the point a state is taken at.
	struct CardState
	{
		u8 current_command;
		u8 term;
		u8 good_sector;
		u8 msb;
		u8 lsb;
		u8 flag;
		u32 sector_addr;
		u32 transfer_addr;
		u64 auto_eject_ticks;
	};

	struct AtaState
	{
		u16 registers[12];
		u32 last_read;
		u32 last_write;
		u32 cmd_handled;
		u32 cmd_handledc;
		atapi_packet_t packet;
		s64 lba;
		u32 nsector;
		u32 sectorsize;
		ACATA::PioState pio;
	};

	struct BoardState
	{
		u32 magic;
		u32 version;
		ACRAM::BankState acram_banks[ACRAM_NUM_BANKS];
		ACJV::SavedState jvs;
		u16 accore_interrupts;
		u32 pending_transfer;
		AtaState ata;
		ACATAPI::SavedState atapi;
		u16 uart_registers[7];
		s8 dev9_registers[0x10000];
		u16 dev9_irqcause;
		u16 dev9_irqmask;
		u16 dev9_dma_ctrl;
		u16 dev9_xfr_ctrl;
		u16 dev9_if_ctrl;
		u16 dev9_pio_mode;
		u16 dev9_mdma_mode;
		u16 dev9_udma_mode;
		CardState cards[2][4];
	};

	u16* const ATA_REGISTERS[] = {
		&ACATA::R_DATA, &ACATA::R_FEATURE, &ACATA::R_ERROR, &ACATA::R_NSECTOR, &ACATA::R_SECTOR, &ACATA::R_LCYL,
		&ACATA::R_HCYL, &ACATA::R_SELECT, &ACATA::R_STATUS, &ACATA::R_COMMAND, &ACATA::R_STATUS_ALT, &ACATA::R_CONTROL};
	u16* const UART_REGISTERS[] = {
		&ACUART::IER, &ACUART::LCR, &ACUART::MCR, &ACUART::SCR, &ACUART::DLL, &ACUART::DLH, &ACUART::FCR_SHADOW};

	// An ATA DMA command transfers at most 256 sectors (a sector count of 0 means 256).
	constexpr u64 MAX_ATA_TRANSFER_SECTORS = 256;
	// The sector sizes of a pending transfer: 512-byte HDD sectors or 2048-byte data sectors.
	constexpr u64 MAX_SECTOR_SIZE = 2048;

	// Value-initialized, so padding is zero and equal state gives equal bytes.
	std::unique_ptr<BoardState> NewState()
	{
		return std::make_unique<BoardState>();
	}
} // namespace

bool ACSTATE::CheckSupported(Error* error)
{
	if (ACUART::s_device)
	{
		Error::SetString(error, "Save states are not supported for games with a serial drive-board device.");
		return false;
	}
	return true;
}

u32 ACSTATE::GetSize()
{
	return sizeof(BoardState);
}

void ACSTATE::Save(std::span<u8> out)
{
	const std::unique_ptr<BoardState> state = NewState();
	state->magic = STATE_MAGIC;
	state->version = STATE_VERSION;
	std::memcpy(state->acram_banks, ACRAM::banks, sizeof(state->acram_banks));
	ACJV::SaveState(&state->jvs);
	state->accore_interrupts = INTR_REG;
	state->pending_transfer = static_cast<u32>(ACCORE::DMA::PendTrasnfType);

	AtaState& ata = state->ata;
	for (size_t i = 0; i < std::size(ATA_REGISTERS); i++)
		ata.registers[i] = *ATA_REGISTERS[i];
	ata.last_read = ACATA::last_read;
	ata.last_write = ACATA::last_write;
	ata.cmd_handled = ACATA::cmd_handled;
	ata.cmd_handledc = ACATA::cmd_handledc;
	ata.packet = ACATA::ata_c_packet;
	ata.lba = ACATA::TH::LBA;
	ata.nsector = ACATA::TH::nsector;
	ata.sectorsize = ACATA::TH::sectorsize;
	ACATA::SavePioState(&ata.pio);
	ACATAPI::SaveState(&state->atapi);

	for (size_t i = 0; i < std::size(UART_REGISTERS); i++)
		state->uart_registers[i] = *UART_REGISTERS[i];

	std::memcpy(state->dev9_registers, dev9.dev9R, sizeof(state->dev9_registers));
	state->dev9_irqcause = dev9.irqcause;
	state->dev9_irqmask = dev9.irqmask;
	state->dev9_dma_ctrl = dev9.dma_ctrl;
	state->dev9_xfr_ctrl = dev9.xfr_ctrl;
	state->dev9_if_ctrl = dev9.if_ctrl;
	state->dev9_pio_mode = dev9.pio_mode;
	state->dev9_mdma_mode = dev9.mdma_mode;
	state->dev9_udma_mode = dev9.udma_mode;

	for (u32 port = 0; port < 2; port++)
	{
		for (u32 slot = 0; slot < 4; slot++)
		{
			const _mcd& mcd = mcds[port][slot];
			CardState& card = state->cards[port][slot];
			card.current_command = mcd.currentCommand;
			card.term = mcd.term;
			card.good_sector = mcd.goodSector ? 1 : 0;
			card.msb = mcd.msb;
			card.lsb = mcd.lsb;
			card.flag = mcd.FLAG;
			card.sector_addr = mcd.sectorAddr;
			card.transfer_addr = mcd.transferAddr;
			card.auto_eject_ticks = mcd.autoEjectTicks;
		}
	}

	std::memcpy(out.data(), state.get(), sizeof(BoardState));
}

bool ACSTATE::Validate(std::span<const u8> data, Error* error)
{
	if (data.size() != sizeof(BoardState))
	{
		Error::SetString(error, "The arcade device state has the wrong size.");
		return false;
	}

	const std::unique_ptr<BoardState> state = NewState();
	std::memcpy(state.get(), data.data(), sizeof(BoardState));
	if (state->magic != STATE_MAGIC || state->version != STATE_VERSION)
	{
		Error::SetString(error, "The arcade device state has another format version.");
		return false;
	}

	// A pending DMA transfer reads or writes nsector sectors at lba once the guest starts the DMA;
	// it has to lie within the image. Without a pending transfer the values are not used.
	const AtaState& ata = state->ata;
	bool transfer_valid = state->pending_transfer <= static_cast<u32>(ACCORE::DMA::ATA_WRITE);
	if (transfer_valid && state->pending_transfer != static_cast<u32>(ACCORE::DMA::NONE))
	{
		// The sector size is checked first; the range then compares sector counts, so neither
		// a product nor a sum can wrap.
		const u64 image_size = static_cast<u64>(std::max<s64>(ACATA::TH::IMAGESIZE, 0));
		const bool sector_valid = ata.sectorsize == 512 || ata.sectorsize == MAX_SECTOR_SIZE;
		const u64 image_sectors = sector_valid ? image_size / ata.sectorsize : 0;
		transfer_valid = sector_valid && ata.lba >= 0 && ata.nsector <= MAX_ATA_TRANSFER_SECTORS &&
						 static_cast<u64>(ata.lba) <= image_sectors && ata.nsector <= image_sectors - static_cast<u64>(ata.lba);
	}
	if (!transfer_valid)
	{
		Error::SetStringFmt(error, "The arcade device state holds a pending transfer outside the media (type {}, LBA {}, {} sectors of {} bytes).",
			state->pending_transfer, ata.lba, ata.nsector, ata.sectorsize);
		return false;
	}
	if (!ACATA::ValidatePioState(ata.pio))
	{
		Error::SetStringFmt(error, "The arcade device state holds an ATA PIO transfer longer than its buffer ({} bytes).", ata.pio.length);
		return false;
	}
	if (!ACATAPI::ValidateState(state->atapi))
	{
		Error::SetStringFmt(error, "The arcade device state holds an ATAPI transfer longer than its buffers (PIO {}, write {}, DMA {} bytes).",
			state->atapi.pio_len, state->atapi.pio_write_len, state->atapi.dma_len);
		return false;
	}
	return true;
}

void ACSTATE::Load(std::span<const u8> data)
{
	const std::unique_ptr<BoardState> state = NewState();
	std::memcpy(state.get(), data.data(), sizeof(BoardState));

	std::memcpy(ACRAM::banks, state->acram_banks, sizeof(state->acram_banks));
	ACJV::LoadState(state->jvs);
	INTR_REG = state->accore_interrupts;
	ACCORE::DMA::PendTrasnfType = static_cast<ACCORE::DMA::TT>(state->pending_transfer);

	const AtaState& ata = state->ata;
	for (size_t i = 0; i < std::size(ATA_REGISTERS); i++)
		*ATA_REGISTERS[i] = ata.registers[i];
	ACATA::last_read = ata.last_read;
	ACATA::last_write = ata.last_write;
	ACATA::cmd_handled = ata.cmd_handled;
	ACATA::cmd_handledc = ata.cmd_handledc;
	ACATA::ata_c_packet = ata.packet;
	ACATA::TH::LBA = ata.lba;
	ACATA::TH::nsector = ata.nsector;
	ACATA::TH::sectorsize = ata.sectorsize;
	ACATA::LoadPioState(ata.pio);
	ACATAPI::LoadState(state->atapi);

	for (size_t i = 0; i < std::size(UART_REGISTERS); i++)
		*UART_REGISTERS[i] = state->uart_registers[i];

	std::memcpy(dev9.dev9R, state->dev9_registers, sizeof(state->dev9_registers));
	dev9.irqcause = state->dev9_irqcause;
	dev9.irqmask = state->dev9_irqmask;
	dev9.dma_ctrl = state->dev9_dma_ctrl;
	dev9.xfr_ctrl = state->dev9_xfr_ctrl;
	dev9.if_ctrl = state->dev9_if_ctrl;
	dev9.pio_mode = state->dev9_pio_mode;
	dev9.mdma_mode = state->dev9_mdma_mode;
	dev9.udma_mode = state->dev9_udma_mode;

	for (u32 port = 0; port < 2; port++)
	{
		for (u32 slot = 0; slot < 4; slot++)
		{
			_mcd& mcd = mcds[port][slot];
			const CardState& card = state->cards[port][slot];
			mcd.currentCommand = card.current_command;
			mcd.term = card.term;
			mcd.goodSector = (card.good_sector != 0);
			mcd.msb = card.msb;
			mcd.lsb = card.lsb;
			mcd.FLAG = card.flag;
			mcd.sectorAddr = card.sector_addr;
			mcd.transferAddr = card.transfer_addr;
			mcd.autoEjectTicks = static_cast<size_t>(card.auto_eject_ticks);
		}
	}
}
