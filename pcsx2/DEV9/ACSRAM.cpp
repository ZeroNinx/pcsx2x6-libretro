#include "ACSRAM.h"
#include "IopMem.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "ps2/BiosTools.h"
#include "ACMACROS.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

u8 ACSRAM::buffer[ACSRAM_MAX_SIZE];
std::string ACSRAM::filepath = "";
std::string ACSRAM::initial_filepath = "";

// Set when an existing SRAM image could not be read. Saving the blank buffer would wipe
// data that may still be recoverable, so the session leaves the file alone.
static bool s_write_protected = false;

enum class ImageRead {
    Loaded,
    Missing,
    Failed,
};

// Opens an SRAM image read-only. Only an image that is reported as absent sets
// `missing`; any other failure means the image may exist and must be protected.
static FileSystem::ManagedCFilePtr OpenImageForReading(const std::string& path, bool* missing, Error* error) {
    FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(path.c_str(), "rb");
    // Capture errno before any other call can change it.
    const int open_errno = errno;
    *missing = !fp && open_errno == ENOENT;
    if (!fp)
        Error::SetErrno(error, open_errno);
    return fp;
}

// Reads the first 32 KiB of `path` into the buffer. A partial image never reaches the game.
static ImageRead ReadImage(const std::string& path) {
    if (FileSystem::DirectoryExists(path.c_str())) {
        Console.ErrorFmt("ACSRAM: '{}' is a directory. SRAM will not be saved this session.", path);
        return ImageRead::Failed;
    }

    bool missing;
    Error error;
    const FileSystem::ManagedCFilePtr fp = OpenImageForReading(path, &missing, &error);
    if (missing)
        return ImageRead::Missing;
    if (!fp) {
        Console.ErrorFmt("ACSRAM: cannot open '{}': {}. The image is left untouched and will not be saved this session.",
            path, error.GetDescription());
        return ImageRead::Failed;
    }

    // Only the first 32 KiB belong to the SRAM; anything after them is ignored.
    const size_t size = std::fread(ACSRAM::buffer, 1, ACSRAM_MAX_SIZE, fp.get());
    if (size != ACSRAM_MAX_SIZE) {
        ACSRAM::Clear(0x0);
        if (std::ferror(fp.get()))
            Console.ErrorFmt("ACSRAM: cannot read '{}'. The image is left untouched and will not be saved this session.", path);
        else
            Console.ErrorFmt("ACSRAM: '{}' holds {} bytes, expected {}. The image is left untouched and will not be saved this session.",
                path, size, ACSRAM_MAX_SIZE);
        return ImageRead::Failed;
    }

    return ImageRead::Loaded;
}

int ACSRAM::ReadFile() {
    // Every session starts blank and decides its own protection.
    ACSRAM::Clear(0x0);
    s_write_protected = false;

    // No manifest set a path: this session has no arcade SRAM.
    if (ACSRAM::filepath.empty())
        return 0;

    // unlike PS2 NVRAM, which we can do a generic damage/invalid detection attempt.
    // ACSRAM is per-game, and not all of them strongly check if it has valid data
    // this is even problematic on real hardware, where such games can behave erratically when ran on a machine whose ACSRAM has payload of another game
    ImageRead result = ReadImage(ACSRAM::filepath);
    if (result == ImageRead::Loaded) {
        Console.WriteLn(Color_StrongCyan, "%-16s OK", __FUNCTION__);
        return 1;
    }

    // An existing save always wins; initial data is only consulted when there is none.
    if (result == ImageRead::Missing && !ACSRAM::initial_filepath.empty()) {
        result = ReadImage(ACSRAM::initial_filepath);
        if (result == ImageRead::Loaded) {
            Console.WriteLnFmt(Color_StrongCyan, "ACSRAM: imported initial SRAM from '{}'", ACSRAM::initial_filepath);
            return 1;
        }
    }

    if (result == ImageRead::Missing) {
        Console.WarningFmt("ACSRAM: no SRAM image at '{}', starting with a blank one", ACSRAM::filepath);
        return 0;
    }

    // Unreadable initial data is not replaced by a blank save either, so the import is
    // retried once the source is fixed.
    s_write_protected = true;
    return 0;
}

// Removes the temporary image of a failed save. The file must already be closed, because
// Windows cannot delete a file that is still open.
static void DeleteTemporaryImage(const std::string& path) {
    Error error;
    if (!FileSystem::DeleteFilePath(path.c_str(), &error))
        Console.ErrorFmt("ACSRAM: cannot remove the incomplete image '{}': {}", path, error.GetDescription());
}

// A new image is written beside its destination and renamed into place, so a failed save
// never leaves a partial image that a later session would have to reject.
static bool CreateImage(Error* error) {
    const std::string temporary_path = ACSRAM::filepath + ".tmp";
    FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(temporary_path.c_str(), "wb", error);
    if (!fp)
        return false;

    bool written = std::fwrite(ACSRAM::buffer, ACSRAM_MAX_SIZE, 1, fp.get()) == 1 && std::fflush(fp.get()) == 0;
    int write_errno = errno;
    // Closing can still fail to write buffered data, so its result counts as part of the save.
    if (std::fclose(fp.release()) != 0 && written) {
        written = false;
        write_errno = errno;
    }
    if (!written) {
        Error::SetErrno(error, write_errno);
        DeleteTemporaryImage(temporary_path);
        return false;
    }

    if (!FileSystem::RenamePath(temporary_path.c_str(), ACSRAM::filepath.c_str(), error)) {
        DeleteTemporaryImage(temporary_path);
        return false;
    }

    return true;
}

// An existing image is updated in place; it is never recreated, so a file that cannot be
// opened for writing is not truncated instead.
static bool UpdateImage(Error* error) {
    const FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(ACSRAM::filepath.c_str(), "r+b", error);
    if (!fp)
        return false;

    if (std::fwrite(ACSRAM::buffer, ACSRAM_MAX_SIZE, 1, fp.get()) != 1 || std::fflush(fp.get()) != 0) {
        Error::SetErrno(error, errno);
        return false;
    }

    return true;
}

int ACSRAM::WriteFile() {
    if (ACSRAM::filepath.empty())
        return 0;

    if (s_write_protected) {
        Console.Warning("ACSRAM: skipping sram save to preserve existing image");
        return 0;
    }

    bool missing;
    Error error;
    {
        const FileSystem::ManagedCFilePtr existing = OpenImageForReading(ACSRAM::filepath, &missing, &error);
        if (!existing && !missing) {
            ERROR_LOG("Cannot check existing ACSRAM image '{}': {}. It is left untouched.", ACSRAM::filepath, error.GetDescription());
            return EIO;
        }
        if (existing) {
            std::vector<u8> current(ACSRAM_MAX_SIZE);
            if (std::fread(current.data(), 1, ACSRAM_MAX_SIZE, existing.get()) == ACSRAM_MAX_SIZE &&
                std::memcmp(current.data(), ACSRAM::buffer, ACSRAM_MAX_SIZE) == 0) {
                DEV_LOG("ACSRAM has not changed, skip writing to disk.");
                return 0;
            }
        }
    }

    if (!(missing ? CreateImage(&error) : UpdateImage(&error))) {
        ERROR_LOG("Failed to save ACSRAM to '{}': {}", ACSRAM::filepath, error.GetDescription());
        return EIO;
    }

    INFO_LOG("ACSRAM saved to {}.", Path::GetFileName(ACSRAM::filepath));
    return 0;
}

#define OOB_REPORT(T) Console.Error("%s: out of bound index: %08X", __FUNCTION__, T);
#define GET_SRAM_OFF(t) ((t - ACSRAM_ADDR_BASE)/2) // u8 buffer on u16 MMIO, halve the address to get real offset

void ACSRAM::Clear(u8 fillerbyte) {
    std::memset(ACSRAM::buffer, fillerbyte, sizeof(ACSRAM::buffer));
}

u8 ACSRAM::Read8(u32 addr) {
    u32 T = GET_SRAM_OFF(addr);
    if (T < ACSRAM_MAX_SIZE) {
        ACSRAM_LOG("read8  [%04X]:%02X", T, ACSRAM::buffer[T]);
        return ACSRAM::buffer[T];
    } else OOB_REPORT(T);
    return 0;
}

u16 ACSRAM::Read16(u32 addr) {
    u32 T = GET_SRAM_OFF(addr);
    if (T < ACSRAM_MAX_SIZE) {
        ACSRAM_LOG("read16 [%04X]:%02X", T, ACSRAM::buffer[T]);
        return ACSRAM::buffer[T];
    } else OOB_REPORT(T);
    return 0;
}

void ACSRAM::Write16(u32 addr, u16 val) {
    u32 T = GET_SRAM_OFF(addr);
    if (T < ACSRAM_MAX_SIZE) {
        ACSRAM_LOG("write16 [%04X]=%02X", T, val);
        ACSRAM::buffer[T] = val;
    } else OOB_REPORT(T);
}
