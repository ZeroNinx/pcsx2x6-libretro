// SPDX-FileCopyrightText: 2026 pcsx2x6-libretro contributors
// SPDX-License-Identifier: GPL-3.0+

#include "LibretroState.h"
#include "LibretroHost.h"
#include "LibretroSession.h"
#include "LibretroVersion.h"

#include "Config.h"
#include "CDVD/CDVD.h"
#include "DEV9/ACATA_IO_CHD.h"
#include "DEV9/ACJV.h"
#include "MTVU.h"
#include "Patch.h"
#include "SIO/Memcard/MemoryCardFile.h"
#include "SIO/Sio.h"
#include "SIO/Sio0.h"
#include "SIO/SioTypes.h"
#include "SaveState.h"
#include "VMManager.h"
#include "ps2/BiosTools.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/MD5Digest.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <zlib.h>

#include <array>
#include <limits>
#include <span>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace
{
	constexpr char STATE_MAGIC[8] = {'X', '6', 'L', 'R', 'S', 'T', 'A', 'T'};
	// 2: segments at their actual length, EE/IOP/ACRAM memory page-encoded. Other versions are
	// rejected, whatever build wrote them.
	constexpr u32 FORMAT_VERSION = 2;
	constexpr u32 PLATFORM_WINDOWS_X64 = 1;
	constexpr u32 MAX_SEGMENTS = 32;
	// Segments start at this alignment within the state.
	constexpr u32 SEGMENT_ALIGNMENT = 16;

	// What a state may only be loaded into: the same content and the configuration that
	// decides how the emulator runs it. Compared as a whole; unused bytes are Zero.
	struct Identity
	{
		char game_id[16];
		u8 manifest_md5[16];
		u8 elf_md5[16];
		u8 media_sha1[20];
		u32 bios_checksum;
		u32 mtvu;
		s32 ee_cycle_rate;
		u32 ee_cycle_skip;
		u32 renderer;
		float upscale_multiplier;
		u64 patches;
		// Content of the external files that a load does not restore: the inserted memory cards
		// (the dongle among them), hashed over their whole current content, and the BIOS NVRAM
		// and mechacon data.
		u64 cards[2][4];
		u64 persistent_data;
	};

	struct Header
	{
		char magic[8];
		u32 format_version;
		u32 header_size;
		u32 save_version;
		u32 platform;
		char build[128];
		Identity identity;
		u32 segment_count;
		// Bytes of the state in use: header, table and segments.
		u32 used_length;
	};

	struct SegmentEntry
	{
		char name[32];
		u32 offset;
		u32 length;
		u32 crc32;
		u32 reserved;
	};

	constexpr u32 DATA_START = static_cast<u32>(sizeof(Header) + MAX_SEGMENTS * sizeof(SegmentEntry));

	// The measurement of the running VM's state, for the state generation it was taken in.
	struct Measurement
	{
		u64 generation = 0;
		SaveStateMemory::Plan plan;
		u32 total = 0;
	};
	std::optional<Measurement> s_measurement;

	// The content files hashed once per content session.
	struct FileHashes
	{
		std::string manifest_path;
		std::string elf_path;
		std::array<u8, 16> manifest_md5 = {};
		std::array<u8, 16> elf_md5 = {};
	};
	std::optional<FileHashes> s_file_hashes;

	u32 AlignSegment(u32 value)
	{
		return (value + SEGMENT_ALIGNMENT - 1) & ~(SEGMENT_ALIGNMENT - 1);
	}

	bool HashFile(const std::string& path, std::array<u8, 16>* digest)
	{
		const std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
		if (!data.has_value())
			return false;

		MD5Digest md5;
		md5.Update(data->data(), static_cast<u32>(data->size()));
		md5.Final(digest->data());
		return true;
	}

	// CPU thread: the identity of the running content and configuration.
	bool GetIdentity(Identity* identity, Error* error)
	{
		std::memset(identity, 0, sizeof(*identity));

		const std::string& manifest = LibretroHost::GetContentPath();
		const std::string& elf = VMManager::Internal::GetELFOverride();
		if (!s_file_hashes.has_value() || s_file_hashes->manifest_path != manifest || s_file_hashes->elf_path != elf)
		{
			FileHashes hashes;
			hashes.manifest_path = manifest;
			hashes.elf_path = elf;
			if (!HashFile(manifest, &hashes.manifest_md5) || (!elf.empty() && !HashFile(elf, &hashes.elf_md5)))
			{
				Error::SetString(error, "The content files could not be read to identify the state.");
				return false;
			}
			s_file_hashes = std::move(hashes);
		}

		StringUtil::Strlcpy(identity->game_id, ACJV::GetGameId(), sizeof(identity->game_id));
		std::memcpy(identity->manifest_md5, s_file_hashes->manifest_md5.data(), sizeof(identity->manifest_md5));
		std::memcpy(identity->elf_md5, s_file_hashes->elf_md5.data(), sizeof(identity->elf_md5));
		// Writable raw images change underneath a state, so only read-only CHD media qualify.
		if (!CHD.IsOpen())
		{
			Error::SetString(error, "Save states need CHD media; raw media images are not supported.");
			return false;
		}
		if (!CHD.GetSha1(identity->media_sha1))
		{
			Error::SetString(error, "The media image could not be identified.");
			return false;
		}

		identity->bios_checksum = BiosChecksum;
		identity->mtvu = THREAD_VU1 ? 1 : 0;
		identity->ee_cycle_rate = EmuConfig.Speedhacks.EECycleRate;
		identity->ee_cycle_skip = EmuConfig.Speedhacks.EECycleSkip;
		identity->renderer = static_cast<u32>(EmuConfig.GS.Renderer);
		identity->upscale_multiplier = EmuConfig.GS.UpscaleMultiplier;
		identity->patches = Patch::GetActivePatchesFingerprint();
		for (u32 port = 0; port < 2; port++)
		{
			for (u32 slot = 0; slot < 4; slot++)
			{
				if (!FileMcd_IsPresent(port, slot))
					continue;

				const std::optional<u64> fingerprint = FileMcd_GetContentFingerprint(port, slot);
				if (!fingerprint.has_value())
				{
					Error::SetStringFmt(error, "The content of the memory card in port {} slot {} could not be read.", port + 1, slot + 1);
					return false;
				}
				identity->cards[port][slot] = *fingerprint;
			}
		}
		identity->persistent_data = cdvdGetPersistentDataFingerprint();
		return true;
	}

	// Names the first identity field that differs, for the rejection message.
	const char* DescribeIdentityMismatch(const Identity& saved, const Identity& current)
	{
#define CHECK_FIELD(field, description) \
	if (std::memcmp(&saved.field, &current.field, sizeof(saved.field)) != 0) \
		return description;
		CHECK_FIELD(game_id, "it belongs to another game");
		CHECK_FIELD(manifest_md5, "it was saved with another content manifest");
		CHECK_FIELD(elf_md5, "it was saved with another game program");
		CHECK_FIELD(media_sha1, "it was saved with another media image");
		CHECK_FIELD(bios_checksum, "it was saved with another BIOS");
		CHECK_FIELD(mtvu, "it was saved with another VU1 thread (MTVU) setting");
		CHECK_FIELD(ee_cycle_rate, "it was saved with another EE cycle rate");
		CHECK_FIELD(ee_cycle_skip, "it was saved with another EE cycle skip");
		CHECK_FIELD(renderer, "it was saved with another renderer");
		CHECK_FIELD(upscale_multiplier, "it was saved at another internal resolution");
		CHECK_FIELD(patches, "it was saved with other patches");
		CHECK_FIELD(cards, "a memory card or the dongle has changed since it was saved");
		CHECK_FIELD(persistent_data, "the BIOS NVRAM has changed since it was saved");
#undef CHECK_FIELD
		return nullptr;
	}

	void FillHeaderBase(Header* header)
	{
		std::memcpy(header->magic, STATE_MAGIC, sizeof(STATE_MAGIC));
		header->format_version = FORMAT_VERSION;
		header->header_size = sizeof(Header);
		header->save_version = g_SaveVersion;
		header->platform = PLATFORM_WINDOWS_X64;
		StringUtil::Strlcpy(header->build, LIBRETRO_CORE_VERSION, sizeof(header->build));
	}

	bool IsNormalContext(retro_environment_t environment)
	{
		int context = RETRO_SAVESTATE_CONTEXT_NORMAL;
		// A frontend without the query only makes normal saves.
		if (!environment(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &context))
			return true;
		return context == RETRO_SAVESTATE_CONTEXT_NORMAL;
	}

	// CPU thread, at the state boundary: the measurement of the VM's current state, taken once
	// per state generation. Frontends query the size several times before they save.
	const Measurement* Measure(Error* error)
	{
		const u64 generation = LibretroSession::GetStateGeneration();
		if (s_measurement.has_value() && s_measurement->generation == generation)
			return &*s_measurement;

		s_measurement.reset();
		Measurement measurement;
		measurement.generation = generation;
		if (!SaveStateMemory::Measure(&measurement.plan, error))
			return nullptr;
		if (measurement.plan.lengths.size() > MAX_SEGMENTS)
		{
			Error::SetStringFmt(error, "The state has {} segments; the format holds {}.", measurement.plan.lengths.size(), MAX_SEGMENTS);
			return nullptr;
		}

		u64 total = DATA_START;
		for (const u32 length : measurement.plan.lengths)
			total += AlignSegment(length);
		if (total > std::numeric_limits<u32>::max())
		{
			Error::SetString(error, "The state exceeds 4 GiB.");
			return nullptr;
		}
		measurement.total = static_cast<u32>(total);
		s_measurement = std::move(measurement);
		return &*s_measurement;
	}

	// Frontend thread: the container checks that need no VM: format, build and a table whose
	// segments lie in order within the used bytes and match their CRCs.
	bool CheckContainer(std::span<const u8> state, Header* header, std::vector<SegmentEntry>* table, Error* error)
	{
		if (state.size() < DATA_START)
		{
			Error::SetString(error, "The state is truncated.");
			return false;
		}

		std::memcpy(header, state.data(), sizeof(Header));
		if (std::memcmp(header->magic, STATE_MAGIC, sizeof(STATE_MAGIC)) != 0 || header->format_version != FORMAT_VERSION ||
			header->header_size != sizeof(Header))
		{
			Error::SetString(error, "The data is not a state of this core's format.");
			return false;
		}
		if (header->save_version != g_SaveVersion || header->platform != PLATFORM_WINDOWS_X64 ||
			std::memchr(header->build, 0, sizeof(header->build)) == nullptr ||
			std::strcmp(header->build, LIBRETRO_CORE_VERSION) != 0)
		{
			Error::SetString(error, "The state was saved by another build of the core.");
			return false;
		}
		if (header->segment_count > MAX_SEGMENTS || header->used_length < DATA_START || header->used_length > state.size())
		{
			Error::SetString(error, "The state's table is malformed or the state is truncated.");
			return false;
		}

		table->resize(header->segment_count);
		std::memcpy(table->data(), state.data() + sizeof(Header), header->segment_count * sizeof(SegmentEntry));
		u64 end = DATA_START;
		for (const SegmentEntry& entry : *table)
		{
			if (std::memchr(entry.name, 0, sizeof(entry.name)) == nullptr || entry.offset < end ||
				static_cast<u64>(entry.offset) + entry.length > header->used_length)
			{
				Error::SetString(error, "The state's segments overlap or lie outside the state.");
				return false;
			}
			const uLong crc = crc32(crc32(0L, Z_NULL, 0), state.data() + entry.offset, entry.length);
			if (static_cast<u32>(crc) != entry.crc32)
			{
				Error::SetStringFmt(error, "The state's {} segment is corrupt.", entry.name);
				return false;
			}
			end = static_cast<u64>(entry.offset) + entry.length;
		}
		return true;
	}

	// CPU thread: whether a memory card command spans the point the state is taken at. PS2 card
	// commands complete within one SIO2 packet, and the SIO2 FIFOs belong to the saved SIO2
	// state; only a PS1 card transfer on SIO0 runs byte by byte over several accesses.
	bool IsCardCommandInProgress()
	{
		return g_Sio0.sioMode == SioMode::MEMCARD;
	}
} // namespace

void LibretroState::ResetSession()
{
	s_measurement.reset();
	s_file_hashes.reset();
}

size_t LibretroState::GetSize()
{
	size_t size = 0;
	Error error;
	const LibretroSession::StateResult result = LibretroSession::RunStateOperation(false,
		[&size](Error* error) {
			const Measurement* measurement = Measure(error);
			if (!measurement)
				return LibretroSession::StateResult::Rejected;
			size = measurement->total;
			return LibretroSession::StateResult::Done;
		},
		&error);
	if (result != LibretroSession::StateResult::Done)
	{
		// No VM yet is the expected answer before content runs; anything else is reported.
		if (LibretroSession::IsActive())
			Console.ErrorFmt("Save states are unavailable: {}", error.GetDescription());
		return 0;
	}

	DevCon.WriteLnFmt("Save state size: {} bytes (VU1 thread {})", size, THREAD_VU1 ? "on" : "off");
	return size;
}

bool LibretroState::Save(retro_environment_t environment, void* data, size_t size)
{
	if (!IsNormalContext(environment))
	{
		Console.Warning("Only normal save states are supported; the frontend asked for another kind");
		return false;
	}

	const std::span<u8> buffer(static_cast<u8*>(data), size);
	Error error;
	const LibretroSession::StateResult result = LibretroSession::RunStateOperation(false,
		[buffer](Error* error) {
			if (IsCardCommandInProgress())
			{
				Error::SetString(error, "A memory card command is in progress.");
				return LibretroSession::StateResult::Rejected;
			}

			// The state as it stands now, which a size query since the last change measured.
			const Measurement* measurement = Measure(error);
			if (!measurement)
				return LibretroSession::StateResult::Rejected;
			if (buffer.size() < measurement->total)
			{
				Error::SetStringFmt(error, "A {}-byte buffer cannot hold the {}-byte state.", buffer.size(), measurement->total);
				return LibretroSession::StateResult::Rejected;
			}
			const std::span<u8> state = buffer.first(measurement->total);
			const SaveStateMemory::Plan& plan = measurement->plan;

			Header header = {};
			FillHeaderBase(&header);
			if (!GetIdentity(&header.identity, error))
				return LibretroSession::StateResult::Rejected;

			std::vector<SaveStateMemory::Segment> segments;
			if (!SaveStateMemory::GetSegments(&segments, error))
				return LibretroSession::StateResult::Rejected;

			std::vector<SegmentEntry> table(plan.lengths.size());
			u32 offset = DATA_START;
			for (size_t i = 0; i < plan.lengths.size(); i++)
			{
				const u32 length = plan.lengths[i];
				if (!SaveStateMemory::SaveSegment(plan, i, state.subspan(offset, length), error))
					return LibretroSession::StateResult::Rejected;

				SegmentEntry& entry = table[i];
				StringUtil::Strlcpy(entry.name, segments[i].name, sizeof(entry.name));
				entry.offset = offset;
				entry.length = length;
				entry.crc32 = static_cast<u32>(crc32(crc32(0L, Z_NULL, 0), state.data() + offset, length));
				offset = AlignSegment(offset + length);
			}

			header.segment_count = static_cast<u32>(table.size());
			header.used_length = offset;
			std::memcpy(state.data(), &header, sizeof(header));
			std::memset(state.data() + sizeof(Header), 0, DATA_START - sizeof(Header));
			std::memcpy(state.data() + sizeof(Header), table.data(), table.size() * sizeof(SegmentEntry));

			// Unused bytes, between segments and after the last, are Zero.
			for (size_t i = 0; i < table.size(); i++)
			{
				const u32 gap_start = table[i].offset + table[i].length;
				const u32 gap_end = (i + 1 < table.size()) ? table[i + 1].offset : static_cast<u32>(state.size());
				std::memset(state.data() + gap_start, 0, gap_end - gap_start);
			}
			return LibretroSession::StateResult::Done;
		},
		&error);

	if (result != LibretroSession::StateResult::Done)
	{
		Console.ErrorFmt("The state could not be saved: {}", error.GetDescription());
		return false;
	}
	return true;
}

LibretroState::LoadResult LibretroState::Load(retro_environment_t environment, const void* data, size_t size, std::string* failure)
{
	if (!IsNormalContext(environment))
	{
		Console.Warning("Only normal save states are supported; the frontend asked for another kind");
		return LoadResult::Rejected;
	}

	const std::span<const u8> state(static_cast<const u8*>(data), size);
	Header header;
	std::vector<SegmentEntry> table;
	Error error;
	if (!CheckContainer(state, &header, &table, &error))
	{
		Console.ErrorFmt("The state could not be loaded: {}", error.GetDescription());
		return LoadResult::Rejected;
	}

	std::vector<std::span<const u8>> segments;
	for (const SegmentEntry& entry : table)
		segments.push_back(state.subspan(entry.offset, entry.length));

	const LibretroSession::StateResult result = LibretroSession::RunStateOperation(true,
		[&header, &segments](Error* error) {
			if (MemcardBusy::IsBusy())
			{
				Error::SetString(error, "A memory card is being written.");
				return LibretroSession::StateResult::Rejected;
			}

			Identity current;
			if (!GetIdentity(&current, error))
				return LibretroSession::StateResult::Rejected;
			if (const char* mismatch = DescribeIdentityMismatch(header.identity, current))
			{
				Error::SetStringFmt(error, "The state does not fit the running game: {}.", mismatch);
				return LibretroSession::StateResult::Rejected;
			}

			if (!SaveStateMemory::Validate(segments, error))
				return LibretroSession::StateResult::Rejected;

			return SaveStateMemory::Load(segments, error) ? LibretroSession::StateResult::Done : LibretroSession::StateResult::Failed;
		},
		&error);

	switch (result)
	{
		case LibretroSession::StateResult::Done:
			Console.WriteLn("State loaded");
			return LoadResult::Loaded;
		case LibretroSession::StateResult::Rejected:
			Console.ErrorFmt("The state could not be loaded: {}", error.GetDescription());
			return LoadResult::Rejected;
		case LibretroSession::StateResult::Failed:
			*failure = fmt::format("The state failed while it was being loaded: {}", error.GetDescription());
			return LoadResult::Failed;
	}
	return LoadResult::Failed;
}
