// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "SPU2/defs.h"
#include "SPU2/spu2.h" // hopefully temporary, until I resolve lClocks depdendency
#include "IopMem.h"

#include "common/Console.h"
#include "common/Error.h"

#include <cstring>
#include <memory>

namespace SPU2Savestate
{
	// Arbitrary ID to identify SPU2 saves.
	static constexpr u32 SAVE_ID = 0x1227521;

	// versioning for saves.
	// Increment this when changes to the savestate system are made.
	static constexpr u32 SAVE_VERSION = 0x000e;

	static void wipe_the_cache()
	{
		memset(pcm_cache_data, 0, pcm_BlockCount * sizeof(PcmCacheEntry));
	}
} // namespace SPU2Savestate

struct SPU2Savestate::DataBlock
{
	u32 spu2id;          // SPU2 state identifier lets ZeroGS/PeopsSPU2 know this isn't their state)
	u8 unkregs[0x10000]; // SPU2 raw register memory
	u8 mem[0x200000];    // SPU2 raw sample memory

	u32 version; // SPU2 version identifier
	V_Core Cores[2];
	V_SPDIF Spdif;
	u16 OutPos;
	u16 InputPos;
	u32 Cycles;
	u64 lClocks;
	int PlayMode;
};

s32 SPU2Savestate::FreezeIt(DataBlock& spud)
{
	spud.spu2id = SAVE_ID;
	spud.version = SAVE_VERSION;

	memcpy(spud.unkregs, spu2regs, sizeof(spud.unkregs));
	memcpy(spud.mem, _spu2mem, sizeof(spud.mem));

	memcpy(spud.Cores, Cores, sizeof(Cores));
	memcpy(&spud.Spdif, &Spdif, sizeof(Spdif));

	// Convert pointers to offsets so we can safely restore them when loading.
	// We use -1 for null, and anything else as an offset from iop memory.
#define FIX_POINTER(x) \
	if (!(x)) \
	{ \
		x = reinterpret_cast<decltype(x)>(-1); \
	} \
	else \
	{ \
		pxAssert(reinterpret_cast<const u8*>((x)) >= iopPhysMem(0) && reinterpret_cast<const u8*>((x)) < iopPhysMem(0x1fffff)); \
		x = reinterpret_cast<decltype(x)>(reinterpret_cast<const u8*>((x)) - iopPhysMem(0)); \
	}

	for (u32 i = 0; i < 2; i++)
	{
		V_Core& core = spud.Cores[i];
		FIX_POINTER(core.DMAPtr);
		FIX_POINTER(core.DMARPtr);
	}

#undef FIX_POINTER

	spud.OutPos = OutPos;
	spud.InputPos = InputPos;
	spud.Cycles = Cycles;
	spud.lClocks = lClocks;
	spud.PlayMode = PlayMode;

	// note: Don't save the cache.  PCSX2 doesn't offer a safe method of predicting
	// the required size of the savestate prior to saving, plus this is just too
	// "implementation specific" for the intended spec of a savestate.  Let's just
	// force the user to rebuild their cache instead.

	return 0;
}

s32 SPU2Savestate::ThawIt(DataBlock& spud)
{
	if (spud.spu2id != SAVE_ID || spud.version < SAVE_VERSION)
	{
		fprintf(stderr, "\n*** SPU2 Warning:\n");
		if (spud.spu2id == SAVE_ID)
			fprintf(stderr, "\tSavestate version is from an older version of PCSX2.\n");
		else
			fprintf(stderr, "\tThe savestate you are trying to load is incorrect or corrupted.\n");

		fprintf(stderr,
				"\tAudio may not recover correctly.  Save your game to memorycard, reset,\n\n"
				"\tand then continue from there.\n\n");

		// Do *not* reset the cores.
		// We'll need some "hints" as to how the cores should be initialized, and the
		// only way to get that is to use the game's existing core settings and hope
		// they kinda match the settings for the savestate (IRQ enables and such).

		// adpcm cache : Clear all the cache flags and buffers.

		wipe_the_cache();
	}
	else
	{
		memcpy(spu2regs, spud.unkregs, sizeof(spud.unkregs));
		memcpy(_spu2mem, spud.mem, sizeof(spud.mem));

		memcpy(Cores, spud.Cores, sizeof(Cores));
		memcpy(&Spdif, &spud.Spdif, sizeof(Spdif));

		// Reverse the pointer offset from above.
#define FIX_POINTER(x) \
	if ((x) == reinterpret_cast<decltype(x)>(-1)) \
	{ \
		x = nullptr; \
	} \
	else \
	{ \
		pxAssert(reinterpret_cast<size_t>((x)) <= 0x1fffff); \
		x = reinterpret_cast<decltype(x)>(iopPhysMem(0) + reinterpret_cast<size_t>((x))); \
	}

		for (u32 i = 0; i < 2; i++)
		{
			V_Core& core = Cores[i];
			FIX_POINTER(core.DMAPtr);
			FIX_POINTER(core.DMARPtr);
		}

#undef FIX_POINTER

		OutPos = spud.OutPos;
		InputPos = spud.InputPos;
		Cycles = spud.Cycles;
		lClocks = spud.lClocks;
		PlayMode = spud.PlayMode;

		wipe_the_cache();

		// Go through the V_Voice structs and recalculate SBuffer pointer from
		// the NextA setting.

		for (int c = 0; c < 2; c++)
		{
			for (int v = 0; v < 24; v++)
			{
				const int cacheIdx = Cores[c].Voices[v].NextA / pcm_WordsPerBlock;
				Cores[c].Voices[v].SBuffer = pcm_cache_data[cacheIdx].Sampledata;
			}
		}
	}
	return 0;
}

s32 SPU2Savestate::SizeIt()
{
	return sizeof(DataBlock);
}

bool SPU2::ValidateFreezeData(std::span<const u8> data, Error* error)
{
	using SPU2Savestate::DataBlock;
	if (data.size() != sizeof(DataBlock))
	{
		Error::SetStringFmt(error, "The SPU2 state holds {} bytes instead of {}.", data.size(), sizeof(DataBlock));
		return false;
	}

	// The block lies unaligned in the state, so it is examined in a copy.
	const std::unique_ptr<DataBlock> block = std::make_unique<DataBlock>();
	std::memcpy(block.get(), data.data(), sizeof(DataBlock));
	if (block->spu2id != SPU2Savestate::SAVE_ID || block->version != SPU2Savestate::SAVE_VERSION)
	{
		Error::SetString(error, "The SPU2 state has another format version.");
		return false;
	}

	// Loading turns the DMA offsets into IOP memory addresses and the voices' next addresses
	// into sample cache entries; each has to lie within its memory. The reverb position indexes
	// its sample buffers before it wraps.
	// IOP memory addressed by SPU2 DMA, and SPU2 sample memory in 16-bit words.
	constexpr u64 iop_memory = 0x200000;
	constexpr u32 sample_words = 0x100000;
	for (u32 index = 0; index < std::size(block->Cores); index++)
	{
		const V_Core& core = block->Cores[index];
		// The index selects the core's own registers and memory areas.
		if (core.Index != index)
		{
			Error::SetString(error, "The SPU2 state holds a core with another index.");
			return false;
		}
		if (core.RevbSampleBufPos >= std::size(core.RevbDownBuf[0]) / 2)
		{
			Error::SetString(error, "The SPU2 state holds a reverb position outside its buffer.");
			return false;
		}
		if (core.ActiveTSA >= sample_words || core.EffectsStartA >= sample_words || core.EffectsEndA >= sample_words)
		{
			Error::SetString(error, "The SPU2 state holds a transfer or effects address outside sample memory.");
			return false;
		}

		// A pending DMA copies ReadSize words, and AutoDMA its remaining input, at the DMA address.
		// The offset is checked on its own and the length against the space left after it, so no
		// sum can wrap; the "no pointer" marker allows any length.
		const auto dma_fits = [](const u16* pointer, u64 words) {
			const uptr offset = reinterpret_cast<uptr>(pointer);
			if (offset == static_cast<uptr>(-1))
				return true;
			return offset < iop_memory && words <= (iop_memory - offset) / 2;
		};
		if (!dma_fits(core.DMAPtr, core.ReadSize) || !dma_fits(core.DMARPtr, core.ReadSize) ||
			!dma_fits(core.DMAPtr, static_cast<u64>(core.InputDataProgress) + core.InputDataLeft))
		{
			Error::SetString(error, "The SPU2 state holds a DMA transfer outside IOP memory.");
			return false;
		}

		for (const V_Voice& voice : core.Voices)
		{
			if (voice.NextA >= sample_words)
			{
				Error::SetString(error, "The SPU2 state holds a voice address outside sample memory.");
				return false;
			}
			// A playing voice reads the sample at its position within an 8-word ADPCM block, past
			// the block header at position 0, which the mixer never leaves it on.
			if (voice.ADSR.Phase != V_ADSR::PHASE_STOPPED && (voice.NextA % pcm_WordsPerBlock) == 0)
			{
				Error::SetString(error, "The SPU2 state holds a playing voice on an ADPCM block header.");
				return false;
			}
			// The envelope phase selects one of its cached phases.
			if (voice.ADSR.Phase >= V_ADSR::ADSR_PHASES)
			{
				Error::SetString(error, "The SPU2 state holds an invalid envelope phase.");
				return false;
			}
		}
	}
	return true;
}

void SPU2::NormalizeFreezeData(std::span<u8> data)
{
	using SPU2Savestate::DataBlock;
	if (data.size() != sizeof(DataBlock))
	{
		Console.ErrorFmt("SPU2: a {}-byte state cannot be normalized", data.size());
		return;
	}

	// The voices' sample buffer pointers are host addresses; loading rebuilds them from NextA.
	const std::unique_ptr<DataBlock> block = std::make_unique<DataBlock>();
	std::memcpy(block.get(), data.data(), sizeof(DataBlock));
	for (V_Core& core : block->Cores)
	{
		for (V_Voice& voice : core.Voices)
			voice.SBuffer = nullptr;
	}
	std::memcpy(data.data(), block.get(), sizeof(DataBlock));
}
