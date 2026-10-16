// YM2413 (OPLL) for VGM files, on emu2413 (ext/), which GME already ships
// for the NES VRC7. Upstream 0.6.5 has only a stub here ("YM2413 FM sound
// isn't supported"); this file is Kiplay's, not GME's.
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Ym2413_Emu.h"

#include "ext/emu2413.h"

#define OPLL_OF( p ) ( (OPLL*) (p) )

Ym2413_Emu::Ym2413_Emu() : opll( 0 ) { }

Ym2413_Emu::~Ym2413_Emu()
{
	if ( opll )
		OPLL_delete( OPLL_OF( opll ) );
}

int Ym2413_Emu::set_rate( double sample_rate, double clock_rate )
{
	if ( opll )
		OPLL_delete( OPLL_OF( opll ) );
	opll = OPLL_new( (uint32_t) clock_rate, (uint32_t) sample_rate );
	if ( !opll )
		return 1;
	OPLL_setChipType( OPLL_OF( opll ), 0 ); // YM2413, not the VRC7
	reset();
	return 0;
}

void Ym2413_Emu::reset()
{
	OPLL_reset( OPLL_OF( opll ) );
	OPLL_resetPatch( OPLL_OF( opll ), 0 );
}

void Ym2413_Emu::write( int addr, int data )
{
	OPLL_writeReg( OPLL_OF( opll ), (uint32_t) addr, (uint8_t) data );
}

void Ym2413_Emu::mute_voices( int mask )
{
	OPLL_setMask( OPLL_OF( opll ), (uint32_t) mask );
}

// Adds to out, like the YM2612: two chips may share the buffer.
void Ym2413_Emu::run( int pair_count, sample_t* out )
{
	while ( pair_count-- > 0 )
	{
		int32_t s [2];
		OPLL_calcStereo( OPLL_OF( opll ), s );
		for ( int i = 0; i < 2; i++ )
		{
			int32_t t = out [i] + s [i];
			if ( (int16_t) t != t )
				t = 0x7FFF ^ (t >> 31);
			out [i] = (sample_t) t;
		}
		out += 2;
	}
}
