/* hacktv - Analogue video transmitter for the HackRF                    */
/*=======================================================================*/
/* Copyright 2017 Philip Heron <phil@sanslogic.co.uk>                    */
/*                                                                       */
/* This program is free software: you can redistribute it and/or modify  */
/* it under the terms of the GNU General Public License as published by  */
/* the Free Software Foundation, either version 3 of the License, or     */
/* (at your option) any later version.                                   */
/*                                                                       */
/* This program is distributed in the hope that it will be useful,       */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of        */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         */
/* GNU General Public License for more details.                          */
/*                                                                       */
/* You should have received a copy of the GNU General Public License     */
/* along with this program.  If not, see <http://www.gnu.org/licenses/>. */

/* -=== Videocrypt encoder ===-
 * 
 * This is a Videocrypt I encoder. It scrambles the image using a technique
 * called "line cut-and-rotate", and inserts the necessary data into the
 * VBI area of the image to activate the Videocrypt hardware unscrambler.
 * 
 * There are no details of the PRNG published that I could find, so the
 * cut points used here where captured by analysing how the real hardware
 * attempts to unscramble an already clear image.
 * 
 * THANKS
 * 
 * Markus Kuhn and William Andrew Steer for their detailed descriptions
 * and examples of how Videocrypt works:
 * 
 * https://www.cl.cam.ac.uk/~mgk25/tv-crypt/
 * http://www.techmind.org/vdc/
 * 
 * Ralph Metzler for the details of how the VBI data is encoded:
 * 
 * http://src.gnu-darwin.org/ports/misc/vbidecode/work/bttv/apps/vbidecode/vbidecode.cc
 * 
 * Alex L. James for providing an active Sky subscriber card, VBI samples and testing.
 *
*/

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "video.h"

/* Packet header sequence */
static const uint8_t _sequence[8] = {
	0x87,0x96,0xA5,0xB4,0xC3,0xD2,0xE1,0x87,
};

/* Hamming codes */
static const uint8_t _hamming[16] = {
	0x15,0x02,0x49,0x5E,0x64,0x73,0x38,0x2F,
	0xD0,0xC7,0x8C,0x9B,0xA1,0xB6,0xFD,0xEA,
};

/* Sequence for free-access decoding */
static const _vc_block_t _fa_blocks[] = {
	{
		0x05, 0xFFFFFFFFFFFFFFFUL,
		{
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
		},
	},
};

/* Sequence for Conditional-access sample, taken from MTV UK and modified. */
/* Requires an active Sky card to decode. */
static const _vc_block_t _mtv_blocks[] = {
	{
		0x07, 0xB2DD55A7BCE178EUL,
		{
			{ 0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0xF8,0x19,0x10,0x83,0x20,0x85,0x60,0xAF,0x8F,0xF0,0x49,0x34,0x86,0xC4,0x6A,0xCA,0xC3,0x21,0x4D,0x44,0xB3,0x24,0x36,0x57,0xEC,0xA7,0xCE,0x12,0x38,0x91,0x3E,0x00, },
		},
	},
	{
		0x07, 0xF9885DA50770B80UL,
		{
			/* Modify the following line to change the channel name displayed by the decoder.
			 * The third byte is 0x60 + number of characters, followed by the ASCII characters themselves. */
			{ 0x20,0x00,0x69,0x20,0x20,0x20,0x48,0x41,0x43,0x4B,0x54,0x56,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, },
			{ 0xF8,0x19,0x10,0x83,0x20,0xD1,0xB5,0xA9,0x1F,0x82,0xFE,0xB3,0x6B,0x0A,0x82,0xC3,0x30,0x7B,0x65,0x9C,0xF2,0xBD,0x5C,0xB0,0x6A,0x3B,0x64,0x0F,0xA2,0x66,0xBB,0x00, },
		},
	},
};

/* Reverse bits in an 8-bit value */
static uint8_t _reverse(uint8_t b)
{
	b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
	b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
	b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
	return(b);
}

/* Reverse bits in an x-bit value */
static uint64_t _rev(uint64_t b, int x)
{
	uint64_t r = 0;
	
	while(x--)
	{
		r = (r << 1) | (b & 1);
		b >>= 1;
	}
	

	return(r);
}

/* Generate IW for PRBS */
static uint64_t _generate_iw(uint64_t cw, uint8_t fcnt)
{
	uint64_t iw;
	
	/* FCNT is repeated 8 times, each time inverted */
	iw  = ((fcnt ^ 0xFF) << 8) | fcnt;
	iw |= (iw << 16) | (iw << 32) | (iw << 48);
	
	return((iw ^ cw) & VC_PRBS_CW_MASK);
}

/* Apply VBI frame interleaving */
static void _interleave(uint8_t *frame)
{
	int b, i, j;
	int offset[6] = { 0, 6, 12, 20, 26, 32 };
	uint8_t r[8];
	uint8_t m;
	
	for(b = 0; b < 6; b++)
	{
		uint8_t *s = frame + offset[b];
		
		s[0] = _reverse(s[0]);
		s[7] = _reverse(s[7]);
		
		for(i = 0, m = 0x80; i < 8; i++, m >>= 1)
		{
			r[i] = 0x00;
			for(j = 0; j < 8; j++)
			{
				r[i] |= ((m & s[j]) ? 1 : 0) << j;
			}
		}
		
		memcpy(s, r, 8);
	}
}

int vc_init(vc_t *s, vid_t *vid, const char *mode)
{
	double f, l;
	int x;
	
	memset(s, 0, sizeof(vc_t));
	
	s->vid      = vid;
	s->counter  = 0;
	s->cw       = VC_PRBS_CW_FA;
	
	if(strcmp(mode, "free") == 0)
	{
		s->blocks    = _fa_blocks;
		s->block_len = 1;
	}
	else if(strcmp(mode, "conditional") == 0)
	{
		s->blocks    = _mtv_blocks;
		s->block_len = 2;
	}
	else
	{
		fprintf(stderr, "Unrecognised videocrypt mode '%s'.\n", mode);
		return(VID_ERROR);
	}
	
	s->block = 0;
	
	/* Sample rate ratio */
	f = (double) s->vid->width / VC_WIDTH;
	
	/* Videocrypt timings appear to be calculated against the centre of the hsync pulse */
	l = (double) VC_SAMPLE_RATE * s->vid->conf.hsync_width / 2;
	
	/* Quick and dirty sample rate conversion array */
	for(x = 0; x < VC_WIDTH; x++)
	{
		s->video_scale[x] = round((l + x) * f);
	}
	
	/* Allocate memory for the 1-line delay */
	s->vid->delay += 1;
	s->delay = calloc(2 * vid->width, sizeof(int16_t));
	if(!s->delay)
	{
		return(VID_OUT_OF_MEMORY);
	}
	
	return(VID_OK);
}

void vc_free(vc_t *s)
{
	if(s->delay)
	{
		free(s->delay);
	}
}

void vc_render_line(vc_t *s)
{
	int x;
	const uint8_t *bline = NULL;
	
	/* On the first line of each frame, generate the VBI data */
	if(s->vid->line == 1)
	{
		uint64_t iw;
		uint8_t crc;
		
		if((s->counter & 7) == 0)
		{
			/* The active message is updated every 8 frames. The last
			 * message in the block is a duplicate of the first. */
			for(crc = x = 0; x < 31; x++)
			{
				crc += s->message[x] = s->blocks[s->block].messages[((s->counter >> 3) & 7) % 7][x];
			}
			
			s->message[x] = ~crc + 1;
		}
		
		if((s->counter & 4) == 0)
		{
			/* The first half of the message. Transmitted for 4 frames */
			
			crc = s->vbi[0] = _sequence[(s->counter >> 4) & 7];
			for(x = 0; x < 8; x++)
			{
				crc += s->vbi[1 + x] = s->message[0 + x];
			}
			s->vbi[9] = crc;
			
			crc = s->vbi[10] = s->counter & 0xFF;
			for(x = 0; x < 8; x++)
			{
				crc += s->vbi[11 + x] = s->message[8 + x];
			}
			s->vbi[19] = crc;
		}
		else
		{
			/* The second half of the message. Transmitted for four frames */
			x = _sequence[(s->counter >> 4) & 7];
			crc = s->vbi[0] = ((x & 0x0F) << 4) | ((x & 0xF0) >> 4);
			for(x = 0; x < 8; x++)
			{
				crc += s->vbi[1 + x] = s->message[16 + x];
			}
			s->vbi[9] = crc;
			
			crc = s->vbi[10] = s->blocks[s->block].mode;
			for(x = 0; x < 8; x++)
			{
				crc += s->vbi[11 + x] = s->message[24 + x];
			}
			s->vbi[19] = crc;
		}
		
		/* Hamming code the VBI data */
		for(x = 19; x >= 0; x--)
		{
			s->vbi[x * 2 + 1] = _hamming[s->vbi[x] & 0x0F];
			s->vbi[x * 2 + 0] = _hamming[s->vbi[x] >> 4];
		}
		
		/* Interleave the VBI data */
		_interleave(s->vbi);
		
		/* Reset the PRBS */
		iw = _generate_iw(s->cw, s->counter);
		s->sr1 = iw & VC_PRBS_SR1_MASK;
		s->sr2 = (iw >> 31) & VC_PRBS_SR2_MASK;
		
		/* After 64 frames, advance to the next command */
		s->counter++;
		
		if((s->counter & 0x3F) == 0)
		{
			/* Apply the current block codeword */
			s->cw = s->blocks[s->block].codeword;
			
			/* Move to the next block */
			s->block++;
			
			if(s->block == s->block_len)
			{
				s->block = 0;
			}
		}
	}
	
	/* Calculate VBI line, or < 0 if not */
	if(s->vid->line >= VC_VBI_FIELD_1_START && s->vid->line < VC_VBI_FIELD_1_START + VC_VBI_LINES_PER_FIELD)
	{
		/* Top VBI field */
		bline = &s->vbi[(s->vid->line - VC_VBI_FIELD_1_START) * VC_VBI_BYTES_PER_LINE];
	}
	else if(s->vid->line >= VC_VBI_FIELD_2_START && s->vid->line < VC_VBI_FIELD_2_START + VC_VBI_LINES_PER_FIELD)
	{
		/* Bottom VBI field */
		bline = &s->vbi[(s->vid->line - VC_VBI_FIELD_2_START + VC_VBI_LINES_PER_FIELD) * VC_VBI_BYTES_PER_LINE];
	}
	
	/* Render the VBI line if necessary */
	if(bline)
	{
		int b, c;
		
		x = s->video_scale[VC_VBI_LEFT];
		
		for(b = 0; b < VC_VBI_BITS_PER_LINE; b++)
		{
			c = (bline[b / 8] >> (b % 8)) & 1;
			c = c ? 0xFFFFFF : 0x000000;
			
			for(; x < s->video_scale[VC_VBI_LEFT + VC_VBI_SAMPLES_PER_BIT * (b + 1)]; x++)
			{
				s->vid->output[x * 2] = s->vid->y_level_lookup[c];
			}
		}
	}
	
	/* Scramble the line if necessary */
	x = -1;
	
	if((s->vid->line >= VC_FIELD_1_START && s->vid->line < VC_FIELD_1_START + VC_LINES_PER_FIELD) ||
	   (s->vid->line >= VC_FIELD_2_START && s->vid->line < VC_FIELD_2_START + VC_LINES_PER_FIELD))
	{
		int i;
		
		x = (s->c >> 8) & 0xFF;
		
		for(i = 0; i < 16; i++)
		{
			int a;
			
			/* Update shift registers */
			s->sr1 = (s->sr1 >> 1) ^ (s->sr1 & 1 ? 0x7BB88888UL : 0);
			s->sr2 = (s->sr2 >> 1) ^ (s->sr2 & 1 ? 0x17A2C100UL : 0);
			
			/* Load the multiplexer address */
			a = _rev(s->sr2, 29) & 0x1F;
			if(a == 31) a = 30;
			
			/* Shift into result register */
			s->c = (s->c >> 1) | (((_rev(s->sr1, 31) >> a) & 1) << 15);
		}
	}
	
	/* Hack to preserve WSS signal data */
	if(s->vid->line == 24) x = -1;
	
	if(x != -1)
	{
		int cut;
		int lshift;
		int y;
		
		cut = 105 + (0xFF - x) * 2;
		lshift = 710 - cut;
		
		y = s->video_scale[VC_LEFT + lshift];
		for(x = s->video_scale[VC_LEFT]; x < s->video_scale[VC_LEFT + cut]; x++, y++)
		{
			s->delay[x * 2] = s->vid->output[y * 2];
		}
		
		y = s->video_scale[VC_LEFT];
		for(; x < s->video_scale[VC_RIGHT + VC_OVERLAP]; x++, y++)
		{
			s->delay[x * 2] = s->vid->output[y * 2];
		}
	}
	
	/* Delay by 1 line. Uses the Q part for temporary storage */
	for(x = 0; x < s->vid->width * 2; x += 2)
	{
		s->delay[x + 1] = s->vid->output[x];
		s->vid->output[x] = s->delay[x];
		s->delay[x] = s->delay[x + 1];
	}
}

