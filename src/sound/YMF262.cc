// $Id$

/*
 *
 * File: ymf262.c - software implementation of YMF262
 *                  FM sound generator type OPL3
 *
 * Copyright (C) 2003 Jarek Burczynski
 *
 * Version 0.2
 *
 *
 * Revision History:
 *
 * 03-03-2003: initial release
 *  - thanks to Olivier Galibert and Chris Hardy for YMF262 and YAC512 chips
 *  - thanks to Stiletto for the datasheets
 *
 *
 *
 * differences between OPL2 and OPL3 not documented in Yamaha datahasheets:
 * - sinus table is a little different: the negative part is off by one...
 *
 * - in order to enable selection of four different waveforms on OPL2
 *   one must set bit 5 in register 0x01(test).
 *   on OPL3 this bit is ignored and 4-waveform select works *always*.
 *   (Don't confuse this with OPL3's 8-waveform select.)
 *
 * - Envelope Generator: all 15 x rates take zero time on OPL3
 *   (on OPL2 15 0 and 15 1 rates take some time while 15 2 and 15 3 rates
 *   take zero time)
 *
 * - channel calculations: output of operator 1 is in perfect sync with
 *   output of operator 2 on OPL3; on OPL and OPL2 output of operator 1
 *   is always delayed by one sample compared to output of operator 2
 *
 *
 * differences between OPL2 and OPL3 shown in datasheets:
 * - YMF262 does not support CSM mode
 */

#include "YMF262.hh"
#include "SoundDevice.hh"
#include "EmuTimer.hh"
#include "Resample.hh"
#include "IRQHelper.hh"
#include "FixedPoint.hh"
#include "SimpleDebuggable.hh"
#include "MSXMotherBoard.hh"
#include <cmath>
#include <cstring>

namespace openmsx {

class YMF262Debuggable : public SimpleDebuggable
{
public:
	YMF262Debuggable(MSXMotherBoard& motherBoard, YMF262Impl& ymf262);
	virtual byte read(unsigned address);
	virtual void write(unsigned address, byte value, const EmuTime& time);
private:
	YMF262Impl& ymf262;
};


enum EnvelopeState {
	EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE, EG_OFF
};

class YMF262Channel;

/** 16.16 fixed point type for frequency calculations
 */
typedef FixedPoint<16> FreqIndex;

static inline FreqIndex fnumToIncrement(int block_fnum)
{
	// opn phase increment counter = 20bit
	// chip works with 10.10 fixed point, while we use 16.16
	int block = (block_fnum & 0x1C00) >> 10;
	return FreqIndex(block_fnum & 0x03FF) >> (11 - block);
}

class YMF262Slot
{
public:
	YMF262Slot();
	inline int op_calc(unsigned phase, int pm, byte LFO_AM);
	inline void FM_KEYON(byte key_set);
	inline void FM_KEYOFF(byte key_clr);
	inline void advanceEnvelopeGenerator(unsigned eg_cnt);
	inline void advancePhaseGenerator(YMF262Channel& ch, unsigned LFO_PM);

	/** Sets the amount of feedback [0..7]
	 */
	void setFeedbackShift(byte value) {
		fb_shift = value ? 9 - value : 0;
	}

	// Phase Generator
	FreqIndex Cnt;	// frequency counter
	FreqIndex Incr;	// frequency counter step
	int* connect;	// slot output pointer
	int op1_out[2];	// slot1 output for feedback

	// Envelope Generator
	unsigned TL;	// total level: TL << 2
	int TLL;	// adjusted now TL
	int volume;	// envelope counter
	int sl;		// sustain level: sl_tab[SL]

	unsigned* wavetable; // waveform select

	unsigned eg_m_ar;// (attack state)
	unsigned eg_m_dr;// (decay state)
	unsigned eg_m_rr;// (release state)
	byte eg_sh_ar;	// (attack state)
	byte eg_sel_ar;	// (attack state)
	byte eg_sh_dr;	// (decay state)
	byte eg_sel_dr;	// (decay state)
	byte eg_sh_rr;	// (release state)
	byte eg_sel_rr;	// (release state)

	byte key;	// 0 = KEY OFF, >0 = KEY ON

	byte fb_shift;	// PG: feedback shift value
	bool CON;	// PG: connection (algorithm) type
	bool eg_type;	// EG: percussive/non-percussive mode
	EnvelopeState state; // EG: phase type

	// LFO
	byte AMmask;	// LFO Amplitude Modulation enable mask
	bool vib;	// LFO Phase Modulation enable flag (active high)

	byte waveform_number; // waveform select

	byte ar;	// attack rate: AR<<2
	byte dr;	// decay rate:  DR<<2
	byte rr;	// release rate:RR<<2
	byte KSR;	// key scale rate
	byte ksl;	// keyscale level
	byte ksr;	// key scale rate: kcode>>KSR
	byte mul;	// multiple: mul_tab[ML]
};

class YMF262Channel
{
public:
	YMF262Channel();
	void chan_calc(byte LFO_AM);
	void chan_calc_ext(byte LFO_AM);
	void CALC_FCSLOT(YMF262Slot& slot);

	YMF262Slot slots[2];

	int block_fnum;	// block+fnum
	FreqIndex fc;	// Freq. Increment base
	int ksl_base;	// KeyScaleLevel Base step
	byte kcode;	// key code (for key scaling)

	// there are 12 2-operator channels which can be combined in pairs
	// to form six 4-operator channel, they are:
	//  0 and 3,
	//  1 and 4,
	//  2 and 5,
	//  9 and 12,
	//  10 and 13,
	//  11 and 14
	byte extended;	// set to 1 if this channel forms up a 4op channel with another channel(only used by first of pair of channels, ie 0,1,2 and 9,10,11)
};

class YMF262Impl : private SoundDevice, private EmuTimerCallback, private Resample
{
public:
	YMF262Impl(MSXMotherBoard& motherBoard, const std::string& name,
	       const XMLElement& config, const EmuTime& time);
	virtual ~YMF262Impl();

	void reset(const EmuTime& time);
	void writeReg(int r, byte v, const EmuTime& time);
	byte readReg(int reg);
	byte peekReg(int reg) const;
	byte readStatus();
	byte peekStatus() const;

private:
	// SoundDevice
	virtual void setOutputRate(unsigned sampleRate);
	virtual void generateChannels(int** bufs, unsigned num);
	virtual bool updateBuffer(unsigned length, int* buffer,
		const EmuTime& time, const EmuDuration& sampDur);

	// Resample
	virtual bool generateInput(int* buffer, unsigned num);

	void callback(byte flag);

	void writeRegForce(int r, byte v, const EmuTime& time);
	void init_tables(void);
	void setStatus(byte flag);
	void resetStatus(byte flag);
	void changeStatusMask(byte flag);
	void advance_lfo();
	void advance();

	inline int genPhaseHighHat();
	inline int genPhaseSnare();
	inline int genPhaseCymbal();

	void chan_calc_rhythm();
	void set_mul(byte sl, byte v);
	void set_ksl_tl(byte sl, byte v);
	void set_ar_dr(byte sl, byte v);
	void set_sl_rr(byte sl, byte v);
	void update_channels(YMF262Channel& ch);
	bool checkMuteHelper();
	int adjust(int x);

	friend class YMF262Debuggable;
	const std::auto_ptr<YMF262Debuggable> debuggable;

	// Bitmask for register 0x04
	static const int R04_ST1       = 0x01; // Timer1 Start
	static const int R04_ST2       = 0x02; // Timer2 Start
	static const int R04_MASK_T2   = 0x20; // Mask Timer2 flag
	static const int R04_MASK_T1   = 0x40; // Mask Timer1 flag
	static const int R04_IRQ_RESET = 0x80; // IRQ RESET

	// Bitmask for status register
	static const int STATUS_T2      = R04_MASK_T2;
	static const int STATUS_T1      = R04_MASK_T1;
	// Timers (see EmuTimer class for details about timing)
	EmuTimerOPL4_1 timer1; //  80.8us OPL4  ( 80.5us OPL3)
	EmuTimerOPL4_2 timer2; // 323.1us OPL4  (321.8us OPL3)

	IRQHelper irq;

	int chanout[18]; // 18 channels

	byte reg[512];
	YMF262Channel channels[18];	// OPL3 chips have 18 channels

	unsigned pan[18*4];		// channels output masks 4 per channel
	                                //    0xffffffff = enable
	unsigned eg_cnt;		// global envelope generator counter
	unsigned noise_rng;		// 23 bit noise shift register

	// LFO
	typedef FixedPoint< 6> LFOAMIndex;
	typedef FixedPoint<10> LFOPMIndex;
	LFOAMIndex lfo_am_cnt;
	LFOPMIndex lfo_pm_cnt;
	byte LFO_AM;
	byte LFO_PM;
	bool lfo_am_depth;
	byte lfo_pm_depth_range;

	byte rhythm;			// Rhythm mode
	bool nts;			// NTS (note select)
	bool OPL3_mode;			// OPL3 extension enable flag

	byte status;			// status flag
	byte status2;
	byte statusMask;		// status mask
};


// envelope output entries
static const int ENV_BITS    = 10;
static const int ENV_LEN     = 1 << ENV_BITS;
static const double ENV_STEP = 128.0 / ENV_LEN;

static const int MAX_ATT_INDEX = (1 << (ENV_BITS - 1)) - 1; //511
static const int MIN_ATT_INDEX = 0;

// sinwave entries
static const int SIN_BITS = 10;
static const int SIN_LEN  = 1 << SIN_BITS;
static const int SIN_MASK = SIN_LEN - 1;

static const int TL_RES_LEN = 256;	// 8 bits addressing (real chip)

// register number to channel number , slot offset
static const byte SLOT1 = 0;
static const byte SLOT2 = 1;


// mapping of register number (offset) to slot number used by the emulator
static const int slot_array[32] = {
	 0,  2,  4,  1,  3,  5, -1, -1,
	 6,  8, 10,  7,  9, 11, -1, -1,
	12, 14, 16, 13, 15, 17, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1
};


// key scale level
// table is 3dB/octave , DV converts this into 6dB/octave
// 0.1875 is bit 0 weight of the envelope counter (volume) expressed
// in the 'decibel' scale
#define DV(x) (int)(x / (0.1875 / 2.0))
static const unsigned ksl_tab[8 * 16] = {
	// OCT 0
	DV( 0.000), DV( 0.000), DV( 0.000), DV( 0.000),
	DV( 0.000), DV( 0.000), DV( 0.000), DV( 0.000),
	DV( 0.000), DV( 0.000), DV( 0.000), DV( 0.000),
	DV( 0.000), DV( 0.000), DV( 0.000), DV( 0.000),
	// OCT 1
	DV( 0.000), DV( 0.000), DV( 0.000), DV( 0.000),
	DV( 0.000), DV( 0.000), DV( 0.000), DV( 0.000),
	DV( 0.000), DV( 0.750), DV( 1.125), DV( 1.500),
	DV( 1.875), DV( 2.250), DV( 2.625), DV( 3.000),
	// OCT 2
	DV( 0.000), DV( 0.000), DV( 0.000), DV( 0.000),
	DV( 0.000), DV( 1.125), DV( 1.875), DV( 2.625),
	DV( 3.000), DV( 3.750), DV( 4.125), DV( 4.500),
	DV( 4.875), DV( 5.250), DV( 5.625), DV( 6.000),
	// OCT 3
	DV( 0.000), DV( 0.000), DV( 0.000), DV( 1.875),
	DV( 3.000), DV( 4.125), DV( 4.875), DV( 5.625),
	DV( 6.000), DV( 6.750), DV( 7.125), DV( 7.500),
	DV( 7.875), DV( 8.250), DV( 8.625), DV( 9.000),
	// OCT 4
	DV( 0.000), DV( 0.000), DV( 3.000), DV( 4.875),
	DV( 6.000), DV( 7.125), DV( 7.875), DV( 8.625),
	DV( 9.000), DV( 9.750), DV(10.125), DV(10.500),
	DV(10.875), DV(11.250), DV(11.625), DV(12.000),
	// OCT 5
	DV( 0.000), DV( 3.000), DV( 6.000), DV( 7.875),
	DV( 9.000), DV(10.125), DV(10.875), DV(11.625),
	DV(12.000), DV(12.750), DV(13.125), DV(13.500),
	DV(13.875), DV(14.250), DV(14.625), DV(15.000),
	// OCT 6
	DV( 0.000), DV( 6.000), DV( 9.000), DV(10.875),
	DV(12.000), DV(13.125), DV(13.875), DV(14.625),
	DV(15.000), DV(15.750), DV(16.125), DV(16.500),
	DV(16.875), DV(17.250), DV(17.625), DV(18.000),
	// OCT 7
	DV( 0.000), DV( 9.000), DV(12.000), DV(13.875),
	DV(15.000), DV(16.125), DV(16.875), DV(17.625),
	DV(18.000), DV(18.750), DV(19.125), DV(19.500),
	DV(19.875), DV(20.250), DV(20.625), DV(21.000)
};
#undef DV

// sustain level table (3dB per step)
// 0 - 15: 0, 3, 6, 9,12,15,18,21,24,27,30,33,36,39,42,93 (dB)
#define SC(db) (unsigned) (db * (2.0 / ENV_STEP))
static const unsigned sl_tab[16] = {
	SC( 0), SC( 1), SC( 2), SC(3 ), SC(4 ), SC(5 ), SC(6 ), SC( 7),
	SC( 8), SC( 9), SC(10), SC(11), SC(12), SC(13), SC(14), SC(31)
};
#undef SC


static const byte RATE_STEPS = 8;
static const byte eg_inc[15 * RATE_STEPS] = {
//cycle:0 1  2 3  4 5  6 7
	0,1, 0,1, 0,1, 0,1, //  0  rates 00..12 0 (increment by 0 or 1)
	0,1, 0,1, 1,1, 0,1, //  1  rates 00..12 1
	0,1, 1,1, 0,1, 1,1, //  2  rates 00..12 2
	0,1, 1,1, 1,1, 1,1, //  3  rates 00..12 3

	1,1, 1,1, 1,1, 1,1, //  4  rate 13 0 (increment by 1)
	1,1, 1,2, 1,1, 1,2, //  5  rate 13 1
	1,2, 1,2, 1,2, 1,2, //  6  rate 13 2
	1,2, 2,2, 1,2, 2,2, //  7  rate 13 3

	2,2, 2,2, 2,2, 2,2, //  8  rate 14 0 (increment by 2)
	2,2, 2,4, 2,2, 2,4, //  9  rate 14 1
	2,4, 2,4, 2,4, 2,4, // 10  rate 14 2
	2,4, 4,4, 2,4, 4,4, // 11  rate 14 3

	4,4, 4,4, 4,4, 4,4, // 12  rates 15 0, 15 1, 15 2, 15 3 for decay
	8,8, 8,8, 8,8, 8,8, // 13  rates 15 0, 15 1, 15 2, 15 3 for attack (zero time)
	0,0, 0,0, 0,0, 0,0, // 14  infinity rates for attack and decay(s)
};


#define O(a) (a * RATE_STEPS)
// note that there is no O(13) in this table - it's directly in the code
static const byte eg_rate_select[16 + 64 + 16] = {
	// Envelope Generator rates (16 + 64 rates + 16 RKS)
	// 16 infinite time rates
	O(14), O(14), O(14), O(14), O(14), O(14), O(14), O(14),
	O(14), O(14), O(14), O(14), O(14), O(14), O(14), O(14),

	// rates 00-12
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),
	O( 0), O( 1), O( 2), O( 3),

	// rate 13
	O( 4), O( 5), O( 6), O( 7),

	// rate 14
	O( 8), O( 9), O(10), O(11),

	// rate 15
	O(12), O(12), O(12), O(12),

	// 16 dummy rates (same as 15 3)
	O(12), O(12), O(12), O(12), O(12), O(12), O(12), O(12),
	O(12), O(12), O(12), O(12), O(12), O(12), O(12), O(12),
};
#undef O

//rate  0,    1,    2,    3,   4,   5,   6,  7,  8,  9,  10, 11, 12, 13, 14, 15
//shift 12,   11,   10,   9,   8,   7,   6,  5,  4,  3,  2,  1,  0,  0,  0,  0
//mask  4095, 2047, 1023, 511, 255, 127, 63, 31, 15, 7,  3,  1,  0,  0,  0,  0
#define O(a) (a * 1)
static const byte eg_rate_shift[16 + 64 + 16] =
{
	// Envelope Generator counter shifts (16 + 64 rates + 16 RKS)
	// 16 infinite time rates
	O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0),
	O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0),

	// rates 00-15
	O(12), O(12), O(12), O(12),
	O(11), O(11), O(11), O(11),
	O(10), O(10), O(10), O(10),
	O( 9), O( 9), O( 9), O( 9),
	O( 8), O( 8), O( 8), O( 8),
	O( 7), O( 7), O( 7), O( 7),
	O( 6), O( 6), O( 6), O( 6),
	O( 5), O( 5), O( 5), O( 5),
	O( 4), O( 4), O( 4), O( 4),
	O( 3), O( 3), O( 3), O( 3),
	O( 2), O( 2), O( 2), O( 2),
	O( 1), O( 1), O( 1), O( 1),
	O( 0), O( 0), O( 0), O( 0),
	O( 0), O( 0), O( 0), O( 0),
	O( 0), O( 0), O( 0), O( 0),
	O( 0), O( 0), O( 0), O( 0),

	// 16 dummy rates (same as 15 3)
	O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0),
	O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0), O( 0),
};
#undef O


// multiple table
#define ML(x) (byte)(2 * x)
static const byte mul_tab[16] = {
	// 1/2, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,10,12,12,15,15
	ML( 0.5), ML( 1.0), ML( 2.0), ML( 3.0),
	ML( 4.0), ML( 5.0), ML( 6.0), ML( 7.0),
	ML( 8.0), ML( 9.0), ML(10.0), ML(10.0),
	ML(12.0), ML(12.0), ML(15.0), ML(15.0)
};
#undef ML

// TL_TAB_LEN is calculated as:
//  (12+1)=13 - sinus amplitude bits     (Y axis)
//  additional 1: to compensate for calculations of negative part of waveform
//  (if we don't add it then the greatest possible _negative_ value would be -2
//  and we really need -1 for waveform #7)
//  2  - sinus sign bit           (Y axis)
//  TL_RES_LEN - sinus resolution (X axis)

static const int TL_TAB_LEN = 13 * 2 * TL_RES_LEN;
static int tl_tab[TL_TAB_LEN];
static const int ENV_QUIET = TL_TAB_LEN >> 4;

// sin waveform table in 'decibel' scale
// there are eight waveforms on OPL3 chips
static unsigned sin_tab[SIN_LEN * 8];

// LFO Amplitude Modulation table (verified on real YM3812)
//  27 output levels (triangle waveform); 1 level takes one of: 192, 256 or 448 samples
//
// Length: 210 elements
//
// Each of the elements has to be repeated
// exactly 64 times (on 64 consecutive samples).
// The whole table takes: 64 * 210 = 13440 samples.
//
// When AM = 1 data is used directly
// When AM = 0 data is divided by 4 before being used (loosing precision is important)

static const unsigned LFO_AM_TAB_ELEMENTS = 210;
static const byte lfo_am_table[LFO_AM_TAB_ELEMENTS] = {
	 0,  0,  0, /**/
	 0,  0,  0,  0,
	 1,  1,  1,  1,
	 2,  2,  2,  2,
	 3,  3,  3,  3,
	 4,  4,  4,  4,
	 5,  5,  5,  5,
	 6,  6,  6,  6,
	 7,  7,  7,  7,
	 8,  8,  8,  8,
	 9,  9,  9,  9,
	10, 10, 10, 10,
	11, 11, 11, 11,
	12, 12, 12, 12,
	13, 13, 13, 13,
	14, 14, 14, 14,
	15, 15, 15, 15,
	16, 16, 16, 16,
	17, 17, 17, 17,
	18, 18, 18, 18,
	19, 19, 19, 19,
	20, 20, 20, 20,
	21, 21, 21, 21,
	22, 22, 22, 22,
	23, 23, 23, 23,
	24, 24, 24, 24,
	25, 25, 25, 25,
	26, 26, 26, /**/
	25, 25, 25, 25,
	24, 24, 24, 24,
	23, 23, 23, 23,
	22, 22, 22, 22,
	21, 21, 21, 21,
	20, 20, 20, 20,
	19, 19, 19, 19,
	18, 18, 18, 18,
	17, 17, 17, 17,
	16, 16, 16, 16,
	15, 15, 15, 15,
	14, 14, 14, 14,
	13, 13, 13, 13,
	12, 12, 12, 12,
	11, 11, 11, 11,
	10, 10, 10, 10,
	 9,  9,  9,  9,
	 8,  8,  8,  8,
	 7,  7,  7,  7,
	 6,  6,  6,  6,
	 5,  5,  5,  5,
	 4,  4,  4,  4,
	 3,  3,  3,  3,
	 2,  2,  2,  2,
	 1,  1,  1,  1
};

// LFO Phase Modulation table (verified on real YM3812)
static const char lfo_pm_table[8 * 8 * 2] = {
	// FNUM2/FNUM = 00 0xxxxxxx (0x0000)
	0, 0, 0, 0, 0, 0, 0, 0,	//LFO PM depth = 0
	0, 0, 0, 0, 0, 0, 0, 0,	//LFO PM depth = 1

	// FNUM2/FNUM = 00 1xxxxxxx (0x0080)
	0, 0, 0, 0, 0, 0, 0, 0,	//LFO PM depth = 0
	1, 0, 0, 0,-1, 0, 0, 0,	//LFO PM depth = 1

	// FNUM2/FNUM = 01 0xxxxxxx (0x0100)
	1, 0, 0, 0,-1, 0, 0, 0,	//LFO PM depth = 0
	2, 1, 0,-1,-2,-1, 0, 1,	//LFO PM depth = 1

	// FNUM2/FNUM = 01 1xxxxxxx (0x0180)
	1, 0, 0, 0,-1, 0, 0, 0,	//LFO PM depth = 0
	3, 1, 0,-1,-3,-1, 0, 1,	//LFO PM depth = 1

	// FNUM2/FNUM = 10 0xxxxxxx (0x0200)
	2, 1, 0,-1,-2,-1, 0, 1,	//LFO PM depth = 0
	4, 2, 0,-2,-4,-2, 0, 2,	//LFO PM depth = 1

	// FNUM2/FNUM = 10 1xxxxxxx (0x0280)
	2, 1, 0,-1,-2,-1, 0, 1,	//LFO PM depth = 0
	5, 2, 0,-2,-5,-2, 0, 2,	//LFO PM depth = 1

	// FNUM2/FNUM = 11 0xxxxxxx (0x0300)
	3, 1, 0,-1,-3,-1, 0, 1,	//LFO PM depth = 0
	6, 3, 0,-3,-6,-3, 0, 3,	//LFO PM depth = 1

	// FNUM2/FNUM = 11 1xxxxxxx (0x0380)
	3, 1, 0,-1,-3,-1, 0, 1,	//LFO PM depth = 0
	7, 3, 0,-3,-7,-3, 0, 3	//LFO PM depth = 1
};

// TODO clean this up
static int phase_modulation;  // phase modulation input (SLOT 2)
static int phase_modulation2; // phase modulation input (SLOT 3
                              // in 4 operator channels)


YMF262Slot::YMF262Slot()
	: Cnt(0), Incr(0)
{
	ar = dr = rr = KSR = ksl = ksr = mul = 0;
	fb_shift = op1_out[0] = op1_out[1] = 0;
	CON = eg_type = vib = false;
	connect = 0;
	TL = TLL = volume = sl = 0;
	state = EG_OFF;
	eg_m_ar = eg_sh_ar = eg_sel_ar = eg_m_dr = eg_sh_dr = 0;
	eg_sel_dr = eg_m_rr = eg_sh_rr = eg_sel_rr = 0;
	key = AMmask = waveform_number = 0;
	wavetable = &sin_tab[0 * SIN_LEN];
}

YMF262Channel::YMF262Channel()
{
	block_fnum = ksl_base = kcode = extended = 0;
	fc = FreqIndex(0);
}


void YMF262Impl::callback(byte flag)
{
	setStatus(flag);
}

// status set and IRQ handling
void YMF262Impl::setStatus(byte flag)
{
	// set status flag masking out disabled IRQs
	status |= flag;
	if (status & statusMask) {
		status |= 0x80;
		irq.set();
	}
}

// status reset and IRQ handling
void YMF262Impl::resetStatus(byte flag)
{
	// reset status flag
	status &= ~flag;
	if (!(status & statusMask)) {
		status &= 0x7F;
		irq.reset();
	}
}

// IRQ mask set
void YMF262Impl::changeStatusMask(byte flag)
{
	statusMask = flag;
	status &= statusMask;
	if (status) {
		status |= 0x80;
		irq.set();
	} else {
		status &= 0x7F;
		irq.reset();
	}
}


// advance LFO to next sample
void YMF262Impl::advance_lfo()
{
	// Amplitude modulation: 27 output levels (triangle waveform);
	// 1 level takes one of: 192, 256 or 448 samples
	// One entry from LFO_AM_TABLE lasts for 64 samples
	lfo_am_cnt.addQuantum();
	if (lfo_am_cnt == LFOAMIndex(LFO_AM_TAB_ELEMENTS)) {
		// lfo_am_table is 210 elements long
		lfo_am_cnt = LFOAMIndex(0);
	}
	byte tmp = lfo_am_table[lfo_am_cnt.toInt()];
	LFO_AM = lfo_am_depth ? tmp : tmp / 4;

	// Vibrato: 8 output levels (triangle waveform); 1 level takes 1024 samples
	lfo_pm_cnt.addQuantum();
	LFO_PM = (lfo_pm_cnt.toInt() & 7) | lfo_pm_depth_range;
}

void YMF262Slot::advanceEnvelopeGenerator(unsigned eg_cnt)
{
	switch (state) {
	case EG_ATTACK:
		if (!(eg_cnt & eg_m_ar)) {
			volume += (~volume * eg_inc[eg_sel_ar + ((eg_cnt >> eg_sh_ar) & 7)]) >> 3;
			if (volume <= MIN_ATT_INDEX) {
				volume = MIN_ATT_INDEX;
				state = EG_DECAY;
			}
		}
		break;

	case EG_DECAY:
		if (!(eg_cnt & eg_m_dr)) {
			volume += eg_inc[eg_sel_dr + ((eg_cnt >> eg_sh_dr) & 7)];
			if (volume >= sl) {
				state = EG_SUSTAIN;
			}
		}
		break;

	case EG_SUSTAIN:
		// this is important behaviour:
		// one can change percusive/non-percussive
		// modes on the fly and the chip will remain
		// in sustain phase - verified on real YM3812
		if (eg_type) {
			// non-percussive mode
			// do nothing
		} else {
			// percussive mode
			// during sustain phase chip adds Release Rate (in percussive mode)
			if (!(eg_cnt & eg_m_rr)) {
				volume += eg_inc[eg_sel_rr + ((eg_cnt >> eg_sh_rr) & 7)];
				if (volume >= MAX_ATT_INDEX) {
					volume = MAX_ATT_INDEX;
				}
			} else {
				// do nothing in sustain phase
			}
		}
		break;

	case EG_RELEASE:
		if (!(eg_cnt & eg_m_rr)) {
			volume += eg_inc[eg_sel_rr + ((eg_cnt >> eg_sh_rr) & 7)];
			if (volume >= MAX_ATT_INDEX) {
				volume = MAX_ATT_INDEX;
				state = EG_OFF;
			}
		}
		break;

	default:
		break;
	}
}

void YMF262Slot::advancePhaseGenerator(YMF262Channel& ch, unsigned LFO_PM)
{
	if (vib) {
		unsigned block_fnum = ch.block_fnum;
		unsigned fnum_lfo   = (block_fnum & 0x0380) >> 7;
		int lfo_fn_table_index_offset = lfo_pm_table[LFO_PM + 16 * fnum_lfo];
		if (lfo_fn_table_index_offset) {
			// LFO phase modulation active
			Cnt += fnumToIncrement(block_fnum + lfo_fn_table_index_offset)
			        * mul;
		} else {
			// LFO phase modulation  = zero
			Cnt += Incr;
		}
	} else {
		// LFO phase modulation disabled for this operator
		Cnt += Incr;
	}
}

// advance to next sample
void YMF262Impl::advance()
{
	++eg_cnt;
	for (int c = 0; c < 18; ++c) {
		YMF262Channel& ch = channels[c];
		for (int s = 0; s < 2; ++s) {
			YMF262Slot& op = ch.slots[s];
			op.advanceEnvelopeGenerator(eg_cnt);
			op.advancePhaseGenerator(ch, LFO_PM);
		}
	}

	// The Noise Generator of the YM3812 is 23-bit shift register.
	// Period is equal to 2^23-2 samples.
	// Register works at sampling frequency of the chip, so output
	// can change on every sample.
	//
	// Output of the register and input to the bit 22 is:
	// bit0 XOR bit14 XOR bit15 XOR bit22
	//
	// Simply use bit 22 as the noise output.
	//
	// unsigned j = ((noise_rng >>  0) ^ (noise_rng >> 14) ^
	//               (noise_rng >> 15) ^ (noise_rng >> 22)) & 1;
	// noise_rng = (j << 22) | (noise_rng >> 1);
	//
	// Instead of doing all the logic operations above, we
	// use a trick here (and use bit 0 as the noise output).
	// The difference is only that the noise bit changes one
	// step ahead. This doesn't matter since we don't know
	// what is real state of the noise_rng after the reset.
	if (noise_rng & 1) {
		noise_rng ^= 0x800302;
	}
	noise_rng >>= 1;
}


inline int YMF262Slot::op_calc(unsigned phase, int pm, byte LFO_AM)
{
	unsigned env = (TLL + volume + (LFO_AM & AMmask)) << 4;
	int p = env + wavetable[(phase + pm) & SIN_MASK];
	return (p < TL_TAB_LEN) ? tl_tab[p] : 0;
}

// calculate output of a standard 2 operator channel
// (or 1st part of a 4-op channel)
void YMF262Channel::chan_calc(byte LFO_AM)
{
	phase_modulation  = 0;
	phase_modulation2 = 0;

	// SLOT 1
	slots[SLOT1].op1_out[0] = slots[SLOT1].op1_out[1];
	int out = slots[SLOT1].fb_shift
		? slots[SLOT1].op1_out[0] + slots[SLOT1].op1_out[1]
		: 0;
	slots[SLOT1].op1_out[1] = slots[SLOT1].op_calc(slots[SLOT1].Cnt.toInt(), out >> slots[SLOT1].fb_shift, LFO_AM);
	*slots[SLOT1].connect += slots[SLOT1].op1_out[1];

	// SLOT 2
	*slots[SLOT2].connect += slots[SLOT2].op_calc(slots[SLOT2].Cnt.toInt(), phase_modulation, LFO_AM);
}

// calculate output of a 2nd part of 4-op channel
void YMF262Channel::chan_calc_ext(byte LFO_AM)
{
	phase_modulation = 0;

	// SLOT 1
	*slots[SLOT1].connect += slots[SLOT1].op_calc(slots[SLOT1].Cnt.toInt(), phase_modulation2, LFO_AM);

	// SLOT 2
	*slots[SLOT2].connect += slots[SLOT2].op_calc(slots[SLOT2].Cnt.toInt(), phase_modulation, LFO_AM);
}

// operators used in the rhythm sounds generation process:
//
// Envelope Generator:
//
// channel  operator  register number   Bass  High  Snare Tom  Top
// / slot   number    TL ARDR SLRR Wave Drum  Hat   Drum  Tom  Cymbal
//  6 / 0   12        50  70   90   f0  +
//  6 / 1   15        53  73   93   f3  +
//  7 / 0   13        51  71   91   f1        +
//  7 / 1   16        54  74   94   f4              +
//  8 / 0   14        52  72   92   f2                    +
//  8 / 1   17        55  75   95   f5                          +
//
// Phase Generator:
//
// channel  operator  register number   Bass  High  Snare Tom  Top
// / slot   number    MULTIPLE          Drum  Hat   Drum  Tom  Cymbal
//  6 / 0   12        30                +
//  6 / 1   15        33                +
//  7 / 0   13        31                      +     +           +
//  7 / 1   16        34                -----  n o t  u s e d -----
//  8 / 0   14        32                                  +
//  8 / 1   17        35                      +                 +
//
// channel  operator  register number   Bass  High  Snare Tom  Top
// number   number    BLK/FNUM2 FNUM    Drum  Hat   Drum  Tom  Cymbal
//    6     12,15     B6        A6      +
//
//    7     13,16     B7        A7            +     +           +
//
//    8     14,17     B8        A8            +           +     +

// The following formulas can be well optimized.
// I leave them in direct form for now (in case I've missed something).

inline int YMF262Impl::genPhaseHighHat()
{
	// high hat phase generation:
	// phase = d0 or 234 (based on frequency only)
	// phase = 34 or 2d0 (based on noise)

	// base frequency derived from operator 1 in channel 7
	int op71phase = channels[7].slots[SLOT1].Cnt.toInt();
	bool bit7 = op71phase & 0x80;
	bool bit3 = op71phase & 0x08;
	bool bit2 = op71phase & 0x04;
	bool res1 = (bit2 ^ bit7) | bit3;
	// when res1 = 0 phase = 0x000 | 0xd0;
	// when res1 = 1 phase = 0x200 | (0xd0>>2);
	unsigned phase = res1 ? (0x200 | (0xd0 >> 2)) : 0xd0;

	// enable gate based on frequency of operator 2 in channel 8
	int op82phase = channels[8].slots[SLOT2].Cnt.toInt();
	bool bit5e= op82phase & 0x20;
	bool bit3e= op82phase & 0x08;
	bool res2 = (bit3e ^ bit5e);
	// when res2 = 0 pass the phase from calculation above (res1);
	// when res2 = 1 phase = 0x200 | (0xd0>>2);
	if (res2) {
		phase = (0x200 | (0xd0 >> 2));
	}

	// when phase & 0x200 is set and noise=1 then phase = 0x200|0xd0
	// when phase & 0x200 is set and noise=0 then phase = 0x200|(0xd0>>2), ie no change
	if (phase & 0x200) {
		if (noise_rng & 1) {
			phase = 0x200 | 0xd0;
		}
	} else {
	// when phase & 0x200 is clear and noise=1 then phase = 0xd0>>2
	// when phase & 0x200 is clear and noise=0 then phase = 0xd0, ie no change
		if (noise_rng & 1) {
			phase = 0xd0 >> 2;
		}
	}
	return phase;
}

inline int YMF262Impl::genPhaseSnare()
{
	// base frequency derived from operator 1 in channel 7
	// noise bit XOR'es phase by 0x100
	return ((channels[7].slots[SLOT1].Cnt.toInt() & 0x100) + 0x100)
	     ^ ((noise_rng & 1) << 8);
}

inline int YMF262Impl::genPhaseCymbal()
{
	// enable gate based on frequency of operator 2 in channel 8
	//  NOTE: YM2413_2 uses bit5 | bit3, this core uses bit5 ^ bit3
	//        most likely only one of the two is correct
	int op82phase = channels[8].slots[SLOT2].Cnt.toInt();
	if ((op82phase ^ (op82phase << 2)) & 0x20) { // bit5 ^ bit3
		return 0x300;
	} else {
		// base frequency derived from operator 1 in channel 7
		int op71phase = channels[7].slots[SLOT1].Cnt.toInt();
		bool bit7 = op71phase & 0x80;
		bool bit3 = op71phase & 0x08;
		bool bit2 = op71phase & 0x04;
		return ((bit2 ^ bit7) | bit3) ? 0x300 : 0x100;
	}
}

// calculate rhythm
void YMF262Impl::chan_calc_rhythm()
{
	YMF262Slot& SLOT6_1 = channels[6].slots[SLOT1];
	YMF262Slot& SLOT6_2 = channels[6].slots[SLOT2];
	YMF262Slot& SLOT7_1 = channels[7].slots[SLOT1];
	YMF262Slot& SLOT7_2 = channels[7].slots[SLOT2];
	YMF262Slot& SLOT8_1 = channels[8].slots[SLOT1];
	YMF262Slot& SLOT8_2 = channels[8].slots[SLOT2];

	// Bass Drum (verified on real YM3812):
	//  - depends on the channel 6 'connect' register:
	//      when connect = 0 it works the same as in normal (non-rhythm)
	//      mode (op1->op2->out)
	//      when connect = 1 _only_ operator 2 is present on output
	//      (op2->out), operator 1 is ignored
	//  - output sample always is multiplied by 2

	phase_modulation = 0;

	// SLOT 1
	SLOT6_1.op1_out[0] = SLOT6_1.op1_out[1];

	if (!SLOT6_1.CON) {
		phase_modulation = SLOT6_1.op1_out[0];
	} else {
		// ignore output of operator 1
	}

	int out = SLOT6_1.fb_shift
		? SLOT6_1.op1_out[0] + SLOT6_1.op1_out[1]
		: 0;
	SLOT6_1.op1_out[1] = SLOT6_1.op_calc(SLOT6_1.Cnt.toInt(), out >> SLOT6_1.fb_shift, LFO_AM);

	// SLOT 2
	chanout[6] += SLOT6_2.op_calc(SLOT6_2.Cnt.toInt(), phase_modulation, LFO_AM) * 2;

	// Phase generation is based on:
	// HH  (13) channel 7->slot 1 combined with channel 8->slot 2
	//          (same combination as TOP CYMBAL but different output phases)
	// SD  (16) channel 7->slot 1
	// TOM (14) channel 8->slot 1
	// TOP (17) channel 7->slot 1 combined with channel 8->slot 2
	//          (same combination as HIGH HAT but different output phases)

	// Envelope generation based on:
	// HH  channel 7->slot1
	// SD  channel 7->slot2
	// TOM channel 8->slot1
	// TOP channel 8->slot2

	// High Hat (verified on real YM3812)
	chanout[7] += SLOT7_1.op_calc(genPhaseHighHat(), 0, LFO_AM) * 2;

	// Snare Drum (verified on real YM3812)
	chanout[7] += SLOT7_2.op_calc(genPhaseSnare(), 0, LFO_AM) * 2;

	// Tom Tom (verified on real YM3812)
	chanout[8] += SLOT8_1.op_calc(SLOT8_1.Cnt.toInt(), 0, LFO_AM) * 2;

	// Top Cymbal (verified on real YM3812)
	chanout[8] += SLOT8_2.op_calc(genPhaseCymbal(), 0, LFO_AM) * 2;
}


// generic table initialize
void YMF262Impl::init_tables()
{
	static bool alreadyInit = false;
	if (alreadyInit) {
		return;
	}
	alreadyInit = true;

	for (int x = 0; x < TL_RES_LEN; x++) {
		double m = (1 << 16) / pow(2, (x + 1) * (ENV_STEP / 4.0) / 8.0);
		m = floor(m);

		// we never reach (1<<16) here due to the (x+1)
		// result fits within 16 bits at maximum
		int n = (int)m;		// 16 bits here
		n >>= 4;		// 12 bits here
		n = (n >> 1) + (n & 1); // round to nearest
		// 11 bits here (rounded)
		n <<= 1;		// 12 bits here (as in real chip)
		tl_tab[x * 2 + 0] = n;
		tl_tab[x * 2 + 1] = ~tl_tab[x * 2 + 0]; // this _is_ different from OPL2 (verified on real YMF262)

		for (int i = 1; i < 13; i++) {
			tl_tab[x * 2 + 0 + i * 2 * TL_RES_LEN] =  tl_tab[x * 2 + 0] >> i;
			tl_tab[x * 2 + 1 + i * 2 * TL_RES_LEN] = ~tl_tab[x * 2 + 0 + i * 2 * TL_RES_LEN];  // this _is_ different from OPL2 (verified on real YMF262)
		}
	}

	const double LOG2 = ::log(2);
	for (int i = 0; i < SIN_LEN; i++) {
		// non-standard sinus
		double m = sin(((i * 2) + 1) * M_PI / SIN_LEN); // checked against the real chip
		// we never reach zero here due to ((i * 2) + 1)
		double o = (m > 0.0) ?
			8 * ::log( 1.0 / m) / LOG2:	// convert to 'decibels'
			8 * ::log(-1.0 / m) / LOG2;	// convert to 'decibels'
		o = o / (ENV_STEP / 4);

		int n = (int)(2 * o);
		n = (n >> 1) + (n & 1); // round to nearest
		sin_tab[i] = n * 2 + (m >= 0.0 ? 0 : 1);
	}

	for (int i = 0; i < SIN_LEN; ++i) {
		// these 'pictures' represent _two_ cycles
		// waveform 1:  __      __
		//             /  \____/  \____
		// output only first half of the sinus waveform (positive one)
		sin_tab[1 * SIN_LEN + i] = (i & (1 << (SIN_BITS - 1)))
		                         ? TL_TAB_LEN
		                         : sin_tab[i];

		// waveform 2:  __  __  __  __
		//             /  \/  \/  \/  \.
		// abs(sin)
		sin_tab[2 * SIN_LEN + i] = sin_tab[i & (SIN_MASK >> 1)];

		// waveform 3:  _   _   _   _
		//             / |_/ |_/ |_/ |_
		// abs(output only first quarter of the sinus waveform)
		sin_tab[3 * SIN_LEN + i] = (i & (1 << (SIN_BITS - 2)))
		                         ? TL_TAB_LEN
		                         : sin_tab[i & (SIN_MASK>>2)];

		// waveform 4: /\  ____/\  ____
		//               \/      \/
		// output whole sinus waveform in half the cycle(step=2)
		// and output 0 on the other half of cycle
		sin_tab[4 * SIN_LEN + i] = (i & (1 << (SIN_BITS - 1)))
		                         ? TL_TAB_LEN
		                         : sin_tab[i * 2];

		// waveform 5: /\/\____/\/\____
		//
		// output abs(whole sinus) waveform in half the cycle(step=2)
		// and output 0 on the other half of cycle
		sin_tab[5 * SIN_LEN + i] = (i & (1 << (SIN_BITS - 1)))
		                         ? TL_TAB_LEN
		                         : sin_tab[(i * 2) & (SIN_MASK >> 1)];

		// waveform 6: ____    ____
		//                 ____    ____
		// output maximum in half the cycle and output minimum
		// on the other half of cycle
		sin_tab[6 * SIN_LEN + i] = (i & (1 << (SIN_BITS - 1)))
		                         ? 1  // negative
		                         : 0; // positive

		// waveform 7:|\____  |\____
		//                   \|      \|
		// output sawtooth waveform
		int x = (i & (1 << (SIN_BITS - 1)))
		      ? ((SIN_LEN - 1) - i) * 16 + 1  // negative: from 8177 to 1
		      : i * 16;                       // positive: from 0 to 8176
		x = std::min(x, TL_TAB_LEN); // clip to the allowed range
		sin_tab[7 * SIN_LEN + i] = x;
	}
}


void YMF262Impl::setOutputRate(unsigned sampleRate)
{
	const int CLOCK_FREQ = 3579545 * 4;
	double input = CLOCK_FREQ / (8.0 * 36.0);
	setInputRate(static_cast<int>(input + 0.5));
	setResampleRatio(input, sampleRate);
}

void YMF262Slot::FM_KEYON(byte key_set)
{
	if (!key) {
		// restart Phase Generator
		Cnt = FreqIndex(0);
		// phase -> Attack
		state = EG_ATTACK;
	}
	key |= key_set;
}

void YMF262Slot::FM_KEYOFF(byte key_clr)
{
	if (key) {
		key &= ~key_clr;
		if (!key) {
			// phase -> Release
			if (state != EG_OFF) {
				state = EG_RELEASE;
			}
		}
	}
}

// update phase increment counter of operator (also update the EG rates if necessary)
void YMF262Channel::CALC_FCSLOT(YMF262Slot& slot)
{
	// (frequency) phase increment counter
	slot.Incr = fc * slot.mul;
	int ksr = kcode >> slot.KSR;

	if (slot.ksr == ksr) return;
	slot.ksr = ksr;

	// calculate envelope generator rates
	if ((slot.ar + slot.ksr) < 16 + 60) {
		slot.eg_sh_ar  = eg_rate_shift [slot.ar + slot.ksr];
		slot.eg_sel_ar = eg_rate_select[slot.ar + slot.ksr];
	} else {
		slot.eg_sh_ar  = 0;
		slot.eg_sel_ar = 13 * RATE_STEPS;
	}
	slot.eg_m_ar   = (1 << slot.eg_sh_ar) - 1;
	slot.eg_sh_dr  = eg_rate_shift [slot.dr + slot.ksr];
	slot.eg_m_dr   = (1 << slot.eg_sh_dr) - 1;
	slot.eg_sel_dr = eg_rate_select[slot.dr + slot.ksr];
	slot.eg_sh_rr  = eg_rate_shift [slot.rr + slot.ksr];
	slot.eg_m_rr   = (1 << slot.eg_sh_rr) - 1;
	slot.eg_sel_rr = eg_rate_select[slot.rr + slot.ksr];
}

// set multi,am,vib,EG-TYP,KSR,mul
void YMF262Impl::set_mul(byte sl, byte v)
{
	int chan_no = sl / 2;
	YMF262Channel& ch  = channels[chan_no];
	YMF262Slot& slot = ch.slots[sl & 1];

	slot.mul     = mul_tab[v & 0x0f];
	slot.KSR     = (v & 0x10) ? 0 : 2;
	slot.eg_type = (v & 0x20);
	slot.vib     = (v & 0x40);
	slot.AMmask  = (v & 0x80) ? ~0 : 0;

	if (OPL3_mode) {
		// in OPL3 mode
		// DO THIS:
		//  if this is one of the slots of 1st channel forming up a 4-op channel
		//  do normal operation
		//  else normal 2 operator function
		// OR THIS:
		//  if this is one of the slots of 2nd channel forming up a 4-op channel
		//  update it using channel data of 1st channel of a pair
		//  else normal 2 operator function
		switch(chan_no) {
		case 0: case 1: case 2:
		case 9: case 10: case 11:
			if (ch.extended) {
				// normal
				ch.CALC_FCSLOT(slot);
			} else {
				// normal
				ch.CALC_FCSLOT(slot);
			}
			break;
		case 3: case 4: case 5:
		case 12: case 13: case 14: {
			YMF262Channel& ch3 = channels[chan_no - 3];
			if (ch3.extended) {
				// update this slot using frequency data for 1st channel of a pair
				ch3.CALC_FCSLOT(slot);
			} else {
				// normal
				ch.CALC_FCSLOT(slot);
			}
			break;
		}
		default:
			// normal
			ch.CALC_FCSLOT(slot);
			break;
		}
	} else {
		// in OPL2 mode
		ch.CALC_FCSLOT(slot);
	}
}

// set ksl & tl
void YMF262Impl::set_ksl_tl(byte sl, byte v)
{
	int chan_no = sl/2;
	YMF262Channel& ch = channels[chan_no];
	YMF262Slot& slot = ch.slots[sl & 1];

	int ksl = v >> 6; // 0 / 1.5 / 3.0 / 6.0 dB/OCT

	slot.ksl = ksl ? 3 - ksl : 31;
	slot.TL  = (v & 0x3F) << (ENV_BITS - 1 - 7); // 7 bits TL (bit 6 = always 0)

	if (OPL3_mode) {
		// in OPL3 mode
		//DO THIS:
		//if this is one of the slots of 1st channel forming up a 4-op channel
		//do normal operation
		//else normal 2 operator function
		//OR THIS:
		//if this is one of the slots of 2nd channel forming up a 4-op channel
		//update it using channel data of 1st channel of a pair
		//else normal 2 operator function
		switch(chan_no) {
		case 0: case 1: case 2:
		case 9: case 10: case 11:
			if (ch.extended) {
				// normal
				slot.TLL = slot.TL + (ch.ksl_base >> slot.ksl);
			} else {
				// normal
				slot.TLL = slot.TL + (ch.ksl_base >> slot.ksl);
			}
			break;
		case 3: case 4: case 5:
		case 12: case 13: case 14: {
			YMF262Channel& ch3 = channels[chan_no - 3];
			if (ch3.extended) {
				// update this slot using frequency data for 1st channel of a pair
				slot.TLL = slot.TL + (ch3.ksl_base >> slot.ksl);
			} else {
				// normal
				slot.TLL = slot.TL + (ch.ksl_base >> slot.ksl);
			}
			break;
		}
		default:
			// normal
			slot.TLL = slot.TL + (ch.ksl_base >> slot.ksl);
			break;
		}
	} else {
		// in OPL2 mode
		slot.TLL = slot.TL + (ch.ksl_base >> slot.ksl);
	}
}

// set attack rate & decay rate
void YMF262Impl::set_ar_dr(byte sl, byte v)
{
	YMF262Channel& ch = channels[sl / 2];
	YMF262Slot& slot = ch.slots[sl & 1];

	slot.ar = (v >> 4) ? 16 + ((v >> 4) << 2) : 0;
	if ((slot.ar + slot.ksr) < 16 + 60) {
		// verified on real YMF262 - all 15 x rates take "zero" time
		slot.eg_sh_ar  = eg_rate_shift [slot.ar + slot.ksr];
		slot.eg_sel_ar = eg_rate_select[slot.ar + slot.ksr];
	} else {
		slot.eg_sh_ar  = 0;
		slot.eg_sel_ar = 13 * RATE_STEPS;
	}
	slot.eg_m_ar   = (1 << slot.eg_sh_ar) - 1;
	slot.dr        = (v & 0x0F) ? 16 + ((v & 0x0F) << 2) : 0;
	slot.eg_sh_dr  = eg_rate_shift [slot.dr + slot.ksr];
	slot.eg_m_dr   = (1 << slot.eg_sh_dr) - 1;
	slot.eg_sel_dr = eg_rate_select[slot.dr + slot.ksr];
}

// set sustain level & release rate
void YMF262Impl::set_sl_rr(byte sl, byte v)
{
	YMF262Channel& ch = channels[sl / 2];
	YMF262Slot& slot = ch.slots[sl & 1];

	slot.sl  = sl_tab[v >> 4];
	slot.rr  = (v & 0x0F) ? 16 + ((v & 0x0F) << 2) : 0;
	slot.eg_sh_rr  = eg_rate_shift [slot.rr + slot.ksr];
	slot.eg_m_rr   = (1 << slot.eg_sh_rr) - 1;
	slot.eg_sel_rr = eg_rate_select[slot.rr + slot.ksr];
}

void YMF262Impl::update_channels(YMF262Channel& ch)
{
	// update channel passed as a parameter and a channel at CH+=3;
	if (ch.extended) {
		// we've just switched to combined 4 operator mode
	} else {
		// we've just switched to normal 2 operator mode
	}
}

byte YMF262Impl::readReg(int r)
{
	// no need to call updateStream(time)
	return peekReg(r);
}

byte YMF262Impl::peekReg(int r) const
{
	return reg[r];
}

void YMF262Impl::writeReg(int r, byte v, const EmuTime& time)
{
	if (!OPL3_mode && (r != 0x105)) {
		// in OPL2 mode the only accessible in set #2 is register 0x05
		r &= ~0x100;
	}
	writeRegForce(r, v, time);
}
void YMF262Impl::writeRegForce(int r, byte v, const EmuTime& time)
{
	updateStream(time); // TODO optimize only for regs that directly influence sound

	reg[r] = v;

	byte ch_offset = 0;
	if (r & 0x100) {
		switch(r) {
		case 0x101:	// test register
			return;

		case 0x104: { // 6 channels enable
			YMF262Channel& ch0 = channels[0];
			byte prev = ch0.extended;
			ch0.extended = (v >> 0) & 1;
			if (prev != ch0.extended) {
				update_channels(ch0);
			}
			YMF262Channel& ch1 = channels[1];
			prev = ch1.extended;
			ch1.extended = (v >> 1) & 1;
			if (prev != ch1.extended) {
				update_channels(ch1);
			}
			YMF262Channel& ch2 = channels[2];
			prev = ch2.extended;
			ch2.extended = (v >> 2) & 1;
			if (prev != ch2.extended) {
				update_channels(ch2);
			}
			YMF262Channel& ch9 = channels[9];
			prev = ch9.extended;
			ch9.extended = (v >> 3) & 1;
			if (prev != ch9.extended) {
				update_channels(ch9);
			}
			YMF262Channel& ch10 = channels[10];
			prev = ch10.extended;
			ch10.extended = (v >> 4) & 1;
			if (prev != ch10.extended) {
				update_channels(ch10);
			}
			YMF262Channel& ch11 = channels[11];
			prev = ch11.extended;
			ch11.extended = (v >> 5) & 1;
			if (prev != ch11.extended) {
				update_channels(ch11);
			}
			return;
		}
		case 0x105:	// OPL3 extensions enable register
			// OPL3 mode when bit0=1 otherwise it is OPL2 mode
			OPL3_mode = v & 0x01;
			if (OPL3_mode) {
				status2 = 0x02;
			}

			// following behaviour was tested on real YMF262,
			// switching OPL3/OPL2 modes on the fly:
			//  - does not change the waveform previously selected
			//    (unless when ....)
			//  - does not update CH.A, CH.B, CH.C and CH.D output
			//    selectors (registers c0-c8) (unless when ....)
			//  - does not disable channels 9-17 on OPL3->OPL2 switch
			//  - does not switch 4 operator channels back to 2
			//    operator channels
			return;

		default:
			break;
		}
		ch_offset = 9;	// register page #2 starts from channel 9
	}

	r &= 0xFF;
	switch(r & 0xE0) {
	case 0x00: // 00-1F:control
		switch(r & 0x1F) {
		case 0x01: // test register
			break;

		case 0x02: // Timer 1
			timer1.setValue(v);
			break;

		case 0x03: // Timer 2
			timer2.setValue(v);
			break;

		case 0x04: // IRQ clear / mask and Timer enable
			if (v & 0x80) {
				// IRQ flags clear
				resetStatus(0x60);
			} else {
				changeStatusMask((~v) & 0x60);
				timer1.setStart(v & R04_ST1, time);
				timer2.setStart(v & R04_ST2, time);
			}
			break;

		case 0x08: // x,NTS,x,x, x,x,x,x
			nts = v & 0x40;
			break;

		default:
			break;
		}
		break;

	case 0x20: { // am ON, vib ON, ksr, eg_type, mul
		int slot = slot_array[r & 0x1F];
		if (slot < 0) return;
		set_mul(slot + ch_offset * 2, v);
		break;
	}
	case 0x40: {
		int slot = slot_array[r & 0x1F];
		if (slot < 0) return;
		set_ksl_tl(slot + ch_offset * 2, v);
		break;
	}
	case 0x60: {
		int slot = slot_array[r & 0x1F];
		if (slot < 0) return;
		set_ar_dr(slot + ch_offset * 2, v);
		break;
	}
	case 0x80: {
		int slot = slot_array[r & 0x1F];
		if (slot < 0) return;
		set_sl_rr(slot + ch_offset * 2, v);
		break;
	}
	case 0xA0: {
		if (r == 0xBD) {
			// am depth, vibrato depth, r,bd,sd,tom,tc,hh
			if (ch_offset != 0) {
				// 0xbd register is present in set #1 only
				return;
			}
			lfo_am_depth = v & 0x80;
			lfo_pm_depth_range = (v & 0x40) ? 8 : 0;
			rhythm = v & 0x3F;

			if (rhythm & 0x20) {
				// BD key on/off
				if (v & 0x10) {
					channels[6].slots[SLOT1].FM_KEYON (2);
					channels[6].slots[SLOT2].FM_KEYON (2);
				} else {
					channels[6].slots[SLOT1].FM_KEYOFF(2);
					channels[6].slots[SLOT2].FM_KEYOFF(2);
				}
				// HH key on/off
				if (v & 0x01) {
					channels[7].slots[SLOT1].FM_KEYON (2);
				} else {
					channels[7].slots[SLOT1].FM_KEYOFF(2);
				}
				// SD key on/off
				if (v & 0x08) {
					channels[7].slots[SLOT2].FM_KEYON (2);
				} else {
					channels[7].slots[SLOT2].FM_KEYOFF(2);
				}
				// TOM key on/off
				if (v & 0x04) {
					channels[8].slots[SLOT1].FM_KEYON (2);
				} else {
					channels[8].slots[SLOT1].FM_KEYOFF(2);
				}
				// TOP-CY key on/off
				if (v & 0x02) {
					channels[8].slots[SLOT2].FM_KEYON (2);
				} else {
					channels[8].slots[SLOT2].FM_KEYOFF(2);
				}
			} else {
				// BD key off
				channels[6].slots[SLOT1].FM_KEYOFF(2);
				channels[6].slots[SLOT2].FM_KEYOFF(2);
				// HH key off
				channels[7].slots[SLOT1].FM_KEYOFF(2);
				// SD key off
				channels[7].slots[SLOT2].FM_KEYOFF(2);
				// TOM key off
				channels[8].slots[SLOT1].FM_KEYOFF(2);
				// TOP-CY off
				channels[8].slots[SLOT2].FM_KEYOFF(2);
			}
			return;
		}

		// keyon,block,fnum
		if ((r & 0x0F) > 8) {
			return;
		}
		int chan_no = (r & 0x0F) + ch_offset;
		YMF262Channel& ch  = channels[chan_no];
		YMF262Channel& ch3 = channels[chan_no + 3];
		int block_fnum;
		if (!(r & 0x10)) {
			// a0-a8
			block_fnum  = (ch.block_fnum&0x1F00) | v;
		} else {
			// b0-b8
			block_fnum = ((v & 0x1F) << 8) | (ch.block_fnum & 0xFF);
			if (OPL3_mode) {
				// in OPL3 mode
				// DO THIS:
				// if this is 1st channel forming up a 4-op channel
				// ALSO keyon/off slots of 2nd channel forming up 4-op channel
				// else normal 2 operator function keyon/off
				// OR THIS:
				// if this is 2nd channel forming up 4-op channel just do nothing
				// else normal 2 operator function keyon/off
				switch(chan_no) {
				case 0: case 1: case 2:
				case 9: case 10: case 11:
					if (ch.extended) {
						//if this is 1st channel forming up a 4-op channel
						//ALSO keyon/off slots of 2nd channel forming up 4-op channel
						if (v & 0x20) {
							ch.slots[SLOT1].FM_KEYON (1);
							ch.slots[SLOT2].FM_KEYON (1);
							ch3.slots[SLOT1].FM_KEYON(1);
							ch3.slots[SLOT2].FM_KEYON(1);
						} else {
							ch.slots[SLOT1].FM_KEYOFF (1);
							ch.slots[SLOT2].FM_KEYOFF (1);
							ch3.slots[SLOT1].FM_KEYOFF(1);
							ch3.slots[SLOT2].FM_KEYOFF(1);
						}
					} else {
						//else normal 2 operator function keyon/off
						if (v & 0x20) {
							ch.slots[SLOT1].FM_KEYON (1);
							ch.slots[SLOT2].FM_KEYON (1);
						} else {
							ch.slots[SLOT1].FM_KEYOFF(1);
							ch.slots[SLOT2].FM_KEYOFF(1);
						}
					}
					break;

				case 3: case 4: case 5:
				case 12: case 13: case 14: {
					YMF262Channel& ch_3 = channels[chan_no - 3];
					if (ch_3.extended) {
						//if this is 2nd channel forming up 4-op channel just do nothing
					} else {
						//else normal 2 operator function keyon/off
						if (v & 0x20) {
							ch.slots[SLOT1].FM_KEYON (1);
							ch.slots[SLOT2].FM_KEYON (1);
						} else {
							ch.slots[SLOT1].FM_KEYOFF(1);
							ch.slots[SLOT2].FM_KEYOFF(1);
						}
					}
					break;
				}
				default:
					if (v & 0x20) {
						ch.slots[SLOT1].FM_KEYON (1);
						ch.slots[SLOT2].FM_KEYON (1);
					} else {
						ch.slots[SLOT1].FM_KEYOFF(1);
						ch.slots[SLOT2].FM_KEYOFF(1);
					}
					break;
				}
			} else {
				if (v & 0x20) {
					ch.slots[SLOT1].FM_KEYON (1);
					ch.slots[SLOT2].FM_KEYON (1);
				} else {
					ch.slots[SLOT1].FM_KEYOFF(1);
					ch.slots[SLOT2].FM_KEYOFF(1);
				}
			}
		}
		// update
		if (ch.block_fnum != block_fnum) {
			ch.block_fnum = block_fnum;
			ch.ksl_base = ksl_tab[block_fnum >> 6];
			ch.fc       = fnumToIncrement(block_fnum);

			// BLK 2,1,0 bits -> bits 3,2,1 of kcode
			ch.kcode = (ch.block_fnum & 0x1C00) >> 9;

			// the info below is actually opposite to what is stated
			// in the Manuals (verifed on real YMF262)
			// if notesel == 0 -> lsb of kcode is bit 10 (MSB) of fnum
			// if notesel == 1 -> lsb of kcode is bit 9 (MSB-1) of fnum
			if (nts) {
				ch.kcode |= (ch.block_fnum & 0x100) >> 8;	// notesel == 1
			} else {
				ch.kcode |= (ch.block_fnum & 0x200) >> 9;	// notesel == 0
			}
			if (OPL3_mode) {
				// in OPL3 mode
				//DO THIS:
				//if this is 1st channel forming up a 4-op channel
				//ALSO update slots of 2nd channel forming up 4-op channel
				//else normal 2 operator function keyon/off
				//OR THIS:
				//if this is 2nd channel forming up 4-op channel just do nothing
				//else normal 2 operator function keyon/off
				switch (chan_no) {
				case 0: case 1: case 2:
				case 9: case 10: case 11:
					if (ch.extended) {
						//if this is 1st channel forming up a 4-op channel
						//ALSO update slots of 2nd channel forming up 4-op channel

						// refresh Total Level in FOUR SLOTs of this channel and channel+3 using data from THIS channel
						ch.slots[SLOT1].TLL = ch.slots[SLOT1].TL + (ch.ksl_base >> ch.slots[SLOT1].ksl);
						ch.slots[SLOT2].TLL = ch.slots[SLOT2].TL + (ch.ksl_base >> ch.slots[SLOT2].ksl);
						ch3.slots[SLOT1].TLL = ch3.slots[SLOT1].TL + (ch.ksl_base >> ch3.slots[SLOT1].ksl);
						ch3.slots[SLOT2].TLL = ch3.slots[SLOT2].TL + (ch.ksl_base >> ch3.slots[SLOT2].ksl);

						// refresh frequency counter in FOUR SLOTs of this channel and channel+3 using data from THIS channel
						ch.CALC_FCSLOT(ch.slots[SLOT1]);
						ch.CALC_FCSLOT(ch.slots[SLOT2]);
						ch.CALC_FCSLOT(ch3.slots[SLOT1]);
						ch.CALC_FCSLOT(ch3.slots[SLOT2]);
					} else {
						//else normal 2 operator function
						// refresh Total Level in both SLOTs of this channel
						ch.slots[SLOT1].TLL = ch.slots[SLOT1].TL + (ch.ksl_base >> ch.slots[SLOT1].ksl);
						ch.slots[SLOT2].TLL = ch.slots[SLOT2].TL + (ch.ksl_base >> ch.slots[SLOT2].ksl);

						// refresh frequency counter in both SLOTs of this channel
						ch.CALC_FCSLOT(ch.slots[SLOT1]);
						ch.CALC_FCSLOT(ch.slots[SLOT2]);
					}
					break;

				case 3: case 4: case 5:
				case 12: case 13: case 14: {
					YMF262Channel& ch_3 = channels[chan_no - 3];
					if (ch_3.extended) {
						//if this is 2nd channel forming up 4-op channel just do nothing
					} else {
						//else normal 2 operator function
						// refresh Total Level in both SLOTs of this channel
						ch.slots[SLOT1].TLL = ch.slots[SLOT1].TL + (ch.ksl_base >> ch.slots[SLOT1].ksl);
						ch.slots[SLOT2].TLL = ch.slots[SLOT2].TL + (ch.ksl_base >> ch.slots[SLOT2].ksl);

						// refresh frequency counter in both SLOTs of this channel
						ch.CALC_FCSLOT(ch.slots[SLOT1]);
						ch.CALC_FCSLOT(ch.slots[SLOT2]);
					}
					break;
				}
				default:
					// refresh Total Level in both SLOTs of this channel
					ch.slots[SLOT1].TLL = ch.slots[SLOT1].TL + (ch.ksl_base >> ch.slots[SLOT1].ksl);
					ch.slots[SLOT2].TLL = ch.slots[SLOT2].TL + (ch.ksl_base >> ch.slots[SLOT2].ksl);

					// refresh frequency counter in both SLOTs of this channel
					ch.CALC_FCSLOT(ch.slots[SLOT1]);
					ch.CALC_FCSLOT(ch.slots[SLOT2]);
					break;
				}
			} else {
				// in OPL2 mode
				// refresh Total Level in both SLOTs of this channel
				ch.slots[SLOT1].TLL = ch.slots[SLOT1].TL + (ch.ksl_base >> ch.slots[SLOT1].ksl);
				ch.slots[SLOT2].TLL = ch.slots[SLOT2].TL + (ch.ksl_base >> ch.slots[SLOT2].ksl);

				// refresh frequency counter in both SLOTs of this channel
				ch.CALC_FCSLOT(ch.slots[SLOT1]);
				ch.CALC_FCSLOT(ch.slots[SLOT2]);
			}
		}
		break;
	}
	case 0xC0: {
		// CH.D, CH.C, CH.B, CH.A, FB(3bits), C
		if ((r & 0xF) > 8) {
			return;
		}
		int chan_no = (r & 0x0F) + ch_offset;
		YMF262Channel& ch = channels[chan_no];

		int base = chan_no * 4;
		if (OPL3_mode) {
			// OPL3 mode
			pan[base + 0] = (v & 0x10) ? (unsigned)~0 : 0;	// ch.A
			pan[base + 1] = (v & 0x20) ? (unsigned)~0 : 0;	// ch.B
			pan[base + 2] = (v & 0x40) ? (unsigned)~0 : 0;	// ch.C
			pan[base + 3] = (v & 0x80) ? (unsigned)~0 : 0;	// ch.D
		} else {
			// OPL2 mode - always enabled
			pan[base + 0] = (unsigned)~0;	// ch.A
			pan[base + 1] = (unsigned)~0;	// ch.B
			pan[base + 2] = (unsigned)~0;	// ch.C
			pan[base + 3] = (unsigned)~0;	// ch.D
		}

		ch.slots[SLOT1].setFeedbackShift((v >> 1) & 7);
		ch.slots[SLOT1].CON = v & 1;

		if (OPL3_mode) {
			switch(chan_no) {
			case 0: case 1: case 2:
			case 9: case 10: case 11:
				if (ch.extended) {
					YMF262Channel& ch3 = channels[chan_no + 3];
					switch((ch.slots[SLOT1].CON << 1) | ch3.slots[SLOT1].CON) {
					case 0:
						// 1 -> 2 -> 3 -> 4 - out
						ch.slots[SLOT1].connect = &phase_modulation;
						ch.slots[SLOT2].connect = &phase_modulation2;
						ch3.slots[SLOT1].connect = &phase_modulation;
						ch3.slots[SLOT2].connect = &chanout[chan_no + 3];
						break;

					case 1:
						// 1 -> 2 -\.
						// 3 -> 4 -+- out
						ch.slots[SLOT1].connect = &phase_modulation;
						ch.slots[SLOT2].connect = &chanout[chan_no];
						ch3.slots[SLOT1].connect = &phase_modulation;
						ch3.slots[SLOT2].connect = &chanout[chan_no + 3];
						break;

					case 2:
						// 1 -----------\.
						// 2 -> 3 -> 4 -+- out
						ch.slots[SLOT1].connect = &chanout[chan_no];
						ch.slots[SLOT2].connect = &phase_modulation2;
						ch3.slots[SLOT1].connect = &phase_modulation;
						ch3.slots[SLOT2].connect = &chanout[chan_no + 3];
						break;

					case 3:
						// 1 ------\.
						// 2 -> 3 -+- out
						// 4 ------/
						ch.slots[SLOT1].connect = &chanout[chan_no];
						ch.slots[SLOT2].connect = &phase_modulation2;
						ch3.slots[SLOT1].connect = &chanout[chan_no + 3];
						ch3.slots[SLOT2].connect = &chanout[chan_no + 3];
						break;
					}
				} else {
					// 2 operators mode
					ch.slots[SLOT1].connect = ch.slots[SLOT1].CON ? &chanout[chan_no] : &phase_modulation;
					ch.slots[SLOT2].connect = &chanout[chan_no];
				}
				break;

			case 3: case 4: case 5:
			case 12: case 13: case 14: {
				YMF262Channel& ch3 = channels[chan_no - 3];
				if (ch3.extended) {
					switch((ch3.slots[SLOT1].CON << 1) | ch.slots[SLOT1].CON) {
					case 0:
						// 1 -> 2 -> 3 -> 4 - out
						ch3.slots[SLOT1].connect = &phase_modulation;
						ch3.slots[SLOT2].connect = &phase_modulation2;
						ch.slots[SLOT1].connect = &phase_modulation;
						ch.slots[SLOT2].connect = &chanout[chan_no];
						break;

					case 1:
						// 1 -> 2 -\.
						// 3 -> 4 -+- out
						ch3.slots[SLOT1].connect = &phase_modulation;
						ch3.slots[SLOT2].connect = &chanout[chan_no - 3];
						ch.slots[SLOT1].connect = &phase_modulation;
						ch.slots[SLOT2].connect = &chanout[chan_no];
						break;

					case 2:
						// 1 -----------\.
						// 2 -> 3 -> 4 -+- out
						ch3.slots[SLOT1].connect = &chanout[chan_no - 3];
						ch3.slots[SLOT2].connect = &phase_modulation2;
						ch.slots[SLOT1].connect = &phase_modulation;
						ch.slots[SLOT2].connect = &chanout[chan_no];
						break;

					case 3:
						// 1 ------\.
						// 2 -> 3 -+- out
						// 4 ------/
						ch3.slots[SLOT1].connect = &chanout[chan_no - 3];
						ch3.slots[SLOT2].connect = &phase_modulation2;
						ch.slots[SLOT1].connect = &chanout[chan_no];
						ch.slots[SLOT2].connect = &chanout[chan_no];
						break;
					}
				} else {
					// 2 operators mode
					ch.slots[SLOT1].connect = ch.slots[SLOT1].CON ? &chanout[chan_no] : &phase_modulation;
					ch.slots[SLOT2].connect = &chanout[chan_no];
				}
				break;
			}
			default:
				// 2 operators mode
				ch.slots[SLOT1].connect = ch.slots[SLOT1].CON ? &chanout[chan_no] : &phase_modulation;
				ch.slots[SLOT2].connect = &chanout[chan_no];
				break;
			}
		} else {
			// OPL2 mode - always 2 operators mode
			ch.slots[SLOT1].connect = ch.slots[SLOT1].CON ? &chanout[chan_no] : &phase_modulation;
			ch.slots[SLOT2].connect = &chanout[chan_no];
		}
		break;
	}
	case 0xE0: {
		// waveform select
		int slot = slot_array[r & 0x1f];
		if (slot < 0) return;
		slot += ch_offset * 2;
		YMF262Channel& ch = channels[slot / 2];

		// store 3-bit value written regardless of current OPL2 or OPL3
		// mode... (verified on real YMF262)
		v &= 7;
		ch.slots[slot & 1].waveform_number = v;
		// ... but select only waveforms 0-3 in OPL2 mode
		if (!OPL3_mode) {
			v &= 3;
		}
		ch.slots[slot & 1].wavetable = &sin_tab[v * SIN_LEN];
		break;
	}
	}
}


void YMF262Impl::reset(const EmuTime& time)
{
	eg_cnt = 0;

	noise_rng = 1;	// noise shift register
	nts = false; // note split
	resetStatus(0x60);

	// reset with register write
	writeRegForce(0x01, 0, time); // test register
	writeRegForce(0x02, 0, time); // Timer1
	writeRegForce(0x03, 0, time); // Timer2
	writeRegForce(0x04, 0, time); // IRQ mask clear

	//FIX IT  registers 101, 104 and 105
	//FIX IT (dont change CH.D, CH.C, CH.B and CH.A in C0-C8 registers)
	for (int c = 0xFF; c >= 0x20; c--) {
		writeRegForce(c, 0, time);
	}
	//FIX IT (dont change CH.D, CH.C, CH.B and CH.A in C0-C8 registers)
	for (int c = 0x1FF; c >= 0x120; c--) {
		writeRegForce(c, 0, time);
	}

	// reset operator parameters
	for (int c = 0; c < 9 * 2; c++) {
		YMF262Channel& ch = channels[c];
		for (int s = 0; s < 2; s++) {
			ch.slots[s].state  = EG_OFF;
			ch.slots[s].volume = MAX_ATT_INDEX;
		}
	}
}

YMF262Impl::YMF262Impl(MSXMotherBoard& motherBoard, const std::string& name,
               const XMLElement& config, const EmuTime& time)
	: SoundDevice(motherBoard.getMSXMixer(), name, "MoonSound FM-part",
	              18, true)
	, Resample(motherBoard.getGlobalSettings(), 2)
	, debuggable(new YMF262Debuggable(motherBoard, *this))
	, timer1(motherBoard.getScheduler(), *this)
	, timer2(motherBoard.getScheduler(), *this)
	, irq(motherBoard.getCPU())
	, lfo_am_cnt(0), lfo_pm_cnt(0)
{
	LFO_AM = LFO_PM = 0;
	lfo_am_depth = false;
	lfo_pm_depth_range = 0;
	rhythm = 0;
	OPL3_mode = false;
	status = status2 = statusMask = 0;

	init_tables();

	reset(time);
	registerSound(config);
}

YMF262Impl::~YMF262Impl()
{
	unregisterSound();
}

byte YMF262Impl::readStatus()
{
	// no need to call updateStream(time)
	byte result = status | status2;
	status2 = 0;
	return result;
}

byte YMF262Impl::peekStatus() const
{
	return status | status2;
}

bool YMF262Impl::checkMuteHelper()
{
	// TODO this doesn't always mute when possible
	for (int i = 0; i < 18; i++) {
		for (int j = 0; j < 2; j++) {
			YMF262Slot& sl = channels[i].slots[j];
			if (!((sl.state == EG_OFF) ||
			      ((sl.state == EG_RELEASE) &&
			       ((sl.TLL + sl.volume) >= ENV_QUIET)))) {
				return false;
			}
		}
	}
	return true;
}

int YMF262Impl::adjust(int x)
{
	return x << 2;
}

void YMF262Impl::generateChannels(int** bufs, unsigned num)
{
	if (checkMuteHelper()) {
		// TODO update internal state, even if muted
		for (int i = 0; i < 18; ++i) {
			bufs[i] = 0;
		}
		return;
	}

	bool rhythmEnabled = rhythm & 0x20;

	for (unsigned j = 0; j < num; ++j) {
		advance_lfo();

		// clear channel outputs
		memset(chanout, 0, sizeof(int) * 18);

		// register set #1
		// extended 4op ch#0 part 1 or 2op ch#0
		channels[0].chan_calc(LFO_AM);
		if (channels[0].extended) {
			// extended 4op ch#0 part 2
			channels[3].chan_calc_ext(LFO_AM);
		} else {
			// standard 2op ch#3
			channels[3].chan_calc(LFO_AM);
		}

		// extended 4op ch#1 part 1 or 2op ch#1
		channels[1].chan_calc(LFO_AM);
		if (channels[1].extended) {
			// extended 4op ch#1 part 2
			channels[4].chan_calc_ext(LFO_AM);
		} else {
			// standard 2op ch#4
			channels[4].chan_calc(LFO_AM);
		}

		// extended 4op ch#2 part 1 or 2op ch#2
		channels[2].chan_calc(LFO_AM);
		if (channels[2].extended) {
			// extended 4op ch#2 part 2
			channels[5].chan_calc_ext(LFO_AM);
		} else {
			// standard 2op ch#5
			channels[5].chan_calc(LFO_AM);
		}

		if (!rhythmEnabled) {
			channels[6].chan_calc(LFO_AM);
			channels[7].chan_calc(LFO_AM);
			channels[8].chan_calc(LFO_AM);
		} else {
			// Rhythm part
			chan_calc_rhythm();
		}

		// register set #2
		channels[9].chan_calc(LFO_AM);
		if (channels[9].extended) {
			channels[12].chan_calc_ext(LFO_AM);
		} else {
			channels[12].chan_calc(LFO_AM);
		}

		channels[10].chan_calc(LFO_AM);
		if (channels[10].extended) {
			channels[13].chan_calc_ext(LFO_AM);
		} else {
			channels[13].chan_calc(LFO_AM);
		}

		channels[11].chan_calc(LFO_AM);
		if (channels[11].extended) {
			channels[14].chan_calc_ext(LFO_AM);
		} else {
			channels[14].chan_calc(LFO_AM);
		}

		// channels 15,16,17 are fixed 2-operator channels only
		channels[15].chan_calc(LFO_AM);
		channels[16].chan_calc(LFO_AM);
		channels[17].chan_calc(LFO_AM);

		for (int i = 0; i < 18; ++i) {
			bufs[i][2 * j + 0] = adjust(chanout[i] & pan[4 * i + 0]);
			bufs[i][2 * j + 1] = adjust(chanout[i] & pan[4 * i + 1]);
			//c += adjust(chanout[i] & pan[4 * i + 2]);  // unused
			//d += adjust(chanout[i] & pan[4 * i + 3]);  // unused
		}

		advance();
	}
}

bool YMF262Impl::generateInput(int* buffer, unsigned num)
{
	return mixChannels(buffer, num);
}

bool YMF262Impl::updateBuffer(unsigned length, int* buffer,
     const EmuTime& /*time*/, const EmuDuration& /*sampDur*/)
{
	return generateOutput(buffer, length);
}


// SimpleDebuggable

YMF262Debuggable::YMF262Debuggable(MSXMotherBoard& motherBoard, YMF262Impl& ymf262_)
	: SimpleDebuggable(motherBoard, ymf262_.getName() + " regs",
	                   "MoonSound FM-part registers", 0x200)
	, ymf262(ymf262_)
{
}

byte YMF262Debuggable::read(unsigned address)
{
	return ymf262.peekReg(address);
}

void YMF262Debuggable::write(unsigned address, byte value, const EmuTime& time)
{
	ymf262.writeRegForce(address, value, time);
}


// class YMF262
 
YMF262::YMF262(MSXMotherBoard& motherBoard, const std::string& name,
       const XMLElement& config, const EmuTime& time)
	: pimple(new YMF262Impl(motherBoard, name, config, time))
{
}

YMF262::~YMF262()
{
}

void YMF262::reset(const EmuTime& time)
{
	pimple->reset(time);
}

void YMF262::writeReg(int r, byte v, const EmuTime& time)
{
	pimple->writeReg(r, v, time);
}

byte YMF262::readReg(int reg)
{
	return pimple->readReg(reg);
}

byte YMF262::peekReg(int reg) const
{
	return pimple->peekReg(reg);
}

byte YMF262::readStatus()
{
	return pimple->readStatus();
}

byte YMF262::peekStatus() const
{
	return pimple->peekStatus();
}

} // namespace openmsx
