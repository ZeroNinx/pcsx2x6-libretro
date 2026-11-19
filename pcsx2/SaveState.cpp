// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Achievements.h"
#include "BuildVersion.h"
#include "CDVD/CDVD.h"
#include "COP0.h"
#include "DEV9/ACSTATE.h"
#include "Gif_Unit.h"
#include "IPU/IPU.h"
#include "Vif_Dynarec.h"
#include "Cache.h"
#include "Config.h"
#include "Counters.h"
#include "DebugTools/Breakpoints.h"
#include "DebugTools/SymbolImporter.h"
#include "Elfheader.h"
#include "GS.h"
#include "GS/GS.h"
#include "Host.h"
#include "MTGS.h"
#include "MTVU.h"
#include "Patch.h"
#include "R3000A.h"
#include "SIO/Multitap/MultitapProtocol.h"
#include "SIO/Pad/Pad.h"
#include "SIO/Sio.h"
#include "SIO/Sio0.h"
#include "SIO/Sio2.h"
#include "SPU2/spu2.h"
#include "SaveState.h"
#include "StateWrapper.h"
#include "USB/USB.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "ps2/BiosTools.h"

#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/ScopedGuard.h"
#include "common/StringUtil.h"
#include "common/ZipHelpers.h"

#include "IconsFontAwesome.h"
#include "fmt/format.h"

#include <csetjmp>
#include <limits>
#include <png.h>

using namespace R5900;

static tlbs s_tlb_backup[std::size(tlb)];

static void PreLoadPrep()
{
	// ensure everything is in sync before we start overwriting stuff.
	if (THREAD_VU1)
		vu1Thread.WaitVU();
	MTGS::WaitGS(false);

	// backup current TLBs, since we're going to overwrite them all
	std::memcpy(s_tlb_backup, tlb, sizeof(s_tlb_backup));

	// clear protected pages, since we don't want to fault loading EE memory
	mmap_ResetBlockTracking();

	VMManager::Internal::ClearCPUExecutionCaches();
}

static void PostLoadPrep()
{
	resetCache();
//	WriteCP0Status(cpuRegs.CP0.n.Status.val);
	for (int i = 0; i < 48; i++)
	{
		if (std::memcmp(&s_tlb_backup[i], &tlb[i], sizeof(tlbs)) != 0)
		{
			UnmapTLB(s_tlb_backup[i], i);
			MapTLB(tlb[i], i);
		}
	}

	if (EmuConfig.Gamefixes.GoemonTlbHack) GoemonPreloadTlb();
	CBreakPoints::SetSkipFirst(BREAKPOINT_EE, 0);
	CBreakPoints::SetSkipFirst(BREAKPOINT_IOP, 0);

	UpdateVSyncRate(true);

	if (VMManager::Internal::HasBootedELF())
		R5900SymbolImporter.OnElfLoadedInMemory();
}

// --------------------------------------------------------------------------------------
//  SaveStateBase  (implementations)
// --------------------------------------------------------------------------------------
SaveStateBase::SaveStateBase(VmStateBuffer& memblock)
	: m_growable(&memblock)
	, m_data(memblock.data())
	, m_size(static_cast<u32>(memblock.size()))
	, m_version(g_SaveVersion)
{
}

SaveStateBase::SaveStateBase(std::span<u8> memblock)
	: m_data(memblock.data())
	, m_size(static_cast<u32>(memblock.size()))
	, m_version(g_SaveVersion)
{
}

bool SaveStateBase::PrepareAccess(int size)
{
	if (m_error)
		return false;

	if (size < 0)
	{
		Console.Error("(SaveStateBase) Invalid access of %d bytes", size);
		m_error = true;
		return false;
	}

	// The position never lies beyond the data, so the remaining space cannot underflow.
	const u32 remaining = GetRemaining();
	if (static_cast<u32>(size) <= remaining)
		return true;

	const u64 end = static_cast<u64>(static_cast<u32>(m_idx)) + static_cast<u32>(size);
	if (IsLoading() || !m_growable || end > std::numeric_limits<u32>::max())
	{
		Console.Error("(SaveStateBase) Access of %d bytes at %d exceeds the %u-byte block", size, m_idx, m_size);
		m_error = true;
		return false;
	}

	m_growable->resize(static_cast<size_t>(end));
	m_data = m_growable->data();
	m_size = static_cast<u32>(end);
	return true;
}

void SaveStateBase::PrepBlock(int size)
{
	PrepareAccess(size);
}

bool SaveStateBase::FreezeTag(const char* src)
{
	if (m_error)
		return false;

	char tagspace[32];
	pxAssertMsg(std::strlen(src) < (sizeof(tagspace) - 1), "Tag name exceeds the allowed length");

	std::memset(tagspace, 0, sizeof(tagspace));
	StringUtil::Strlcpy(tagspace, src, sizeof(tagspace));
	NoteFixedBytes(sizeof(tagspace));
	Freeze(tagspace);

	if (std::strcmp(tagspace, src) != 0)
	{
		Console.Error(fmt::format("Savestate data corruption detected while reading tag: {}", src));
		m_error = true;
		return false;
	}

	return true;
}

bool SaveStateBase::FreezeBios()
{
	if (!FreezeTag("BIOS"))
		return false;

	// Check the BIOS, and issue a warning if the bios for this state
	// doesn't match the bios currently being used (chances are it'll still
	// work fine, but some games are very picky).

	u32 bioscheck = BiosChecksum;
	char biosdesc[256];
	std::memset(biosdesc, 0, sizeof(biosdesc));
	StringUtil::Strlcpy(biosdesc, BiosDescription, sizeof(biosdesc));

	Freeze(bioscheck);
	Freeze(biosdesc);

	if (bioscheck != BiosChecksum)
	{
		Console.Error("\n  Warning: BIOS Version Mismatch, savestate may be unstable!");
		Console.Error(
			"    Current BIOS:   %s (crc=0x%08x)\n"
			"    Savestate BIOS: %s (crc=0x%08x)\n",
			BiosDescription.c_str(), BiosChecksum,
			biosdesc, bioscheck
		);
	}

	return IsOkay();
}

bool SaveStateBase::FreezeInternals(Error* error)
{
	// Print this until the MTVU problem in gifPathFreeze is taken care of (rama)
	if (THREAD_VU1)
		Console.Warning("MTVU speedhack is enabled, saved states may not be stable");

	if (!vmFreeze())
		return false;

	// Second Block - Various CPU Registers and States
	// -----------------------------------------------
	if (!FreezeTag("cpuRegs"))
		return false;

	Freeze(cpuRegs);		// cpu regs + COP0
	Freeze(psxRegs);		// iop regs
	Freeze(fpuRegs);
	Freeze(tlb);			// tlbs
	NoteRange(cachedTlbs, cachedTlbs.count, 0, static_cast<s64>(cachedTlbs.PageMasks.size()));
	Freeze(cachedTlbs);		// cached tlbs
	Freeze(AllowParams1);	//OSDConfig written (Fast Boot)
	Freeze(AllowParams2);

	// Third Block - Cycle Timers and Events
	// -------------------------------------
	if (!FreezeTag("Cycles"))
		return false;

	Freeze(EEsCycle);
	Freeze(EEoCycle);
	Freeze(nextDeltaCounter);
	Freeze(nextStartCounter);
	Freeze(psxNextStartCounter);
	Freeze(psxNextDeltaCounter);

	// Fourth Block - EE-related systems
	// ---------------------------------
	if (!FreezeTag("EE-Subsystems"))
		return false;

	bool okay = rcntFreeze();
	okay = okay && memFreeze(error);
	okay = okay && gsFreeze();
	okay = okay && vuMicroFreeze();
	okay = okay && vuJITFreeze();
	okay = okay && vif0Freeze();
	okay = okay && vif1Freeze();
	okay = okay && sifFreeze();
	okay = okay && ipuFreeze();
	okay = okay && ipuDmaFreeze();
	okay = okay && gifFreeze();
	okay = okay && gifDmaFreeze();
	okay = okay && sprFreeze();
	okay = okay && mtvuFreeze();
	if (!okay)
		return false;

	// Fifth Block - iop-related systems
	// ---------------------------------
	if (!FreezeTag("IOP-Subsystems"))
		return false;

	FreezeMem(iopMem->Sif, sizeof(iopMem->Sif));		// iop's sif memory (not really needed, but oh well)

	okay = okay && psxRcntFreeze();

	// TODO: move all the others over to StateWrapper too...
	if (!okay)
		return false;
	{
		// This is horrible. We need to move the rest over...
		std::optional<StateWrapper::VectorMemoryStream> save_stream;
		std::optional<StateWrapper::ReadOnlyMemoryStream> load_stream;
		if (IsSaving())
			save_stream.emplace();
		else
			load_stream.emplace(GetBlockPtr(), GetRemaining());

		StateWrapper sw(IsSaving() ? static_cast<StateWrapper::IStream*>(&save_stream.value()) :
									 static_cast<StateWrapper::IStream*>(&load_stream.value()),
			IsSaving() ? StateWrapper::Mode::Write : StateWrapper::Mode::Read, g_SaveVersion);

		okay = okay && g_Sio0.DoState(sw);
		okay = okay && g_Sio2.DoState(sw);
		okay = okay && g_MultitapArr.at(0).DoState(sw);
		okay = okay && g_MultitapArr.at(1).DoState(sw);

		if (!okay || !sw.IsGood())
			return false;

		if (IsSaving())
		{
			NoteVariableField(SaveStateVariableField::Stream, save_stream->GetPosition());
			FreezeMem(const_cast<u8*>(save_stream->GetBuffer().data()), save_stream->GetPosition());
		}
		else
		{
			const u32 new_idx = static_cast<u32>(m_idx) + load_stream->GetPosition();
			if (new_idx > m_size)
				return false;

			m_idx = static_cast<int>(new_idx);
		}
	}

	okay = okay && cdrFreeze();
	okay = okay && cdvdFreeze();

	// technically this is HLE BIOS territory, but we don't have enough such stuff
	// to merit an HLE Bios sub-section... yet.
	okay = okay && deci2Freeze();

	okay = okay && InputRecordingFreeze();

	okay = okay && handleFreeze(); //file handles

	return okay;
}


// --------------------------------------------------------------------------------------
//  memSavingState (implementations)
// --------------------------------------------------------------------------------------
// uncompressed to/from memory state saves implementation

memSavingState::memSavingState(VmStateBuffer& save_to)
	: SaveStateBase(save_to)
{
}

memSavingState::memSavingState(std::span<u8> save_to)
	: SaveStateBase(save_to)
{
}

// Saving of state data
void memSavingState::FreezeMem(void* data, int size)
{
	if (!PrepareAccess(size) || size == 0)
		return;

	std::memcpy(m_data + m_idx, data, size);
	m_idx += size;
}

// --------------------------------------------------------------------------------------
//  memLoadingState  (implementations)
// --------------------------------------------------------------------------------------
// Loading only reads the block, so the source is never written through the mutable view.
memLoadingState::memLoadingState(const VmStateBuffer& load_from)
	: SaveStateBase(std::span<u8>(const_cast<u8*>(load_from.data()), load_from.size()))
{
}

memLoadingState::memLoadingState(std::span<const u8> load_from)
	: SaveStateBase(std::span<u8>(const_cast<u8*>(load_from.data()), load_from.size()))
{
}

// Loading of state data from a memory buffer. Reading past the end fails without touching the
// destination.
void memLoadingState::FreezeMem( void* data, int size )
{
	if (!PrepareAccess(size) || size == 0)
		return;

	std::memcpy(data, m_data + m_idx, size);
	m_idx += size;
}

static const char* EntryFilename_StateVersion = "PCSX2x6 Savestate Version.id";
static const char* EntryFilename_Screenshot = "Screenshot.png";
static const char* EntryFilename_InternalStructures = "PCSX2 Internal Structures.dat";
static constexpr u32 STATE_PCSX2_VERSION_SIZE = 32;

struct SysState_Component
{
	const char* name;
	int (*freeze)(FreezeAction, freezeData*);
};

static int SysState_MTGSFreeze(FreezeAction mode, freezeData* fP)
{
	MTGS::FreezeData sstate = { fP, 0 };
	MTGS::Freeze(mode, sstate);
	return sstate.retval;
}

static constexpr SysState_Component SPU2_{ "SPU2", SPU2freeze };
static constexpr SysState_Component GS{ "GS", SysState_MTGSFreeze };

static bool SysState_ComponentFreezeIn(zip_file_t* zf, SysState_Component comp)
{
	if (!zf)
		return true;

	freezeData fP = { 0, nullptr };
	if (comp.freeze(FreezeAction::Size, &fP) != 0)
		fP.size = 0;

	Console.WriteLn("  Loading %s", comp.name);

	std::unique_ptr<u8[]> data;
	if (fP.size > 0)
	{
		data = std::make_unique<u8[]>(fP.size);
		fP.data = data.get();

		if (zip_fread(zf, data.get(), fP.size) != static_cast<zip_int64_t>(fP.size))
		{
			Console.Error(fmt::format("* {}: Failed to decompress save data", comp.name));
			return false;
		}
	}

	if (comp.freeze(FreezeAction::Load, &fP) != 0)
	{
		Console.Error(fmt::format("* {}: Failed to load freeze data", comp.name));
		return false;
	}

	return true;
}

static bool SysState_ComponentFreezeOut(SaveStateBase& writer, SysState_Component comp)
{
	freezeData fP = {};
	if (comp.freeze(FreezeAction::Size, &fP) != 0)
	{
		Console.Error(fmt::format("* {}: Failed to get freeze size", comp.name));
		return false;
	}

	if (fP.size == 0)
		return true;

	const int size = fP.size;
	writer.PrepBlock(size);
	if (!writer.IsOkay())
		return false;

	Console.WriteLn("  Saving %s", comp.name);

	fP.data = writer.GetBlockPtr();
	if (comp.freeze(FreezeAction::Save, &fP) != 0)
	{
		Console.Error(fmt::format("* {}: Failed to save freeze data", comp.name));
		return false;
	}

	writer.CommitBlock(size);
	return true;
}

static bool SysState_ComponentFreezeInNew(zip_file_t* zf, const char* name, bool(*do_state_func)(StateWrapper&))
{
	// TODO: We could decompress on the fly here for a little bit more speed.
	std::vector<u8> data;
	if (zf)
	{
		std::optional<std::vector<u8>> optdata(ReadBinaryFileInZip(zf));
		if (optdata.has_value())
			data = std::move(optdata.value());
	}

	StateWrapper::ReadOnlyMemoryStream stream(data.empty() ? nullptr : data.data(), data.size());
	StateWrapper sw(&stream, StateWrapper::Mode::Read, g_SaveVersion);

	return do_state_func(sw);
}

static bool SysState_ComponentFreezeOutNew(SaveStateBase& writer, const char* name, u32 reserve, bool (*do_state_func)(StateWrapper&))
{
	StateWrapper::VectorMemoryStream stream(reserve);
	StateWrapper sw(&stream, StateWrapper::Mode::Write, g_SaveVersion);

	if (!do_state_func(sw))
		return false;

	const int size = static_cast<int>(stream.GetBuffer().size());
	if (size > 0)
	{
		writer.PrepBlock(size);
		if (!writer.IsOkay())
			return false;
		std::memcpy(writer.GetBlockPtr(), stream.GetBuffer().data(), size);
		writer.CommitBlock(size);
	}

	return true;
}

// The in-memory counterparts read a component from a block of exactly its size.
static bool SysState_ComponentLoadFromMemory(std::span<const u8> data, SysState_Component comp)
{
	freezeData fP = {0, nullptr};
	if (comp.freeze(FreezeAction::Size, &fP) != 0 || static_cast<size_t>(fP.size) != data.size())
	{
		Console.ErrorFmt("* {}: the state holds {} bytes, the component needs {}", comp.name, data.size(), fP.size);
		return false;
	}

	// Loading reads the data only.
	fP.data = const_cast<u8*>(data.data());
	if (comp.freeze(FreezeAction::Load, &fP) != 0)
	{
		Console.Error(fmt::format("* {}: Failed to load freeze data", comp.name));
		return false;
	}

	return true;
}

static bool SysState_ComponentLoadFromMemoryNew(std::span<const u8> data, const char* name, bool (*do_state_func)(StateWrapper&))
{
	StateWrapper::ReadOnlyMemoryStream stream(data.data(), static_cast<u32>(data.size()));
	StateWrapper sw(&stream, StateWrapper::Mode::Read, g_SaveVersion);
	if (!do_state_func(sw) || !sw.IsGood() || stream.GetPosition() != data.size())
	{
		Console.ErrorFmt("* {}: the state does not match the component", name);
		return false;
	}

	return true;
}

static std::optional<u32> SysState_ComponentSize(SysState_Component comp)
{
	freezeData fP = {0, nullptr};
	if (comp.freeze(FreezeAction::Size, &fP) != 0 || fP.size <= 0)
		return std::nullopt;
	return static_cast<u32>(fP.size);
}

// --------------------------------------------------------------------------------------
//  BaseSavestateEntry
// --------------------------------------------------------------------------------------
class BaseSavestateEntry
{
protected:
	BaseSavestateEntry() = default;

public:
	virtual ~BaseSavestateEntry() = default;

	virtual const char* GetFilename() const = 0;
	virtual bool FreezeIn(zip_file_t* zf) const = 0;
	virtual bool FreezeOut(SaveStateBase& writer) const = 0;
	virtual bool IsRequired() const = 0;

	// In-memory states: whether the entry is part of them, its size when it does not depend
	// on the state, and loading it from a block holding exactly its data. A memory saved
	// page-encoded returns its memory.
	virtual bool IsInMemoryState() const { return true; }
	virtual std::span<u8> GetPagedMemory() const { return {}; }
	virtual std::optional<u32> GetFixedSize() const = 0;
	virtual bool LoadFromMemory(std::span<const u8> data) const = 0;
};

class MemorySavestateEntry : public BaseSavestateEntry
{
protected:
	MemorySavestateEntry() {}
	virtual ~MemorySavestateEntry() = default;

public:
	virtual bool FreezeIn(zip_file_t* zf) const;
	virtual bool FreezeOut(SaveStateBase& writer) const;
	virtual bool IsRequired() const { return true; }

	std::optional<u32> GetFixedSize() const override { return GetDataSize(); }
	bool LoadFromMemory(std::span<const u8> data) const override
	{
		if (data.size() != GetDataSize())
		{
			Console.ErrorFmt("* {}: the state holds {} bytes, the entry needs {}", GetFilename(), data.size(), GetDataSize());
			return false;
		}
		std::memcpy(GetDataPtr(), data.data(), data.size());
		return true;
	}

protected:
	virtual u8* GetDataPtr() const = 0;
	virtual u32 GetDataSize() const = 0;
};

bool MemorySavestateEntry::FreezeIn(zip_file_t* zf) const
{
	const u32 expectedSize = GetDataSize();
	const s64 bytesRead = zip_fread(zf, GetDataPtr(), expectedSize);
	if (bytesRead != static_cast<s64>(expectedSize))
	{
		Console.WriteLn(Color_Yellow, " '%s' is incomplete (expected 0x%x bytes, loading only 0x%x bytes)",
			GetFilename(), expectedSize, static_cast<u32>(bytesRead));
	}

	return true;
}

bool MemorySavestateEntry::FreezeOut(SaveStateBase& writer) const
{
	writer.FreezeMem(GetDataPtr(), GetDataSize());
	return writer.IsOkay();
}

// --------------------------------------------------------------------------------------
//  SavestateEntry_* (EmotionMemory, IopMemory, etc)
// --------------------------------------------------------------------------------------
// Implementation Rationale:
//  The address locations of PS2 virtual memory components is fully dynamic, so we need to
//  resolve the pointers at the time they are requested (eeMem, iopMem, etc).  Thusly, we
//  cannot use static struct member initializers -- we need virtual functions that compute
//  and resolve the addresses on-demand instead... --air

class SavestateEntry_EmotionMemory final : public MemorySavestateEntry
{
public:
	~SavestateEntry_EmotionMemory() override = default;

	const char* GetFilename() const override { return "eeMemory.bin"; }
	u8* GetDataPtr() const override { return eeMem->Main; }
	uint GetDataSize() const override { return Ps2MemSize::ExposedRam; }
	std::span<u8> GetPagedMemory() const override { return {GetDataPtr(), GetDataSize()}; }

	virtual bool FreezeIn(zip_file_t* zf) const override
	{
		return MemorySavestateEntry::FreezeIn(zf);
	}
};

class SavestateEntry_IopMemory final : public MemorySavestateEntry
{
public:
	~SavestateEntry_IopMemory() override = default;

	const char* GetFilename() const override { return "iopMemory.bin"; }
	u8* GetDataPtr() const override { return iopMem->Main; }
	uint GetDataSize() const override { return Ps2MemSize::ExposedIopRam; }
	std::span<u8> GetPagedMemory() const override { return {GetDataPtr(), GetDataSize()}; }
};

class SavestateEntry_ACRAM final : public MemorySavestateEntry
{
public:
	~SavestateEntry_ACRAM() override = default;

	const char* GetFilename() const override { return "ACRAM.bin"; }
	u8* GetDataPtr() const override { return iopMem->ACRAM; }
	uint GetDataSize() const override { return NamcoMemSize::ACRAM; }
	std::span<u8> GetPagedMemory() const override { return {GetDataPtr(), GetDataSize()}; }
};

#include "DEV9/ACSRAM.h"
class SavestateEntry_ACSRAM final : public MemorySavestateEntry
{
public:
	~SavestateEntry_ACSRAM() override = default;

	const char* GetFilename() const override { return "ACSRAM.bin"; }
	u8* GetDataPtr() const override { return ACSRAM::buffer; }
	uint GetDataSize() const override { return ACSRAM_MAX_SIZE; }
};

#include "DEV9/ACJV.h"
// lazy hack, if we ever need something else stored beyond coins count, move to `union {struct{...}; u8[]};` style
class SavestateEntry_JVSSTATE final : public MemorySavestateEntry
{
public:
	~SavestateEntry_JVSSTATE() override = default;

	const char* GetFilename() const override { return "jvs_state.bin"; }
	u8* GetDataPtr() const override { return (u8*)ACJV::coin; }
	uint GetDataSize() const override { return sizeof(ACJV::coin); }
};

class SavestateEntry_HwRegs final : public MemorySavestateEntry
{
public:
	~SavestateEntry_HwRegs() override = default;

	const char* GetFilename() const override { return "eeHwRegs.bin"; }
	u8* GetDataPtr() const override { return eeHw; }
	uint GetDataSize() const override { return sizeof(eeHw); }
};

class SavestateEntry_IopHwRegs final : public MemorySavestateEntry
{
public:
	~SavestateEntry_IopHwRegs() = default;

	const char* GetFilename() const override { return "iopHwRegs.bin"; }
	u8* GetDataPtr() const override { return iopHw; }
	uint GetDataSize() const override { return sizeof(iopHw); }
};

class SavestateEntry_Scratchpad final : public MemorySavestateEntry
{
public:
	~SavestateEntry_Scratchpad() = default;

	const char* GetFilename() const override { return "Scratchpad.bin"; }
	u8* GetDataPtr() const override { return eeMem->Scratch; }
	uint GetDataSize() const override { return sizeof(eeMem->Scratch); }
};

class SavestateEntry_VU0mem final : public MemorySavestateEntry
{
public:
	~SavestateEntry_VU0mem() = default;

	const char* GetFilename() const override { return "vu0Memory.bin"; }
	u8* GetDataPtr() const override { return vuRegs[0].Mem; }
	uint GetDataSize() const override { return VU0_MEMSIZE; }
};

class SavestateEntry_VU1mem final : public MemorySavestateEntry
{
public:
	~SavestateEntry_VU1mem() = default;

	const char* GetFilename() const override { return "vu1Memory.bin"; }
	u8* GetDataPtr() const override { return vuRegs[1].Mem; }
	uint GetDataSize() const override { return VU1_MEMSIZE; }
};

class SavestateEntry_VU0prog final : public MemorySavestateEntry
{
public:
	~SavestateEntry_VU0prog() = default;

	const char* GetFilename() const override { return "vu0MicroMem.bin"; }
	u8* GetDataPtr() const override { return vuRegs[0].Micro; }
	uint GetDataSize() const override { return VU0_PROGSIZE; }
};

class SavestateEntry_VU1prog final : public MemorySavestateEntry
{
public:
	~SavestateEntry_VU1prog() = default;

	const char* GetFilename() const override { return "vu1MicroMem.bin"; }
	u8* GetDataPtr() const override { return vuRegs[1].Micro; }
	uint GetDataSize() const override { return VU1_PROGSIZE; }
};

class SavestateEntry_SPU2 final : public BaseSavestateEntry
{
public:
	~SavestateEntry_SPU2() override = default;

	const char* GetFilename() const override { return "SPU2.bin"; }
	bool FreezeIn(zip_file_t* zf) const override { return SysState_ComponentFreezeIn(zf, SPU2_); }
	bool FreezeOut(SaveStateBase& writer) const override { return SysState_ComponentFreezeOut(writer, SPU2_); }
	bool IsRequired() const override { return true; }
	std::optional<u32> GetFixedSize() const override { return SysState_ComponentSize(SPU2_); }
	bool LoadFromMemory(std::span<const u8> data) const override { return SysState_ComponentLoadFromMemory(data, SPU2_); }
};

class SavestateEntry_USB final : public BaseSavestateEntry
{
public:
	~SavestateEntry_USB() override = default;

	const char* GetFilename() const override { return "USB.bin"; }
	bool FreezeIn(zip_file_t* zf) const override { return SysState_ComponentFreezeInNew(zf, "USB", &USB::DoState); }
	bool FreezeOut(SaveStateBase& writer) const override { return SysState_ComponentFreezeOutNew(writer, "USB", 16 * 1024, &USB::DoState); }
	bool IsRequired() const override { return false; }
	// In-memory states require that no USB device is configured.
	bool IsInMemoryState() const override { return false; }
	std::optional<u32> GetFixedSize() const override { return std::nullopt; }
	bool LoadFromMemory(std::span<const u8> data) const override { return false; }
};

class SavestateEntry_PAD final : public BaseSavestateEntry
{
public:
	~SavestateEntry_PAD() override = default;

	const char* GetFilename() const override { return "PAD.bin"; }
	bool FreezeIn(zip_file_t* zf) const override { return SysState_ComponentFreezeInNew(zf, "PAD", &Pad::Freeze); }
	bool FreezeOut(SaveStateBase& writer) const override { return SysState_ComponentFreezeOutNew(writer, "PAD", 16 * 1024, &Pad::Freeze); }
	bool IsRequired() const override { return true; }
	// The size follows the configured pads; it is measured with a save.
	std::optional<u32> GetFixedSize() const override { return std::nullopt; }
	bool LoadFromMemory(std::span<const u8> data) const override { return SysState_ComponentLoadFromMemoryNew(data, "PAD", &Pad::Freeze); }

	// Reads the block into scratch pads, leaving the connected controllers unchanged.
	static bool Validate(std::span<const u8> data, Error* error)
	{
		StateWrapper::ReadOnlyMemoryStream stream(data.data(), static_cast<u32>(data.size()));
		StateWrapper sw(&stream, StateWrapper::Mode::Read, g_SaveVersion);
		if (!Pad::CheckState(sw) || !sw.IsGood() || stream.GetPosition() != data.size())
		{
			Error::SetString(error, "The state's controller block is malformed.");
			return false;
		}
		return true;
	}
};

class SavestateEntry_GS final : public BaseSavestateEntry
{
public:
	~SavestateEntry_GS() = default;

	const char* GetFilename() const { return "GS.bin"; }
	bool FreezeIn(zip_file_t* zf) const { return SysState_ComponentFreezeIn(zf, GS); }
	bool FreezeOut(SaveStateBase& writer) const { return SysState_ComponentFreezeOut(writer, GS); }
	bool IsRequired() const { return true; }
	std::optional<u32> GetFixedSize() const override { return SysState_ComponentSize(GS); }
	bool LoadFromMemory(std::span<const u8> data) const override { return SysState_ComponentLoadFromMemory(data, GS); }
};

class SaveStateEntry_Achievements final : public BaseSavestateEntry
{
	~SaveStateEntry_Achievements() override = default;

	const char* GetFilename() const override { return "Achievements.bin"; }
	bool FreezeIn(zip_file_t* zf) const override
	{
		if (!Achievements::IsActive())
			return true;

		std::optional<std::vector<u8>> data;
		if (zf)
			data = ReadBinaryFileInZip(zf);

		if (data.has_value())
			Achievements::LoadState(data.value());
		else
			Achievements::LoadState(std::span<const u8>());

		return true;
	}

	bool FreezeOut(SaveStateBase& writer) const override
	{
		if (!Achievements::IsActive())
			return true;

		Achievements::SaveState(writer);
		return writer.IsOkay();
	}

	bool IsRequired() const override { return false; }
	// In-memory states require achievements to be inactive.
	bool IsInMemoryState() const override { return false; }
	std::optional<u32> GetFixedSize() const override { return std::nullopt; }
	bool LoadFromMemory(std::span<const u8> data) const override { return false; }
};

// (cpuRegs, iopRegs, VPU/GIF/DMAC structures should all remain as part of a larger unified
//  block, since they're all PCSX2-dependent and having separate files in the archie for them
//  would not be useful).
//

static const std::unique_ptr<BaseSavestateEntry> SavestateEntries[] = {
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_EmotionMemory),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_IopMemory),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_ACRAM),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_ACSRAM),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_JVSSTATE),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_HwRegs),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_IopHwRegs),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_Scratchpad),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU0mem),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU1mem),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU0prog),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU1prog),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_SPU2),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_USB),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_PAD),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_GS),
	std::unique_ptr<BaseSavestateEntry>(new SaveStateEntry_Achievements),
};

std::unique_ptr<ArchiveEntryList> SaveState_DownloadState(Error* error)
{
	std::unique_ptr<ArchiveEntryList> destlist = std::make_unique<ArchiveEntryList>();
	destlist->GetBuffer().resize(1024 * 1024 * 64);

	memSavingState saveme(destlist->GetBuffer());
	ArchiveEntry internals(EntryFilename_InternalStructures);
	internals.SetDataIndex(saveme.GetCurrentPos());

	if (!saveme.FreezeBios())
	{
		Error::SetString(error, "FreezeBios() failed");
		return nullptr;
	}

	if (!saveme.FreezeInternals(error))
	{
		if (!error->IsValid())
			Error::SetString(error, "FreezeInternals() failed");

		return nullptr;
	}

	internals.SetDataSize(saveme.GetCurrentPos() - internals.GetDataIndex());
	destlist->Add(internals);

	for (const std::unique_ptr<BaseSavestateEntry>& entry : SavestateEntries)
	{
		uint startpos = saveme.GetCurrentPos();
		if (!entry->FreezeOut(saveme))
		{
			Error::SetString(error, fmt::format("FreezeOut() failed for {}.", entry->GetFilename()));
			destlist.reset();
			break;
		}

		destlist->Add(
			ArchiveEntry(entry->GetFilename())
				.SetDataIndex(startpos)
				.SetDataSize(saveme.GetCurrentPos() - startpos));
	}

	return destlist;
}

std::unique_ptr<SaveStateScreenshotData> SaveState_SaveScreenshot()
{
	static constexpr u32 SCREENSHOT_WIDTH = 640;
	static constexpr u32 SCREENSHOT_HEIGHT = 480;

	u32 width, height;
	std::vector<u32> pixels;
	if (!MTGS::SaveMemorySnapshot(SCREENSHOT_WIDTH, SCREENSHOT_HEIGHT, true, false, &width, &height, &pixels))
	{
		// saving failed for some reason, device lost?
		return nullptr;
	}

	std::unique_ptr<SaveStateScreenshotData> data = std::make_unique<SaveStateScreenshotData>();
	data->width = width;
	data->height = height;
	data->pixels = std::move(pixels);
	return data;
}

static bool SaveState_CompressScreenshot(SaveStateScreenshotData* data, zip_t* zf)
{
	zip_error_t ze = {};
	zip_source_t* const zs = zip_source_buffer_create(nullptr, 0, 0, &ze);
	if (!zs)
		return false;

	if (zip_source_begin_write(zs) != 0)
	{
		zip_source_free(zs);
		return false;
	}

	ScopedGuard zs_free([zs]() { zip_source_free(zs); });

	png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	png_infop info_ptr = nullptr;
	if (!png_ptr)
		return false;

	ScopedGuard cleanup([&png_ptr, &info_ptr]() {
		if (png_ptr)
			png_destroy_write_struct(&png_ptr, info_ptr ? &info_ptr : nullptr);
	});

	info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
		return false;

	if (setjmp(png_jmpbuf(png_ptr)))
		return false;

	png_set_write_fn(png_ptr, zs, [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
		zip_source_write(static_cast<zip_source_t*>(png_get_io_ptr(png_ptr)), data_ptr, size);
	}, [](png_structp png_ptr) {});
	png_set_compression_level(png_ptr, 5);
	png_set_IHDR(png_ptr, info_ptr, data->width, data->height, 8, PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png_ptr, info_ptr);

	for (u32 y = 0; y < data->height; ++y)
	{
		// ensure the alpha channel is set to opaque
		u32* row = &data->pixels[y * data->width];
		for (u32 x = 0; x < data->width; x++)
			row[x] |= 0xFF000000u;

		png_write_row(png_ptr, reinterpret_cast<png_bytep>(row));
	}

	png_write_end(png_ptr, nullptr);

	if (zip_source_commit_write(zs) != 0)
		return false;

	const s64 file_index = zip_file_add(zf, EntryFilename_Screenshot, zs, 0);
	if (file_index < 0)
		return false;

	// png is already compressed, no point doing it twice
	zip_set_file_compression(zf, file_index, ZIP_CM_STORE, 0);

	// source is now owned by the zip file for later compression
	zs_free.Cancel();
	return true;
}

static bool SaveState_ReadScreenshot(zip_t* zf, u32* out_width, u32* out_height, std::vector<u32>* out_pixels)
{
	auto zff = zip_fopen_managed(zf, EntryFilename_Screenshot, 0);
	if (!zff)
		return false;

	png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	if (!png_ptr)
		return false;

	png_infop info_ptr = png_create_info_struct(png_ptr);
	if (!info_ptr)
	{
		png_destroy_read_struct(&png_ptr, nullptr, nullptr);
		return false;
	}

	ScopedGuard cleanup([&png_ptr, &info_ptr]() {
		png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
	});

	if (setjmp(png_jmpbuf(png_ptr)))
		return false;

	png_set_read_fn(png_ptr, zff.get(), [](png_structp png_ptr, png_bytep data_ptr, png_size_t size) {
		zip_fread(static_cast<zip_file_t*>(png_get_io_ptr(png_ptr)), data_ptr, size);
	});

	png_read_info(png_ptr, info_ptr);

	png_uint_32 width = 0;
	png_uint_32 height = 0;
	int bitDepth = 0;
	int colorType = -1;
	if (png_get_IHDR(png_ptr, info_ptr, &width, &height, &bitDepth, &colorType, nullptr, nullptr, nullptr) != 1 ||
		width == 0 || height == 0)
	{
		return false;
	}

	const png_uint_32 bytesPerRow = png_get_rowbytes(png_ptr, info_ptr);
	std::vector<u8> rowData(bytesPerRow);

	*out_width = width;
	*out_height = height;
	out_pixels->resize(width * height);

	for (u32 y = 0; y < height; y++)
	{
		png_read_row(png_ptr, static_cast<png_bytep>(rowData.data()), nullptr);

		const u8* row_ptr = rowData.data();
		u32* out_ptr = &out_pixels->at(y * width);
		if (colorType == PNG_COLOR_TYPE_RGB)
		{
			for (u32 x = 0; x < width; x++)
			{
				u32 pixel = static_cast<u32>(*(row_ptr)++);
				pixel |= static_cast<u32>(*(row_ptr)++) << 8;
				pixel |= static_cast<u32>(*(row_ptr)++) << 16;
				pixel |= static_cast<u32>(*(row_ptr)++) << 24;
				*(out_ptr++) = pixel | 0xFF000000u; // make opaque
			}
		}
		else if (colorType == PNG_COLOR_TYPE_RGBA)
		{
			for (u32 x = 0; x < width; x++)
			{
				u32 pixel;
				std::memcpy(&pixel, row_ptr, sizeof(u32));
				row_ptr += sizeof(u32);
				*(out_ptr++) = pixel | 0xFF000000u; // make opaque
			}
		}
	}

	return true;
}

// --------------------------------------------------------------------------------------
//  CompressThread_VmState
// --------------------------------------------------------------------------------------
static bool SaveState_AddToZip(zip_t* zf, ArchiveEntryList* srclist, SaveStateScreenshotData* screenshot)
{
	u32 compression = ZIP_CM_DEFAULT;
	u32 compression_level = 0;

	if (EmuConfig.Savestate.CompressionType == SavestateCompressionMethod::Zstandard)
	{
		compression = ZIP_CM_ZSTD;

		if (EmuConfig.Savestate.CompressionRatio == SavestateCompressionLevel::Low)
			compression_level = 1;
		else if (EmuConfig.Savestate.CompressionRatio == SavestateCompressionLevel::Medium)
			compression_level = 3;
		else if (EmuConfig.Savestate.CompressionRatio == SavestateCompressionLevel::High)
			compression_level = 10;
		else if (EmuConfig.Savestate.CompressionRatio == SavestateCompressionLevel::VeryHigh)
			compression_level = 22;
	}
	else if (EmuConfig.Savestate.CompressionType == SavestateCompressionMethod::Deflate)
	{
		compression = ZIP_CM_DEFLATE;
		if (EmuConfig.Savestate.CompressionRatio == SavestateCompressionLevel::Low)
			compression_level = 1;
		else if (EmuConfig.Savestate.CompressionRatio == SavestateCompressionLevel::Medium)
			compression_level = 5;
		else if (EmuConfig.Savestate.CompressionRatio == SavestateCompressionLevel::High)
			compression_level = 7;
		else if (EmuConfig.Savestate.CompressionRatio == SavestateCompressionLevel::VeryHigh)
			compression_level = 9;
	}
	else if (EmuConfig.Savestate.CompressionType == SavestateCompressionMethod::Uncompressed)
	{
		compression = ZIP_CM_STORE;
		compression_level = 0;
	}

	// version indicator
	{
		struct VersionIndicator
		{
			u32 save_version;
			char version[STATE_PCSX2_VERSION_SIZE];
		};

		VersionIndicator* vi = static_cast<VersionIndicator*>(std::malloc(sizeof(VersionIndicator)));
		vi->save_version = g_SaveVersion;
		if (BuildVersion::GitTaggedCommit)
		{
			StringUtil::Strlcpy(vi->version, BuildVersion::GitTag, std::size(vi->version));
		}
		else
		{
			StringUtil::Strlcpy(vi->version, "Unknown", std::size(vi->version));
		}

		zip_source_t* const zs = zip_source_buffer(zf, vi, sizeof(*vi), 1);
		if (!zs)
		{
			std::free(vi);
			return false;
		}

		// NOTE: Source should not be freed if successful.
		const s64 fi = zip_file_add(zf, EntryFilename_StateVersion, zs, ZIP_FL_ENC_UTF_8);
		if (fi < 0)
		{
			zip_source_free(zs);
			return false;
		}

		// Don't compress the version indicator file so that builds that don't
		// support a given compression method can at least still read it.
		zip_set_file_compression(zf, fi, ZIP_CM_STORE, 0);
	}

	const uint listlen = srclist->GetLength();
	for (uint i = 0; i < listlen; ++i)
	{
		const ArchiveEntry& entry = (*srclist)[i];
		if (!entry.GetDataSize())
			continue;

		zip_source_t* const zs = zip_source_buffer(zf, srclist->GetPtr(entry.GetDataIndex()), entry.GetDataSize(), 0);
		if (!zs)
			return false;

		const s64 fi = zip_file_add(zf, entry.GetFilename().c_str(), zs, ZIP_FL_ENC_UTF_8);
		if (fi < 0)
		{
			zip_source_free(zs);
			return false;
		}

		zip_set_file_compression(zf, fi, compression, compression_level);
	}

	if (screenshot)
	{
		if (!SaveState_CompressScreenshot(screenshot, zf))
			return false;
	}

	return true;
}

bool SaveState_ZipToDisk(
	std::unique_ptr<ArchiveEntryList> srclist, std::unique_ptr<SaveStateScreenshotData> screenshot,
	const char* filename, Error* error)
{
	zip_error_t ze = {};
	zip_source_t* zs = zip_source_file_create(filename, 0, 0, &ze);
	zip_t* zf = nullptr;
	if (zs && !(zf = zip_open_from_source(zs, ZIP_CREATE | ZIP_TRUNCATE, &ze)))
	{
		Error::SetStringFmt(error,
			TRANSLATE_FS("SaveState", "Failed to open zip file '{}' for save state: {}."),
			filename, zip_error_strerror(&ze));

		// have to clean up source
		zip_source_free(zs);
		return false;
	}

	// discard zip file if we fail saving something
	if (!SaveState_AddToZip(zf, srclist.get(), screenshot.get()))
	{
		Error::SetStringFmt(error,
			TRANSLATE_FS("SaveState", "Failed to save state to zip file '{}'."), filename);
		zip_discard(zf);
		return false;
	}

	// force the zip to close, this is the expensive part with libzip.
	if (zip_close(zf) != 0)
	{
		Error::SetStringFmt(error,
			TRANSLATE_FS("SaveState", "Failed to save state to zip file '{}': {}."), filename, zip_strerror(zf));
		zip_discard(zf);
		return false;
	}

	return true;
}

bool SaveState_ReadScreenshot(const std::string& filename, u32* out_width, u32* out_height, std::vector<u32>* out_pixels)
{
	zip_error_t ze = {};
	auto zf = zip_open_managed(filename.c_str(), ZIP_RDONLY, &ze);
	if (!zf)
	{
		Console.Error("Failed to open zip file '%s' for save state screenshot: %s", filename.c_str(), zip_error_strerror(&ze));
		return false;
	}

	return SaveState_ReadScreenshot(zf.get(), out_width, out_height, out_pixels);
}

static bool CheckVersion(const std::string& filename, zip_t* zf, Error* error)
{
	u32 savever;

	auto zff = zip_fopen_managed(zf, EntryFilename_StateVersion, 0);
	if (!zff || zip_fread(zff.get(), &savever, sizeof(savever)) != sizeof(savever))
	{
		Error::SetString(error, "Savestate file does not contain version indicator.");
		return false;
	}

	char version_string[STATE_PCSX2_VERSION_SIZE];
	if (zip_fread(zff.get(), version_string, STATE_PCSX2_VERSION_SIZE) == STATE_PCSX2_VERSION_SIZE)
		version_string[STATE_PCSX2_VERSION_SIZE - 1] = 0;
	else
		StringUtil::Strlcpy(version_string, "Unknown", std::size(version_string));

	// Major version mismatch.  Means we can't load this savestate at all.  Support for it
	// was removed entirely.
	// check for a "minor" version incompatibility; which happens if the savestate being loaded is a newer version
	// than the emulator recognizes.  99% chance that trying to load it will just corrupt emulation or crash.
	if (savever > g_SaveVersion || (savever >> 16) != (g_SaveVersion >> 16))
	{
		std::string current_emulator_version = BuildVersion::GitTag;
		if (current_emulator_version.empty())
		{
			current_emulator_version = "Unknown";
		}
		Error::SetString(error, fmt::format(TRANSLATE_FS("SaveState","This save state was created with PCSX2 version {0}. It is no longer compatible "
											"with your current PCSX2 version {1}.\n\n"
											"If you have any unsaved progress on this save state, you can download the compatible PCSX2 version {0} "
											"from pcsx2.net, load the save state, and save your progress to the memory card."),
											version_string, current_emulator_version));
		return false;
	}

	return true;
}

static zip_int64_t CheckFileExistsInState(zip_t* zf, const char* name, bool required)
{
	zip_int64_t index = zip_name_locate(zf, name, /*ZIP_FL_NOCASE*/ 0);
	if (index >= 0)
	{
		DevCon.WriteLn(Color_Green, " ... found '%s'", name);
		return index;
	}

	if (required)
		Console.WriteLn(Color_Red, " ... not found '%s'!", name);
	else
		DevCon.WriteLn(Color_Red, " ... not found '%s'!", name);

	return index;
}

static bool LoadInternalStructuresState(zip_t* zf, s64 index, Error* error)
{
	zip_stat_t zst;
	if (zip_stat_index(zf, index, 0, &zst) != 0 || zst.size > std::numeric_limits<int>::max())
		return false;

	// Load all the internal data
	auto zff = zip_fopen_index_managed(zf, index, 0);
	if (!zff)
		return false;

	std::vector<u8> buffer(zst.size);
	if (zip_fread(zff.get(), buffer.data(), buffer.size()) != static_cast<zip_int64_t>(buffer.size()))
		return false;

	memLoadingState state(buffer);
	if (!state.FreezeBios())
		return false;
	
	if (!state.FreezeInternals(error))
		return false;

	return true;
}

bool SaveState_UnzipFromDisk(const std::string& filename, Error* error)
{
	zip_error_t ze = {};
	auto zf = zip_open_managed(filename.c_str(), ZIP_RDONLY, &ze);
	if (!zf)
	{
		Console.Error("Failed to open zip file '%s' for save state load: %s", filename.c_str(), zip_error_strerror(&ze));
		if (zip_error_code_zip(&ze) == ZIP_ER_NOENT)
			Error::SetString(error, "Savestate file does not exist.");
		else
			Error::SetString(error, fmt::format("Savestate zip error: {}", zip_error_strerror(&ze)));

		return false;
	}

	// look for version and screenshot information in the zip stream:
	if (!CheckVersion(filename, zf.get(), error))
		return false;

	// check that all parts are included
	const s64 internal_index = CheckFileExistsInState(zf.get(), EntryFilename_InternalStructures, true);
	s64 entryIndices[std::size(SavestateEntries)];

	// Log any parts and pieces that are missing, and then generate an exception.
	bool allPresent = (internal_index >= 0);
	for (u32 i = 0; i < std::size(SavestateEntries); i++)
	{
		const bool required = SavestateEntries[i]->IsRequired();
		entryIndices[i] = CheckFileExistsInState(zf.get(), SavestateEntries[i]->GetFilename(), required);
		if (entryIndices[i] < 0 && required)
		{
			allPresent = false;
			break;
		}
	}
	if (!allPresent)
	{
		Error::SetString(error, "Some required components were not found or are incomplete.");
		return false;
	}

	PreLoadPrep();

	if (!LoadInternalStructuresState(zf.get(), internal_index, error))
	{
		if (!error->IsValid())
			Error::SetString(error, "Save state corruption in internal structures.");

		VMManager::Reset();
		return false;
	}

	for (u32 i = 0; i < std::size(SavestateEntries); ++i)
	{
		if (entryIndices[i] < 0)
		{
			SavestateEntries[i]->FreezeIn(nullptr);
			continue;
		}

		auto zff = zip_fopen_index_managed(zf.get(), entryIndices[i], 0);
		if (!zff || !SavestateEntries[i]->FreezeIn(zff.get()))
		{
			Error::SetString(error, fmt::format("Save state corruption in {}.", SavestateEntries[i]->GetFilename()));
			VMManager::Reset();
			return false;
		}
	}

	PostLoadPrep();
	return true;
}

// --------------------------------------------------------------------------------------
//  In-memory states in caller-owned buffers
// --------------------------------------------------------------------------------------

namespace
{
	enum class SegmentKind
	{
		Internals,
		Layout,
		Entry,
		ArcadeDevices,
		AudioOutput,
	};

	struct SegmentSource
	{
		SegmentKind kind;
		const BaseSavestateEntry* entry;
	};

	// Upper bounds of the variable fields of the internal structures: the only string is the
	// booted ELF path, and the SIO2 FIFOs never hold more than a few commands.
	constexpr u32 MAX_STATE_STRING = 4096;
	constexpr u32 MAX_SIO2_FIFO = 32 * 1024;
	constexpr u32 MAX_LAYOUT_RECORDS = 64;

	// The variable fields of the internals segment saved last; the layout segment holds them.
	std::vector<SaveStateFieldRecord> s_saved_layout;

	// Page encoding of a memory: header, bitmap of non-zero pages, then those pages in order.
	constexpr u32 PAGE_ENCODING_VERSION = 1;
	constexpr u32 STATE_PAGE_SIZE = 4096;

	struct PagedHeader
	{
		u32 version;
		u32 page_size;
		u32 page_count;
		u32 present_count;
	};

	u32 GetBitmapWords(u32 page_count)
	{
		return (page_count + 63) / 64;
	}

	u64 GetPagedLength(u32 page_count, u32 present_count)
	{
		return sizeof(PagedHeader) + static_cast<u64>(GetBitmapWords(page_count)) * sizeof(u64) +
			   static_cast<u64>(present_count) * STATE_PAGE_SIZE;
	}

	bool IsZeroPage(const u8* page)
	{
		u64 bits = 0;
		for (u32 i = 0; i < STATE_PAGE_SIZE; i += sizeof(u64))
		{
			u64 word;
			std::memcpy(&word, page + i, sizeof(word));
			bits |= word;
		}
		return bits == 0;
	}

	void BuildPageBitmap(std::span<const u8> memory, std::vector<u64>* bitmap, u32* present)
	{
		const u32 page_count = static_cast<u32>(memory.size() / STATE_PAGE_SIZE);
		bitmap->assign(GetBitmapWords(page_count), 0);
		*present = 0;
		for (u32 page = 0; page < page_count; page++)
		{
			if (!IsZeroPage(memory.data() + static_cast<size_t>(page) * STATE_PAGE_SIZE))
			{
				(*bitmap)[page / 64] |= u64{1} << (page % 64);
				(*present)++;
			}
		}
	}

	void SavePaged(std::span<const u8> memory, const std::vector<u64>& bitmap, u32 present, std::span<u8> out)
	{
		const u32 page_count = static_cast<u32>(memory.size() / STATE_PAGE_SIZE);
		const PagedHeader header = {PAGE_ENCODING_VERSION, STATE_PAGE_SIZE, page_count, present};
		std::memcpy(out.data(), &header, sizeof(header));
		std::memcpy(out.data() + sizeof(header), bitmap.data(), bitmap.size() * sizeof(u64));
		u8* next = out.data() + sizeof(header) + bitmap.size() * sizeof(u64);
		for (u32 page = 0; page < page_count; page++)
		{
			if (bitmap[page / 64] & (u64{1} << (page % 64)))
			{
				std::memcpy(next, memory.data() + static_cast<size_t>(page) * STATE_PAGE_SIZE, STATE_PAGE_SIZE);
				next += STATE_PAGE_SIZE;
			}
		}
	}

	// The header, the bitmap and the length: the version and page size of this format, the page
	// count of the running VM's memory, as many present pages as bitmap bits, no bits beyond the
	// last page, and exactly as many bytes as the present pages need.
	bool ValidatePaged(std::span<const u8> data, size_t memory_size, const char* name, Error* error)
	{
		const u32 page_count = static_cast<u32>(memory_size / STATE_PAGE_SIZE);
		const u64 bitmap_bytes = static_cast<u64>(GetBitmapWords(page_count)) * sizeof(u64);
		PagedHeader header = {};
		if (data.size() >= sizeof(header))
			std::memcpy(&header, data.data(), sizeof(header));
		if (data.size() < sizeof(header) + bitmap_bytes || header.version != PAGE_ENCODING_VERSION ||
			header.page_size != STATE_PAGE_SIZE || header.page_count != page_count || header.present_count > page_count)
		{
			Error::SetStringFmt(error, "The state's {} segment has an invalid page header.", name);
			return false;
		}

		u64 present = 0;
		for (u32 word = 0; word < GetBitmapWords(page_count); word++)
		{
			u64 bits;
			std::memcpy(&bits, data.data() + sizeof(header) + word * sizeof(u64), sizeof(bits));
			const u32 valid_bits = std::min<u32>(64, page_count - word * 64);
			if (valid_bits < 64 && (bits >> valid_bits) != 0)
			{
				Error::SetStringFmt(error, "The state's {} segment marks pages beyond its memory.", name);
				return false;
			}
			present += static_cast<u64>(std::popcount(bits));
		}
		if (present != header.present_count || data.size() != GetPagedLength(page_count, header.present_count))
		{
			Error::SetStringFmt(error, "The state's {} segment holds {} bytes for {} marked pages.", name, data.size(), present);
			return false;
		}
		return true;
	}

	// Loads a validated page-encoded memory; pages without data are cleared.
	void LoadPaged(std::span<const u8> data, std::span<u8> memory)
	{
		const u32 page_count = static_cast<u32>(memory.size() / STATE_PAGE_SIZE);
		const u8* bitmap = data.data() + sizeof(PagedHeader);
		const u8* next = bitmap + static_cast<size_t>(GetBitmapWords(page_count)) * sizeof(u64);
		for (u32 page = 0; page < page_count; page++)
		{
			u64 bits;
			std::memcpy(&bits, bitmap + (page / 64) * sizeof(u64), sizeof(bits));
			u8* const target = memory.data() + static_cast<size_t>(page) * STATE_PAGE_SIZE;
			if (bits & (u64{1} << (page % 64)))
			{
				std::memcpy(target, next, STATE_PAGE_SIZE);
				next += STATE_PAGE_SIZE;
			}
			else
			{
				std::memset(target, 0, STATE_PAGE_SIZE);
			}
		}
	}

	std::vector<SegmentSource> GetSegmentSources()
	{
		std::vector<SegmentSource> sources = {{SegmentKind::Internals, nullptr}, {SegmentKind::Layout, nullptr}};
		for (const std::unique_ptr<BaseSavestateEntry>& entry : SavestateEntries)
		{
			if (entry->IsInMemoryState())
				sources.push_back({SegmentKind::Entry, entry.get()});
		}
		sources.push_back({SegmentKind::ArcadeDevices, nullptr});
		sources.push_back({SegmentKind::AudioOutput, nullptr});
		return sources;
	}

	bool CheckConfiguration(Error* error)
	{
		if (EmuConfig.HostFs)
		{
			Error::SetString(error, "Save states are not supported while HostFS is enabled.");
			return false;
		}
		for (const Pcsx2Config::USBOptions::Port& port : EmuConfig.USB.Ports)
		{
			if (port.DeviceType >= 0)
			{
				Error::SetString(error, "Save states are not supported while a USB device is configured.");
				return false;
			}
		}
		if (Achievements::IsActive())
		{
			Error::SetString(error, "Save states are not supported while achievements are active.");
			return false;
		}
		return ACSTATE::CheckSupported(error);
	}

	// The Gif_Path fields a save writes before the packet data, and where the host buffer
	// pointer lies within them.
	u32 GetGifPathFieldsSize()
	{
		return static_cast<u32>(sizeof(Gif_Path) - sizeof(Gif_Path_MTVU));
	}

	u32 GetGifPathOffset(const void* member)
	{
		return static_cast<u32>(static_cast<const u8*>(member) - reinterpret_cast<const u8*>(&gifUnit.gifPath[0]));
	}

	// Bytes from the recorded offset of a field to its end, and the part of them that varies.
	u64 GetFieldLength(const SaveStateFieldRecord& field)
	{
		switch (field.kind)
		{
			case SaveStateVariableField::String:
				return sizeof(u32) + static_cast<u64>(field.value);
			case SaveStateVariableField::GifPath:
				return GetGifPathFieldsSize() + static_cast<u64>(field.value);
			case SaveStateVariableField::Stream:
				return field.value;
			case SaveStateVariableField::HostHandles:
				return sizeof(size_t);
			case SaveStateVariableField::VifBuffer:
				return sizeof(u32) + static_cast<u64>(field.value);
		}
		return 0;
	}

	u64 GetFieldPayload(const SaveStateFieldRecord& field)
	{
		return (field.kind == SaveStateVariableField::HostHandles) ? 0 : field.value;
	}

	// The largest payload of a field, given the same field of the running VM. A stream holds
	// fixed-size members besides the two SIO2 FIFOs; HostFS is never enabled for these states.
	u64 GetFieldPayloadLimit(const SaveStateFieldRecord& current, u32 gif_index)
	{
		switch (current.kind)
		{
			case SaveStateVariableField::String:
				return MAX_STATE_STRING;
			case SaveStateVariableField::GifPath:
				return gifUnit.gifPath[gif_index].buffSize;
			case SaveStateVariableField::Stream:
				return static_cast<u64>(current.value) - g_Sio2FifoIn.size() - g_Sio2FifoOut.size() + 2 * MAX_SIO2_FIFO;
			case SaveStateVariableField::HostHandles:
				return 0;
			case SaveStateVariableField::VifBuffer:
				return sizeof(nVif[0].buffer);
		}
		return 0;
	}

	bool SaveInternals(std::vector<u8>* data, std::vector<SaveStateFieldRecord>* layout, Error* error,
		std::vector<SaveStateFieldConstraint>* constraints = nullptr)
	{
		data->clear();
		layout->clear();
		memSavingState saver(*data);
		saver.SetInMemoryState();
		saver.SetLayoutRecorder(layout);
		if (constraints)
		{
			constraints->clear();
			saver.SetConstraintRecorder(constraints);
		}
		Error freeze_error;
		if (!saver.FreezeBios() || !saver.FreezeInternals(&freeze_error) || saver.HasError())
		{
			Error::SetStringFmt(error, "The internal structures could not be saved: {}", freeze_error.GetDescription());
			return false;
		}
		data->resize(saver.GetCurrentPos());
		return true;
	}

	template <typename T>
	T ReadValue(std::span<const u8> data, u64 offset)
	{
		T value;
		std::memcpy(&value, data.data() + offset, sizeof(T));
		return value;
	}

	// The host buffer pointers of the GIF paths are not state: loading keeps the running VM's own.
	void NormalizeGifPaths(std::span<u8> internals, const std::vector<SaveStateFieldRecord>& layout)
	{
		const u32 buffer_offset = GetGifPathOffset(&gifUnit.gifPath[0].buffer);
		for (const SaveStateFieldRecord& field : layout)
		{
			if (field.kind == SaveStateVariableField::GifPath && static_cast<u64>(field.offset) + buffer_offset + sizeof(u8*) <= internals.size())
				std::memset(internals.data() + field.offset + buffer_offset, 0, sizeof(u8*));
		}
	}

	// Little-endian integer of `size` bytes at `offset`; the caller has checked the bounds.
	s64 ReadInteger(std::span<const u8> data, u64 offset, u32 size, bool is_signed)
	{
		u64 value = 0;
		std::memcpy(&value, data.data() + offset, size);
		if (is_signed && size < sizeof(value) && (value >> (size * 8 - 1)) != 0)
			value |= ~u64{0} << (size * 8);
		return static_cast<s64>(value);
	}

	// Where a fixed field of the running VM's internal structures lies in a state whose variable
	// fields before it hold other lengths. Fields are recorded in order, so every variable field
	// that ends before `offset` lies entirely before it.
	u64 MapOffset(u32 offset, const std::vector<SaveStateFieldRecord>& saved, const std::vector<SaveStateFieldRecord>& current)
	{
		s64 delta = 0;
		for (size_t i = 0; i < current.size(); i++)
		{
			if (current[i].offset + GetFieldLength(current[i]) > offset)
				break;
			delta += static_cast<s64>(GetFieldLength(saved[i])) - static_cast<s64>(GetFieldLength(current[i]));
		}
		return static_cast<u64>(static_cast<s64>(offset) + delta);
	}

	bool ReadOperand(std::span<const u8> internals, const SaveStateFieldConstraint::Operand& operand, u64 mapped, s64* value)
	{
		if (operand.size == 0 || operand.size > sizeof(u64) || mapped + operand.size > internals.size())
			return false;
		*value = ReadInteger(internals, mapped, operand.size, operand.is_signed != 0);
		return true;
	}

	// Checks every recorded constraint in the state and returns where located fields lie in it.
	bool CheckConstraints(std::span<const u8> internals, std::span<const u8> current_data,
		const std::vector<SaveStateFieldConstraint>& constraints, const std::vector<SaveStateFieldRecord>& saved,
		const std::vector<SaveStateFieldRecord>& current, std::vector<std::pair<SaveStateFieldConstraint::Location, u64>>* locations,
		Error* error)
	{
		for (size_t i = 0; i < constraints.size(); i++)
		{
			const SaveStateFieldConstraint& c = constraints[i];
			const u64 at = MapOffset(c.value.offset, saved, current);
			switch (c.kind)
			{
				case SaveStateFieldConstraint::Kind::Bytes:
				{
					if (at + c.value.size > internals.size() || c.value.offset + c.value.size > current_data.size() ||
						std::memcmp(internals.data() + at, current_data.data() + c.value.offset, c.value.size) != 0)
					{
						Error::SetStringFmt(error, "The state's internal structures hold a corrupt tag at offset {}.", at);
						return false;
					}
					break;
				}

				case SaveStateFieldConstraint::Kind::Locate:
					if (at + c.value.size > internals.size())
					{
						Error::SetString(error, "The state's internal structures are truncated.");
						return false;
					}
					locations->emplace_back(c.location, at);
					break;

				case SaveStateFieldConstraint::Kind::Range:
				{
					if (c.condition.size != 0)
					{
						s64 condition;
						if (!ReadOperand(internals, c.condition, MapOffset(c.condition.offset, saved, current), &condition))
						{
							Error::SetString(error, "The state's internal structures are truncated.");
							return false;
						}
						const bool equal = (static_cast<u32>(condition) & c.condition_mask) == c.condition_value;
						if (equal == (c.condition_negate != 0))
							break;
					}

					s64 value, addend = 0;
					if (!ReadOperand(internals, c.value, at, &value) ||
						(c.addend.size != 0 && !ReadOperand(internals, c.addend, MapOffset(c.addend.offset, saved, current), &addend)))
					{
						Error::SetString(error, "The state's internal structures are truncated.");
						return false;
					}
					const s64 total = value * c.value.scale + addend * c.addend.scale;
					if (total < c.min || total > c.max || (c.alignment > 1 && (value % c.alignment) != 0))
					{
						Error::SetStringFmt(error, "The state's internal structures hold the value {} at offset {} outside [{}, {}].",
							total, at, c.min, c.max);
						return false;
					}
					break;
				}
			}
		}
		return true;
	}

	// The SIO block is a StateWrapper stream; it is read as its DoState functions read it,
	// without changing anything.
	bool CheckSioStream(std::span<const u8> stream)
	{
		StateWrapper::ReadOnlyMemoryStream input(stream.data(), static_cast<u32>(stream.size()));
		StateWrapper sw(&input, StateWrapper::Mode::Read, g_SaveVersion);
		return Sio0::CheckState(sw) && Sio2::CheckState(sw) && MultitapProtocol::CheckState(sw) && MultitapProtocol::CheckState(sw) &&
			   sw.IsGood() && input.GetPosition() == stream.size();
	}

	// The internal structures are written field by field into the running VM, so they cannot be
	// tried before loading. Instead their layout is compared with a save of the running VM: the
	// same variable fields in the same order, identical fixed parts between them, and every
	// variable length within its limit and matching the bytes it describes.
	bool ValidateInternals(std::span<const u8> internals, std::span<const u8> layout_data,
		std::vector<std::pair<SaveStateFieldConstraint::Location, u64>>* locations, Error* error)
	{
		if (layout_data.size() % sizeof(SaveStateFieldRecord) != 0 || layout_data.size() / sizeof(SaveStateFieldRecord) > MAX_LAYOUT_RECORDS)
		{
			Error::SetString(error, "The state's layout of the internal structures is malformed.");
			return false;
		}
		std::vector<SaveStateFieldRecord> saved(layout_data.size() / sizeof(SaveStateFieldRecord));
		std::memcpy(saved.data(), layout_data.data(), layout_data.size());

		std::vector<u8> current_data;
		std::vector<SaveStateFieldRecord> current;
		std::vector<SaveStateFieldConstraint> constraints;
		if (!SaveInternals(&current_data, &current, error, &constraints))
			return false;

		if (saved.size() != current.size())
		{
			Error::SetStringFmt(error, "The state's internal structures have {} variable fields, the running VM {}.", saved.size(), current.size());
			return false;
		}

		const u32 buffsize_offset = GetGifPathOffset(&gifUnit.gifPath[0].buffSize);
		const u32 bufflimit_offset = GetGifPathOffset(&gifUnit.gifPath[0].buffLimit);
		const u32 cursize_offset = GetGifPathOffset(&gifUnit.gifPath[0].curSize);
		const u32 buffer_offset = GetGifPathOffset(&gifUnit.gifPath[0].buffer);

		u64 saved_end = 0;
		u64 current_end = 0;
		u32 gif_index = 0;
		for (size_t i = 0; i < saved.size(); i++)
		{
			const SaveStateFieldRecord& field = saved[i];
			if (field.kind != current[i].kind || field.offset < saved_end ||
				field.offset - saved_end != current[i].offset - current_end)
			{
				Error::SetStringFmt(error, "The state's internal structures differ from this build at variable field {}.", i);
				return false;
			}

			const u64 field_end = field.offset + GetFieldLength(field);
			if (GetFieldPayload(field) > GetFieldPayloadLimit(current[i], gif_index) || field_end > internals.size())
			{
				Error::SetStringFmt(error, "Variable field {} of the state's internal structures exceeds its limit.", i);
				return false;
			}

			bool consistent = true;
			switch (field.kind)
			{
				case SaveStateVariableField::String:
					consistent = (ReadValue<u32>(internals, field.offset) == field.value);
					break;
				case SaveStateVariableField::GifPath:
				{
					const Gif_Path& path = gifUnit.gifPath[gif_index];
					consistent = ReadValue<u32>(internals, field.offset + cursize_offset) == field.value &&
								 ReadValue<u32>(internals, field.offset + buffsize_offset) == path.buffSize &&
								 ReadValue<u32>(internals, field.offset + bufflimit_offset) == path.buffLimit &&
								 ReadValue<uptr>(internals, field.offset + buffer_offset) == 0;
					gif_index++;
					break;
				}
				case SaveStateVariableField::Stream:
					consistent = CheckSioStream(internals.subspan(field.offset, field.value));
					break;
				case SaveStateVariableField::HostHandles:
					consistent = (ReadValue<size_t>(internals, field.offset) == 0);
					break;
				case SaveStateVariableField::VifBuffer:
					consistent = (ReadValue<u32>(internals, field.offset) == field.value);
					break;
			}
			if (!consistent)
			{
				Error::SetStringFmt(error, "Variable field {} of the state's internal structures does not match its data.", i);
				return false;
			}

			saved_end = field_end;
			current_end = current[i].offset + GetFieldLength(current[i]);
		}

		if (internals.size() - saved_end != current_data.size() - current_end)
		{
			Error::SetString(error, "The state's internal structures end differently from this build.");
			return false;
		}
		return CheckConstraints(internals, current_data, constraints, saved, current, locations, error);
	}

	// A running IPU command resumes in a switch over its command number, which only covers the
	// commands that keep running (IDEC to PACK).
	bool CheckIpuCommand(std::span<const u8> internals, u64 command_offset, std::span<const u8> ee_hw, Error* error)
	{
		const u64 control_offset = static_cast<u64>(reinterpret_cast<const u8*>(&ipuRegs.ctrl) - eeHw);
		if (control_offset + sizeof(u32) > ee_hw.size() || command_offset + sizeof(u32) > internals.size())
		{
			Error::SetString(error, "The state's IPU registers are truncated.");
			return false;
		}
		tIPU_CTRL control(static_cast<u32>(ReadInteger(ee_hw, control_offset, sizeof(u32), false)));
		const u32 command = static_cast<u32>(ReadInteger(internals, command_offset, sizeof(u32), false)) >> 28;
		if (control.BUSY && (command < SCE_IPU_IDEC || command > SCE_IPU_PACK))
		{
			Error::SetStringFmt(error, "The state holds a busy IPU with command {}.", command);
			return false;
		}
		return true;
	}

	void SetSegmentError(Error* error, const char* what, const BaseSavestateEntry* entry)
	{
		Error::SetStringFmt(error, "The {} segment{}{} failed.", what, entry ? " " : "", entry ? entry->GetFilename() : "");
	}
} // namespace

// The segments with their limits, and the lengths the internal structures and their layout have
// in a save of the running VM.
static bool DescribeSegments(std::vector<SaveStateMemory::Segment>* segments, u32* internals_length, u32* layout_length, Error* error)
{
	if (!CheckConfiguration(error))
		return false;

	std::vector<u8> internals;
	std::vector<SaveStateFieldRecord> layout;
	if (!SaveInternals(&internals, &layout, error))
		return false;
	*internals_length = static_cast<u32>(internals.size());
	*layout_length = static_cast<u32>(layout.size() * sizeof(SaveStateFieldRecord));

	u64 internals_limit = internals.size();
	u32 gif_index = 0;
	for (const SaveStateFieldRecord& field : layout)
	{
		internals_limit += GetFieldPayloadLimit(field, gif_index) - GetFieldPayload(field);
		if (field.kind == SaveStateVariableField::GifPath)
			gif_index++;
	}
	if (layout.size() > MAX_LAYOUT_RECORDS || internals_limit > std::numeric_limits<u32>::max())
	{
		Error::SetString(error, "The internal structures have no usable size limit.");
		return false;
	}

	segments->clear();
	for (const SegmentSource& source : GetSegmentSources())
	{
		switch (source.kind)
		{
			case SegmentKind::Internals:
				segments->push_back({"internals", static_cast<u32>(internals_limit), false, false});
				break;
			case SegmentKind::Layout:
				segments->push_back({"layout", static_cast<u32>(MAX_LAYOUT_RECORDS * sizeof(SaveStateFieldRecord)), false, false});
				break;
			case SegmentKind::Entry:
				if (const std::span<u8> memory = source.entry->GetPagedMemory(); !memory.empty())
				{
					const u32 pages = static_cast<u32>(memory.size() / STATE_PAGE_SIZE);
					segments->push_back({source.entry->GetFilename(), static_cast<u32>(GetPagedLength(pages, pages)), false, true});
				}
				else if (const std::optional<u32> size = source.entry->GetFixedSize())
				{
					segments->push_back({source.entry->GetFilename(), *size, true, false});
				}
				else
				{
					std::vector<u8> data;
					memSavingState saver(data);
					if (!source.entry->FreezeOut(saver) || saver.HasError())
					{
						SetSegmentError(error, "size measurement of the", source.entry);
						return false;
					}
					segments->push_back({source.entry->GetFilename(), saver.GetCurrentPos(), true, false});
				}
				break;
			case SegmentKind::ArcadeDevices:
				segments->push_back({"arcade-devices", ACSTATE::GetSize(), true, false});
				break;
			case SegmentKind::AudioOutput:
				segments->push_back({"audio-output", SPU2::GetOutputStateSize(), true, false});
				break;
		}
	}
	return true;
}

bool SaveStateMemory::GetSegments(std::vector<Segment>* segments, Error* error)
{
	u32 internals_length, layout_length;
	return DescribeSegments(segments, &internals_length, &layout_length, error);
}

bool SaveStateMemory::Measure(Plan* plan, Error* error)
{
	std::vector<Segment> segments;
	u32 internals_length, layout_length;
	if (!DescribeSegments(&segments, &internals_length, &layout_length, error))
		return false;

	const std::vector<SegmentSource> sources = GetSegmentSources();
	plan->lengths.assign(sources.size(), 0);
	plan->page_bitmaps.assign(sources.size(), {});
	plan->present_pages.assign(sources.size(), 0);
	for (size_t i = 0; i < sources.size(); i++)
	{
		switch (sources[i].kind)
		{
			case SegmentKind::Internals:
				plan->lengths[i] = internals_length;
				break;
			case SegmentKind::Layout:
				plan->lengths[i] = layout_length;
				break;
			case SegmentKind::Entry:
				if (const std::span<u8> memory = sources[i].entry->GetPagedMemory(); !memory.empty())
				{
					BuildPageBitmap(memory, &plan->page_bitmaps[i], &plan->present_pages[i]);
					plan->lengths[i] = static_cast<u32>(GetPagedLength(static_cast<u32>(memory.size() / STATE_PAGE_SIZE), plan->present_pages[i]));
				}
				else
				{
					plan->lengths[i] = segments[i].size;
				}
				break;
			case SegmentKind::ArcadeDevices:
			case SegmentKind::AudioOutput:
				plan->lengths[i] = segments[i].size;
				break;
		}
	}
	return true;
}

// Writes a segment into `out` and returns its length; SaveSegment() checks it against the plan.
static bool WriteSegment(const SaveStateMemory::Plan& plan, size_t index, std::span<u8> out, u32* length, Error* error)
{
	const std::vector<SegmentSource> sources = GetSegmentSources();
	const SegmentSource& source = sources[index];
	switch (source.kind)
	{
		case SegmentKind::Internals:
		{
			// A coefficient decode suspended in its second half resumes with a table pointer that
			// is not state; it is never saved (see ipuFreeze).
			if (ipu_cmd.pos[5] != 0)
			{
				Error::SetString(error, "The IPU is in the middle of decoding video; try again in a moment.");
				return false;
			}

			s_saved_layout.clear();
			memSavingState saver(out);
			saver.SetInMemoryState();
			saver.SetLayoutRecorder(&s_saved_layout);
			Error freeze_error;
			if (!saver.FreezeBios() || !saver.FreezeInternals(&freeze_error) || saver.HasError() ||
				s_saved_layout.size() > MAX_LAYOUT_RECORDS)
			{
				Error::SetStringFmt(error, "The internal structures could not be saved: {}", freeze_error.GetDescription());
				return false;
			}
			*length = saver.GetCurrentPos();
			NormalizeGifPaths(out.first(*length), s_saved_layout);
			return true;
		}

		case SegmentKind::Layout:
		{
			const size_t size = s_saved_layout.size() * sizeof(SaveStateFieldRecord);
			if (size > out.size())
			{
				SetSegmentError(error, "layout", nullptr);
				return false;
			}
			std::memcpy(out.data(), s_saved_layout.data(), size);
			*length = static_cast<u32>(size);
			return true;
		}

		case SegmentKind::Entry:
		{
			if (const std::span<u8> memory = source.entry->GetPagedMemory(); !memory.empty())
			{
				SavePaged(memory, plan.page_bitmaps[index], plan.present_pages[index], out);
				*length = plan.lengths[index];
				return true;
			}

			// The GS keeps rendered data on the GPU until it is read back; the state takes the
			// local memory only.
			if (dynamic_cast<const SavestateEntry_GS*>(source.entry))
			{
				MTGS::RunOnGSThread(&GSReadbackTextureCacheForState);
				MTGS::WaitGS(false);
			}

			memSavingState saver(out);
			if (!source.entry->FreezeOut(saver) || saver.HasError())
			{
				SetSegmentError(error, "save of the", source.entry);
				return false;
			}
			*length = saver.GetCurrentPos();
			if (dynamic_cast<const SavestateEntry_SPU2*>(source.entry))
				SPU2::NormalizeFreezeData(out.first(*length));
			return true;
		}

		case SegmentKind::ArcadeDevices:
		{
			const u32 size = ACSTATE::GetSize();
			if (size > out.size())
			{
				SetSegmentError(error, "arcade device", nullptr);
				return false;
			}
			ACSTATE::Save(out.first(size));
			*length = size;
			return true;
		}

		case SegmentKind::AudioOutput:
		{
			const u32 size = SPU2::GetOutputStateSize();
			if (size > out.size())
			{
				SetSegmentError(error, "audio output", nullptr);
				return false;
			}
			SPU2::SaveOutputState(out.first(size));
			*length = size;
			return true;
		}
	}
	return false;
}

bool SaveStateMemory::SaveSegment(const Plan& plan, size_t index, std::span<u8> out, Error* error)
{
	if (index >= plan.lengths.size() || out.size() != plan.lengths[index])
	{
		Error::SetStringFmt(error, "Segment {} does not match the measured state.", index);
		return false;
	}

	u32 length = 0;
	if (!WriteSegment(plan, index, out, &length, error))
		return false;

	// The plan describes the VM as it was measured; a different length means it has changed.
	if (length != plan.lengths[index])
	{
		Console.ErrorFmt("Segment {} has {} bytes, {} were measured", index, length, plan.lengths[index]);
		Error::SetString(error, "The VM changed after the state was measured.");
		return false;
	}
	return true;
}

bool SaveStateMemory::Validate(std::span<const std::span<const u8>> segments, Error* error)
{
	std::vector<Segment> expected;
	if (!GetSegments(&expected, error))
		return false;

	if (segments.size() != expected.size())
	{
		Error::SetStringFmt(error, "The state has {} segments, the running VM {}.", segments.size(), expected.size());
		return false;
	}
	const std::vector<SegmentSource> segment_sources = GetSegmentSources();
	for (size_t i = 0; i < segments.size(); i++)
	{
		if (expected[i].paged)
		{
			if (!ValidatePaged(segments[i], segment_sources[i].entry->GetPagedMemory().size(), expected[i].name.c_str(), error))
				return false;
			continue;
		}
		if (expected[i].exact ? (segments[i].size() != expected[i].size) : (segments[i].size() > expected[i].size))
		{
			Error::SetStringFmt(error, "The state's {} segment holds {} bytes, the running VM {} {}.", expected[i].name,
				segments[i].size(), expected[i].exact ? "needs" : "allows at most", expected[i].size);
			return false;
		}
	}

	const std::vector<SegmentSource> sources = GetSegmentSources();
	std::vector<std::pair<SaveStateFieldConstraint::Location, u64>> locations;
	std::span<const u8> ee_hw;
	for (size_t i = 0; i < sources.size(); i++)
	{
		if (sources[i].kind == SegmentKind::Entry && std::strcmp(sources[i].entry->GetFilename(), "eeHwRegs.bin") == 0)
			ee_hw = segments[i];
	}
	for (size_t i = 0; i < sources.size(); i++)
	{
		const SegmentSource& source = sources[i];
		switch (source.kind)
		{
			case SegmentKind::Internals:
				if (!ValidateInternals(segments[i], segments[i + 1], &locations, error))
					return false;
				break;
			case SegmentKind::Layout:
				break;
			case SegmentKind::Entry:
				if (dynamic_cast<const SavestateEntry_SPU2*>(source.entry) && !SPU2::ValidateFreezeData(segments[i], error))
					return false;
				if (dynamic_cast<const SavestateEntry_GS*>(source.entry) && !GSValidateFreezeData(segments[i], error))
					return false;
				if (dynamic_cast<const SavestateEntry_PAD*>(source.entry) && !SavestateEntry_PAD::Validate(segments[i], error))
					return false;
				break;
			case SegmentKind::ArcadeDevices:
				if (!ACSTATE::Validate(segments[i], error))
					return false;
				break;
			case SegmentKind::AudioOutput:
				if (!SPU2::ValidateOutputState(segments[i], error))
					return false;
				break;
		}
	}

	for (const auto& [location, offset] : locations)
	{
		if (location == SaveStateFieldConstraint::Location::IpuCommand && !CheckIpuCommand(segments[0], offset, ee_hw, error))
			return false;
	}
	return true;
}

bool SaveStateMemory::Load(std::span<const std::span<const u8>> segments, Error* error)
{
	const std::vector<SegmentSource> sources = GetSegmentSources();
	if (segments.size() != sources.size())
	{
		Error::SetString(error, "The state does not match the running VM.");
		return false;
	}

	// From here on the VM holds parts of the state, so its persistent data cannot be trusted
	// until everything has been restored.
	VMManager::Internal::SetPersistentDataUntrusted(true);
	PreLoadPrep();

	for (size_t i = 0; i < sources.size(); i++)
	{
		const SegmentSource& source = sources[i];
		bool loaded = true;
		switch (source.kind)
		{
			case SegmentKind::Internals:
			{
				memLoadingState state(segments[i]);
				state.SetInMemoryState();
				Error freeze_error;
				loaded = state.FreezeBios() && state.FreezeInternals(&freeze_error) && !state.HasError() &&
						 state.GetCurrentPos() == segments[i].size();
				if (!loaded)
					Error::SetStringFmt(error, "The internal structures could not be restored: {}", freeze_error.GetDescription());
				break;
			}
			case SegmentKind::Layout:
				break;
			case SegmentKind::Entry:
				if (const std::span<u8> memory = source.entry->GetPagedMemory(); !memory.empty())
				{
					LoadPaged(segments[i], memory);
					break;
				}
				loaded = source.entry->LoadFromMemory(segments[i]);
				if (!loaded)
					SetSegmentError(error, "restore of the", source.entry);
				break;
			case SegmentKind::ArcadeDevices:
				ACSTATE::Load(segments[i]);
				break;
			case SegmentKind::AudioOutput:
				SPU2::LoadOutputState(segments[i]);
				break;
		}
		if (!loaded)
			return false;
	}

	// The VU1 thread receives VU1 memory and programs only now that they hold the state.
	mtvuRebuildAfterStateLoad();
	PostLoadPrep();
	if (THREAD_VU1)
		vu1Thread.WaitVU();
	MTGS::WaitGS(false);

	VMManager::Internal::SetPersistentDataUntrusted(false);
	return true;
}

void SaveState_ReportLoadErrorOSD(const std::string& message, std::optional<s32> slot, bool backup)
{
	std::string full_message;
	if (slot.has_value())
	{
		if (backup)
			full_message = fmt::format(
				TRANSLATE_FS("SaveState", "Failed to load state from backup slot {}: {}"), *slot, message);
		else
			full_message = fmt::format(
				TRANSLATE_FS("SaveState", "Failed to load state from slot {}: {}"), *slot, message);
	}
	else
	{
		full_message = fmt::format(TRANSLATE_FS("SaveState", "Failed to load state: {}"), message);
	}

	Host::AddIconOSDMessage("LoadState", ICON_FA_TRIANGLE_EXCLAMATION,
		full_message, Host::OSD_WARNING_DURATION);
}

void SaveState_ReportSaveErrorOSD(const std::string& message, std::optional<s32> slot)
{
	std::string full_message;
	if (slot.has_value())
		full_message = fmt::format(
			TRANSLATE_FS("SaveState", "Failed to save state to slot {}: {}"), *slot, message);
	else
		full_message = fmt::format(TRANSLATE_FS("SaveState", "Failed to save state: {}"), message);

	Host::AddIconOSDMessage("SaveState", ICON_FA_TRIANGLE_EXCLAMATION,
		full_message, Host::OSD_WARNING_DURATION);
}
