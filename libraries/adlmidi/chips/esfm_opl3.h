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


#ifndef ESFM_OPL3_H
#define ESFM_OPL3_H

#include "opl_chip_base.h"

// Forward declaration for the C struct
typedef struct _esfm_chip esfm_chip;

class ESFM final : public OPLChipBaseT<ESFM>
{
public:
	// These static constants are used by OPLChipBaseT for resampling gain
	// The base class will look for these exact names.
	// You can adjust these values to match the ESFM chip's output level.
	// A value of 1 is a neutral starting point. You might need to increase
	// it if the emulator is too quiet, or decrease it if it's too loud/clipping.
	static const int resamplerPreAmplify = 1;  // Amplifies input to the resampler
	static const int resamplerPostAttenuate = 1; // Attenuates output from the resampler

	ESFM();
	~ESFM() override;

	bool canRunAtPcmRate() const override { return true; }
	void setRate(uint32_t rate) override;
	void reset() override;
	void writeReg(uint16_t addr, uint8_t data) override;
	void writePan(uint16_t addr, uint8_t data) override;
	void nativePreGenerate() override;
	void nativePostGenerate() override;
	void nativeGenerate(int16_t *frame) override;
	const char *emulatorName() override;
	ChipType chipType() override;
private:
	// This will be implemented in the .cpp file
	struct ChipData;
	ChipData *m_chipData;
};

#endif // ESFM_OPL3_H