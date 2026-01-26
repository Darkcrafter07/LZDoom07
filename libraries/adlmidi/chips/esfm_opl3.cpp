/*
 * Interfaces over Yamaha OPL3 (YMF262) chip emulators
 *
 * Copyright (c) 2017-2020 Vitaly Novichkov (Wohlstand)
 * ESSFM emulator by Kagamiin and ADLMidi LZDoom integration by Darkcrafter07
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "esfm_opl3.h"
#include <memory>

// --- Handle the C API ---
extern "C"
{
#include "esfmu/esfm.h"
}

// PIMPL Implementation
struct ESFM::ChipData
{
	esfm_chip chip;
};

// ESFM Class Implementation
ESFM::ESFM() : m_chipData(new ChipData())
{
	// Initialize the internal state of the ESFM chip
	ESFM_init(&m_chipData->chip);

	// Standard OPL3 initialization sequence.
	ESFM_write_reg(&m_chipData->chip, 0x004, 0x60); // Reset timers
	ESFM_write_reg(&m_chipData->chip, 0x004, 0x80); // Clear reset
	ESFM_write_reg(&m_chipData->chip, 0x105, 0);   // Disable OPL3 mode first
	ESFM_write_reg(&m_chipData->chip, 0x105, 1);   // Enable OPL3 mode
}

ESFM::~ESFM()
{
	delete m_chipData;
}

void ESFM::setRate(uint32_t rate)
{
	// This function is called by the base class to set the *target* output sample rate.
	// The ESFM emulator itself runs at a fixed internal rate (e.g., 49716 Hz).
	// The OPLChipBaseT template handles the resampling from the internal rate
	// to the target rate. We just need to call the base class implementation.
	OPLChipBaseT<ESFM>::setRate(rate);
}

void ESFM::reset()
{
	// Re-initialize the chip, resetting all registers and state.
	ESFM_init(&m_chipData->chip);
	// Re-apply the initial state, as the chip might be in an unknown state after init.
	ESFM_write_reg(&m_chipData->chip, 0x105, 1);   // Re-enable OPL3 mode
}

void ESFM::writeReg(uint16_t addr, uint8_t data)
{
	// Write a register to the ESFM chip.
	// The 16-bit `addr` from AdlMidi matches the format used by ESFM_write_reg.
	// (Bank 0: 0x000-0x0FF, Bank 1: 0x100-0x1FF)
	ESFM_write_reg(&m_chipData->chip, addr, data);
}

void ESFM::writePan(uint16_t addr, uint8_t data)
{
	// ESFM hardware likely doesn't have built-in panning.
	// The GZDoom audio system will handle panning based on the stereo audio we provide.
	// This function is a hook for emulators that do support hardware panning.
	// We can ignore it for now.
	(void)addr;
	(void)data;
}

void ESFM::nativePreGenerate()
{
	// Not used for this emulator's generation model (frame-by-frame).
}

void ESFM::nativePostGenerate()
{
	// Not used for this emulator's generation model (frame-by-frame).
}

void ESFM::nativeGenerate(int16_t *frame)
{
	// Generate a single stereo frame (two int16_t samples: left, right).
	// The ESFM_generate function takes a pointer to a buffer.
	// Based on `esfm.h`, `void ESFM_generate(esfm_chip *chip, int16_t *buf);`
	// The buffer must hold at least 2 samples for stereo.
	ESFM_generate(&m_chipData->chip, frame);
}

const char *ESFM::emulatorName()
{
	return "ESFM (ESS FM)";
}

OPLChipBase::ChipType ESFM::chipType()
{
	// Assuming it's compatible with the OPL3 chip type.
	// You might need to create a new ChipType if it's significantly different.
	return CHIPTYPE_OPL3;
}