// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/Assertions.h"

#include <type_traits>

class Error;

enum class FreezeAction
{
	Load,
	Save,
	Size,
};

// Savestate Versioning!

// NOTICE: When updating g_SaveVersion, please make sure you add the following line to your commit message somewhere:
// [SAVEVERSION+]
// This informs the auto updater that the users savestates will be invalidated.

static const u32 g_SaveVersion = (0x9A59 << 16) | 0x0000;


// the freezing data between submodules and core
// an interesting thing to note is that this dates back from before plugin
// merges and was used to pass data between plugins and cores, although the
// struct was system dependant as the size of int differs between systems, thus
// subsystems making use of freezeData, like GSDump and save states aren't
// necessarily portable; we might want to investigate this in the future -- govanify
struct freezeData
{
	int size;
	u8* data;
};

struct SaveStateScreenshotData
{
	u32 width;
	u32 height;
	std::vector<u32> pixels;
};

class ArchiveEntryList;

// Wrappers to generate a save state compatible across all frontends.
// These functions assume that the caller has paused the core thread.
extern std::unique_ptr<ArchiveEntryList> SaveState_DownloadState(Error* error);
extern std::unique_ptr<SaveStateScreenshotData> SaveState_SaveScreenshot();
extern bool SaveState_ZipToDisk(
	std::unique_ptr<ArchiveEntryList> srclist, std::unique_ptr<SaveStateScreenshotData> screenshot,
	const char* filename, Error* error);
extern bool SaveState_ReadScreenshot(const std::string& filename, u32* out_width, u32* out_height, std::vector<u32>* out_pixels);
extern bool SaveState_UnzipFromDisk(const std::string& filename, Error* error);

// Variable-length fields of the internal structures, in the order a save writes them. A save can
// record them so that a state is checked against the layout of the running build before anything
// of it is loaded.
enum class SaveStateVariableField : u32
{
	String, // u32 length, then that many characters
	GifPath, // the Gif_Path fields, then curSize bytes of packet data
	Stream, // a StateWrapper block of the given length
	HostHandles, // number of HostFS handles that follow
	VifBuffer, // u32 length, then that many bytes of a partly received VIF unpack
};

struct SaveStateFieldRecord
{
	SaveStateVariableField kind;
	u32 offset;
	u32 value;
};

// A condition on fixed bytes of the internal structures, recorded by a save of the running VM at
// the positions it writes them. A state is checked against these before anything of it is loaded:
// tags hold exactly the bytes a save writes, and fields the emulator uses without masking as
// indices, lengths, divisors or pointers lie within the range its own code maintains.
struct SaveStateFieldConstraint
{
	// A little-endian integer field at an offset of the internal structures.
	struct Operand
	{
		u32 offset;
		u32 size; // 0 when the operand is absent
		u32 is_signed;
		s32 scale;
	};

	enum class Kind : u32
	{
		Bytes, // `value.size` bytes equal those of the save
		Range, // value * scale + addend * scale in [min, max]
		Locate, // no condition: where a field lies, for checks across segments
	};

	enum class Location : u32
	{
		None,
		IpuCommand, // tIPU_cmd::current
	};

	Kind kind;
	Operand value;
	Operand addend;
	s64 min;
	s64 max;
	// The value is a multiple of it; 0 for any.
	u32 alignment;
	// The constraint holds only while (condition & condition_mask) == condition_value, or while
	// it differs when condition_negate is set; condition.size 0 for always.
	Operand condition;
	u32 condition_mask;
	u32 condition_value;
	u32 condition_negate;
	Location location;
};

// --------------------------------------------------------------------------------------
//  SaveStateBase class
// --------------------------------------------------------------------------------------
// Provides the base API for both loading and saving savestates.  Normally you'll want to
// use one of the four "functional" derived classes rather than this class directly: gzLoadingState, gzSavingState (gzipped disk-saved
// states), and memLoadingState, memSavingState (uncompressed memory states).
class SaveStateBase
{
public:
	using VmStateBuffer = std::vector<u8>;

protected:
	// A growable vector (desktop states) or a fixed caller-owned block. Neither is accessed
	// beyond its size: writing past a fixed block and reading past the end of either fails.
	VmStateBuffer* m_growable = nullptr;
	u8* m_data = nullptr;
	u32 m_size = 0;

	u32 m_version = 0;		// version of the savestate being loaded.

	int m_idx = 0;			// current read/write index of the allocation

	bool m_error = false; // error occurred while reading/writing

	// Records the variable-length fields of a save when set.
	std::vector<SaveStateFieldRecord>* m_layout = nullptr;
	// Records the constraints on fixed fields of a save when set.
	std::vector<SaveStateFieldConstraint>* m_constraints = nullptr;

	// An in-memory state of the libretro core rather than a desktop state. Loading rebuilds the
	// VU1 thread only once every part of the state, VU1 memory and programs included, has been
	// restored; host pointers are written as offsets that a load turns back into pointers.
	bool m_in_memory_state = false;

public:
	SaveStateBase(VmStateBuffer& memblock);
	SaveStateBase(std::span<u8> memblock);
	virtual ~SaveStateBase() = default;

	__fi bool HasError() const { return m_error; }
	__fi bool IsOkay() const { return !m_error; }

	// Gets the version of savestate that this object is acting on.
	// The version refers to the low 16 bits only (high 16 bits classifies Pcsx2 build types)
	u32 GetVersion() const
	{
		return (m_version & 0xffff);
	}

	bool FreezeBios();
	bool FreezeInternals(Error* error);

	void SetLayoutRecorder(std::vector<SaveStateFieldRecord>* layout) { m_layout = layout; }
	void SetConstraintRecorder(std::vector<SaveStateFieldConstraint>* constraints) { m_constraints = constraints; }

	bool IsRecordingConstraints() const { return m_constraints && IsSaving() && !m_error; }

	// Saving: the field `member` of `block`, which is written next starting at the current
	// position. For a field written on its own, `block` is the field.
	template <typename B, typename T>
	SaveStateFieldConstraint::Operand FieldOf(const B& block, const T& member, s32 scale = 1) const
	{
		static_assert(std::is_integral_v<T> || std::is_enum_v<T> || std::is_pointer_v<T>);
		using Underlying = typename std::conditional_t<std::is_enum_v<T>, std::underlying_type<T>, std::type_identity<T>>::type;
		const u32 offset = static_cast<u32>(m_idx) +
			static_cast<u32>(reinterpret_cast<const u8*>(&member) - reinterpret_cast<const u8*>(&block));
		return {offset, static_cast<u32>(sizeof(T)), std::is_signed_v<Underlying> ? 1u : 0u, scale};
	}

	// Saving: the next `size` bytes written must be identical in a state to be loaded.
	void NoteFixedBytes(u32 size)
	{
		if (IsRecordingConstraints())
			m_constraints->push_back({SaveStateFieldConstraint::Kind::Bytes, {static_cast<u32>(m_idx), size, 0, 1}});
	}

	// Saving: `value` (plus `addend` when present) must lie in [min, max] in a state to be loaded.
	// Returns the constraint so that a condition can be added, or null while not recording.
	SaveStateFieldConstraint* NoteRange(SaveStateFieldConstraint::Operand value, s64 min, s64 max,
		SaveStateFieldConstraint::Operand addend = {})
	{
		if (!IsRecordingConstraints())
			return nullptr;
		SaveStateFieldConstraint constraint = {SaveStateFieldConstraint::Kind::Range, value, addend, min, max};
		m_constraints->push_back(constraint);
		return &m_constraints->back();
	}

	template <typename B, typename T>
	SaveStateFieldConstraint* NoteRange(const B& block, const T& member, s64 min, s64 max)
	{
		return NoteRange(FieldOf(block, member), min, max);
	}

	template <typename T>
	SaveStateFieldConstraint* NoteRange(const T& field, s64 min, s64 max)
	{
		return NoteRange(FieldOf(field, field), min, max);
	}

	// Limits a recorded constraint to states where (condition & mask) == value, or != with negate.
	static void When(SaveStateFieldConstraint* constraint, SaveStateFieldConstraint::Operand condition, u32 mask, u32 value,
		bool negate = false)
	{
		if (!constraint)
			return;
		constraint->condition = condition;
		constraint->condition_mask = mask;
		constraint->condition_value = value;
		constraint->condition_negate = negate ? 1 : 0;
	}

	// Saving: records where a field lies, for checks that involve other segments.
	void NoteLocation(SaveStateFieldConstraint::Location location, SaveStateFieldConstraint::Operand field)
	{
		if (!IsRecordingConstraints())
			return;
		SaveStateFieldConstraint constraint = {SaveStateFieldConstraint::Kind::Locate, field};
		constraint.location = location;
		m_constraints->push_back(constraint);
	}
	void SetInMemoryState() { m_in_memory_state = true; }

	// Saving: records a variable-length field that starts at the current position.
	void NoteVariableField(SaveStateVariableField kind, u32 value)
	{
		if (m_layout && IsSaving() && !m_error)
			m_layout->push_back({kind, static_cast<u32>(m_idx), value});
	}

	// Checks an access of `size` bytes at the current position before any arithmetic on it: a
	// negative size, or one beyond the remaining data when loading, fails; when saving, the block
	// grows to hold it, and a fixed or 4 GiB block that cannot fails. Sets the error on failure.
	bool PrepareAccess(int size);

	// Bytes between the current position and the end of the data.
	u32 GetRemaining() const { return (static_cast<u32>(m_idx) < m_size) ? (m_size - static_cast<u32>(m_idx)) : 0; }

	// Loads or saves an arbitrary data type.  Usable on atomic types, structs, and arrays.
	// For dynamically allocated pointers use FreezeMem instead.
	template<typename T>
	void Freeze( T& data )
	{
		FreezeMem( const_cast<void*>((void*)&data), sizeof( T ) );
	}

	// FreezeLegacy can be used to load structures short of their full size, which is
	// useful for loading structures that have had new stuff added since a previous version.
	template<typename T>
	void FreezeLegacy( T& data, int sizeOfNewStuff )
	{
		FreezeMem( &data, sizeof( T ) - sizeOfNewStuff );
	}

	void PrepBlock( int size );

	template <typename T>
	void FreezeDeque(std::deque<T>& q)
	{
		// overwritten when loading
		u32 count = static_cast<u32>(q.size());
		Freeze(count);

		// A count beyond the remaining data is corrupt; it is not allocated.
		if (m_error || (IsLoading() && static_cast<u64>(count) * sizeof(T) > GetRemaining()))
		{
			m_error = true;
			return;
		}

		// have to use a temp array, because deque doesn't have a contiguous block of memory
		std::unique_ptr<T[]> temp;
		if (count > 0)
		{
			temp = std::make_unique<T[]>(count);
			if (IsSaving())
			{
				u32 pos = 0;
				for (const T& it : q)
					temp[pos++] = it;
			}

			FreezeMem(temp.get(), static_cast<int>(sizeof(T) * count));
		}

		if (IsLoading())
		{
			q.clear();
			for (u32 i = 0; i < count; i++)
				q.push_back(temp[i]);
		}
	}

	void FreezeString(std::string& s)
	{
		// overwritten when loading
		u32 length = static_cast<u32>(s.length());
		NoteVariableField(SaveStateVariableField::String, length);
		Freeze(length);

		if (IsLoading())
		{
			// A length beyond the data is corrupt; it is not allocated.
			if (m_error || length > GetRemaining())
			{
				m_error = true;
				return;
			}
			s.resize(length);
		}

		FreezeMem(s.data(), length);
	}

	uint GetCurrentPos() const
	{
		return m_idx;
	}

	// Only valid after a successful PrepBlock() for the bytes that follow.
	u8* GetBlockPtr()
	{
		return m_data + m_idx;
	}

	// Advances past a block prepared with PrepBlock(); a size beyond it fails.
	void CommitBlock( int size )
	{
		if (m_error || size < 0 || static_cast<u32>(size) > GetRemaining())
		{
			m_error = true;
			return;
		}
		m_idx += size;
	}

	// Freezes an identifier value into the savestate for troubleshooting purposes.
	// Identifiers can be used to determine where in a savestate that data has become
	// skewed (if the value does not match then the error occurs somewhere prior to that
	// position).
	bool FreezeTag( const char* src );

	// Returns true if this object is a StateLoading type object.
	bool IsLoading() const { return !IsSaving(); }

	// Loads or saves a memory block.
	virtual void FreezeMem( void* data, int size )=0;

	// Returns true if this object is a StateSaving type object.
	virtual bool IsSaving() const=0;

public:
	// note: gsFreeze() needs to be public because of the GSState recorder.
	bool gsFreeze();

protected:
	bool vmFreeze();
	bool mtvuFreeze();
	bool rcntFreeze();
	bool memFreeze(Error* error);
	bool vuMicroFreeze();
	bool vuJITFreeze();
	bool vif0Freeze();
	bool vif1Freeze();
	bool sifFreeze();
	bool ipuFreeze();
	bool ipuDmaFreeze();
	bool gifFreeze();
	bool gifDmaFreeze();
	bool gifPathFreeze(u32 path); // called by gifFreeze()

	bool sprFreeze();

	bool sioFreeze();
	bool cdrFreeze();
	bool cdvdFreeze();
	bool psxRcntFreeze();
	bool deci2Freeze();
	bool handleFreeze();

	// Save or load PCSX2's global frame counter (g_FrameCount) along with each savestate
	//
	// This is to prevent any inaccuracy issues caused by having a different
	// internal emulation frame count than what it was at the beginning of the
	// original recording
	bool InputRecordingFreeze();
};

// --------------------------------------------------------------------------------------
//  ArchiveEntry
// --------------------------------------------------------------------------------------
class ArchiveEntry final
{
protected:
	std::string	m_filename;
	uptr		m_dataidx;
	size_t		m_datasize;

public:
	ArchiveEntry(std::string filename)
		: m_filename(std::move(filename))
	{
		m_dataidx = 0;
		m_datasize = 0;
	}

	~ArchiveEntry() = default;

	ArchiveEntry& SetDataIndex(uptr idx)
	{
		m_dataidx = idx;
		return *this;
	}

	ArchiveEntry& SetDataSize(size_t size)
	{
		m_datasize = size;
		return *this;
	}

	const std::string& GetFilename() const
	{
		return m_filename;
	}

	uptr GetDataIndex() const
	{
		return m_dataidx;
	}

	uint GetDataSize() const
	{
		return m_datasize;
	}
};

// --------------------------------------------------------------------------------------
//  ArchiveEntryList
// --------------------------------------------------------------------------------------
class ArchiveEntryList final
{
public:
	using VmStateBuffer = std::vector<u8>;
	DeclareNoncopyableObject(ArchiveEntryList);

protected:
	std::vector<ArchiveEntry> m_list;
	VmStateBuffer m_data;

public:
	ArchiveEntryList() = default;
	~ArchiveEntryList() = default;

	const VmStateBuffer& GetBuffer() const
	{
		return m_data;
	}

	VmStateBuffer& GetBuffer()
	{
		return m_data;
	}

	u8* GetPtr(uint idx)
	{
		return &m_data[idx];
	}

	const u8* GetPtr(uint idx) const
	{
		return &m_data[idx];
	}

	ArchiveEntryList& Add(const ArchiveEntry& src)
	{
		m_list.push_back(src);
		return *this;
	}

	size_t GetLength() const
	{
		return m_list.size();
	}

	ArchiveEntry& operator[](uint idx)
	{
		return m_list[idx];
	}

	const ArchiveEntry& operator[](uint idx) const
	{
		return m_list[idx];
	}
};

// --------------------------------------------------------------------------------------
//  Saving and Loading Specialized Implementations...
// --------------------------------------------------------------------------------------

class memSavingState final : public SaveStateBase
{
	typedef SaveStateBase _parent;

public:
	memSavingState(VmStateBuffer& save_to);
	memSavingState(std::span<u8> save_to);
	~memSavingState() override = default;

	void FreezeMem(void* data, int size) override;
	bool IsSaving() const override { return true; }
};

class memLoadingState final : public SaveStateBase
{
public:
	memLoadingState(const VmStateBuffer& load_from);
	memLoadingState(std::span<const u8> load_from);
	~memLoadingState() override = default;

	void FreezeMem(void* data, int size) override;
	bool IsSaving() const override { return false; }
};

// --------------------------------------------------------------------------------------
//  In-memory states in caller-owned buffers
// --------------------------------------------------------------------------------------
// A state as a sequence of segments, each written into and read from a fixed block. Every
// function requires the CPU thread with the VM outside execution and the VU and GS threads idle.
// The EE, IOP and ACRAM memories are page-encoded: a bitmap of their 4 KiB pages that hold any
// non-zero byte, followed by those pages; loading clears every other page.
namespace SaveStateMemory
{
	struct Segment
	{
		std::string name;
		u32 size; // the exact size, or the largest size of a variable segment
		bool exact;
		bool paged; // page-encoded memory; its structure decides its length
	};

	/// The lengths of a state of the running VM as it stands, and the page bitmaps they come
	/// from. Valid until the VM changes.
	struct Plan
	{
		std::vector<u32> lengths;
		// Per segment: one bit per page of a page-encoded memory that holds a non-zero byte.
		std::vector<std::vector<u64>> page_bitmaps;
		std::vector<u32> present_pages;
	};

	/// The segments of a state of the running VM, in save and load order, with the limits a
	/// state is checked against. Fails for configurations whose state is not covered, such as
	/// HostFS or a USB device.
	bool GetSegments(std::vector<Segment>* segments, Error* error);

	/// Measures a state of the running VM: the length each segment will have.
	bool Measure(Plan* plan, Error* error);

	/// Writes segment `index` of the running VM, measured by `plan`, into `out`. The VM must be
	/// unchanged since the measurement; a segment of another length fails.
	bool SaveSegment(const Plan& plan, size_t index, std::span<u8> out, Error* error);

	/// Checks a complete state against the running VM. Nothing is changed.
	bool Validate(std::span<const std::span<const u8>> segments, Error* error);

	/// Restores a validated state. Once restoration has started, a failure leaves the VM
	/// partially restored, with its persistent data marked as untrusted.
	bool Load(std::span<const std::span<const u8>> segments, Error* error);
} // namespace SaveStateMemory

void SaveState_ReportLoadErrorOSD(const std::string& message, std::optional<s32> slot, bool backup);
void SaveState_ReportSaveErrorOSD(const std::string& message, std::optional<s32> slot);
