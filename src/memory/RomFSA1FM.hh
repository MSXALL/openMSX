// $Id$

#ifndef __ROMFSA1FM1_HH__
#define __ROMFSA1FM1_HH__

#include "MSXRom.hh"
#include "Rom8kBBlocks.hh"
#include "SRAM.hh"
#include "FirmwareSwitch.hh"

namespace openmsx {

class FSA1FMRam
{
public:
	static byte* getSRAM(const XMLElement& config);

private:
	FSA1FMRam(const XMLElement& config);
	~FSA1FMRam();

	SRAM sram;
};

class RomFSA1FM1 : public MSXRom
{
public:
	RomFSA1FM1(const XMLElement& config, const EmuTime& time, auto_ptr<Rom> rom);
	virtual ~RomFSA1FM1();
	
	virtual void reset(const EmuTime& time);
	virtual byte readMem(word address, const EmuTime& time);
	virtual const byte* getReadCacheLine(word address) const;
	virtual void writeMem(word address, byte value,
			      const EmuTime& time);
	virtual byte* getWriteCacheLine(word address) const;

private:
	byte* sram;	// 8kb (shared) sram
	FirmwareSwitch firmwareSwitch;
};

class RomFSA1FM2 : public Rom8kBBlocks
{
public:
	RomFSA1FM2(const XMLElement& config, const EmuTime& time, auto_ptr<Rom> rom);
	virtual ~RomFSA1FM2();
	
	virtual void reset(const EmuTime& time);
	virtual byte readMem(word address, const EmuTime& time);
	virtual const byte* getReadCacheLine(word address) const;
	virtual void writeMem(word address, byte value,
			      const EmuTime& time);
	virtual byte* getWriteCacheLine(word address) const;

private:
	void changeBank(byte region, byte bank);

	byte control;
	byte* sram;	// 8kb (shared) sram
	byte bankSelect[8];
	bool isRam[8];
	bool isEmpty[8];
};

} // namespace openmsx

#endif
