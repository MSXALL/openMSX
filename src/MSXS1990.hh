// $Id$

#ifndef __MSXS1990_HH__
#define __MSXS1990_HH__

#include "MSXDevice.hh"
#include "FirmwareSwitch.hh"

namespace openmsx {

/**
 * This class implements the MSX-engine found in a MSX Turbo-R (S1990)
 *
 * TODO explanation
 */
class MSXS1990 : public MSXDevice
{
public:
	MSXS1990(const XMLElement& config, const EmuTime& time);
	virtual ~MSXS1990();

	virtual void reset(const EmuTime& time);
	virtual byte readIO(byte port, const EmuTime& time);
	virtual void writeIO(byte port, byte value, const EmuTime& time);

private:
	void setCPUStatus(byte value);

	FirmwareSwitch firmwareSwitch;
	byte registerSelect;
	byte cpuStatus;
};

} // namespace openmsx

#endif // __MSXS1990_HH__
