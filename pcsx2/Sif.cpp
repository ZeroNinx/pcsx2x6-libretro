// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#define _PC_	// disables MIPS opcode macros.

#include "R3000A.h"
#include "Common.h"
#include "Sif.h"

void sifReset()
{
	std::memset(&sif0, 0, sizeof(sif0));
	std::memset(&sif1, 0, sizeof(sif1));
}

bool SaveStateBase::sifFreeze()
{
	if (!FreezeTag("SIFdma"))
		return false;

	// FIFO positions index its 128 words; the IOP counter is a 20-bit transfer count.
	for (_sif* sif : {&sif0, &sif1})
	{
		NoteRange(*sif, sif->fifo.readPos, 0, FIFO_SIF_W - 1);
		NoteRange(*sif, sif->fifo.writePos, 0, FIFO_SIF_W - 1);
		NoteRange(*sif, sif->fifo.size, 0, FIFO_SIF_W);
		NoteRange(*sif, sif->iop.writeJunk, 0, 3);
		NoteRange(*sif, sif->iop.counter, 0, 0xFFFFF);
		Freeze(*sif);
	}
	return IsOkay();
}
