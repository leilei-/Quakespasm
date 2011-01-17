/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// snd_mix.c -- portable code to mix sounds for snd_dma.c

#include "quakedef.h"

#define	PAINTBUFFER_SIZE	512

/**
 * The paintbuffer is an array of struct { int left, int right } 's.
 * The int's are signed 24-bit samples which will be truncated to
 * signed 16-bit before output.
 */
portable_samplepair_t paintbuffer[PAINTBUFFER_SIZE];
int		snd_scaletable[32][256];
int		*snd_p, snd_linear_count, snd_vol;
short		*snd_out;

void Snd_WriteLinearBlastStereo16 (void)
{
	int		i;
	int		val;
	
	for (i=0 ; i<snd_linear_count ; i+=2)
	{
		val = snd_p[i]>>8;
		if (val > 32767)
			snd_out[i] = 32767;
		else if (val < -32768)
			snd_out[i] = -32768;
		else
			snd_out[i] = val;
		
		val = snd_p[i+1]>>8;
		if (val > 32767)
			snd_out[i+1] = 32767;
		else if (val < -32768)
			snd_out[i+1] = -32768;
		else
			snd_out[i+1] = val;
	}
}

void S_TransferStereo16 (int endtime)
{
	int		lpos;
	int		lpaintedtime;

	snd_p = (int *) paintbuffer;
	lpaintedtime = paintedtime;

	while (lpaintedtime < endtime)
	{
	// handle recirculating buffer issues
		lpos = lpaintedtime & ((shm->samples >> 1) - 1);

		snd_out = (short *)shm->buffer + (lpos << 1);

		snd_linear_count = (shm->samples >> 1) - lpos;
		if (lpaintedtime + snd_linear_count > endtime)
			snd_linear_count = endtime - lpaintedtime;

		snd_linear_count <<= 1;

	// write a linear blast of samples
		Snd_WriteLinearBlastStereo16 ();

		snd_p += snd_linear_count;
		lpaintedtime += (snd_linear_count >> 1);
	}
}

void S_TransferPaintBuffer(int endtime)
{
	int	out_idx, out_mask;
	int	count, step, val;
	int	*p;

	if (shm->samplebits == 16 && shm->channels == 2)
	{
		S_TransferStereo16 (endtime);
		return;
	}
	
	p = (int *) paintbuffer;
	count = (endtime - paintedtime) * shm->channels;
	out_mask = shm->samples - 1;
	out_idx = paintedtime * shm->channels & out_mask;
	step = 3 - shm->channels;

	if (shm->samplebits == 16)
	{
		short *out = (short *)shm->buffer;
		while (count--)
		{
			val = *p >> 8;
			p+= step;
			if (val > 0x7fff)
				val = 0x7fff;
			else if (val < (short)0x8000)
				val = (short)0x8000;
			out[out_idx] = val;
			out_idx = (out_idx + 1) & out_mask;
		}
	}
	else if (shm->samplebits == 8)
	{
		unsigned char *out = shm->buffer;
		while (count--)
		{
			val = *p >> 8;
			p+= step;
			if (val > 0x7fff)
				val = 0x7fff;
			else if (val < (short)0x8000)
				val = (short)0x8000;
			out[out_idx] = (val >> 8) + 128;
			out_idx = (out_idx + 1) & out_mask;
		}
	}
}


/*
===============================================================================

CHANNEL MIXING

===============================================================================
*/

void SND_PaintChannelFrom8 (channel_t *ch, sfxcache_t *sc, int endtime);
void SND_PaintChannelFrom16 (channel_t *ch, sfxcache_t *sc, int endtime);

void S_PaintChannels (int endtime)
{
	int		i;
	int		end, ltime, count;
	int stream;
	channel_t	*ch;
	sfxcache_t	*sc;

	snd_vol = sfxvolume.value * 255;
	
	while (paintedtime < endtime)
	{
	// if paintbuffer is smaller than DMA buffer
		end = endtime;
		if (endtime - paintedtime > PAINTBUFFER_SIZE)
			end = paintedtime + PAINTBUFFER_SIZE;

	// clear the paint buffer and mix any raw samples... (e.g. OGG music)
		memset(paintbuffer, 0, sizeof(paintbuffer));
		for (stream = 0; stream < MAX_RAW_STREAMS; stream++) {
			if ( s_rawend[stream] >= paintedtime ) {
				// copy from the streaming sound source
				const portable_samplepair_t *rawsamples = s_rawsamples[stream];
				const int stop = (end < s_rawend[stream]) ? end : s_rawend[stream];
				for ( i = paintedtime ; i < stop ; i++ ) {
					const int s = i&(MAX_RAW_SAMPLES-1);
					paintbuffer[i-paintedtime].left += rawsamples[s].left;
					paintbuffer[i-paintedtime].right += rawsamples[s].right;
				}
			}
		}
		
	// paint in the sfx channels.
		ch = snd_channels;
		for (i = 0; i < total_channels; i++, ch++)
		{
			if (!ch->sfx)
				continue;
			if (!ch->leftvol && !ch->rightvol)
				continue;
			sc = S_LoadSound (ch->sfx);
			if (!sc)
				continue;

			ltime = paintedtime;

			while (ltime < end)
			{	// paint up to end
				if (ch->end < end)
					count = ch->end - ltime;
				else
					count = end - ltime;

				if (count > 0)
				{
					if (sc->width == 1)
						SND_PaintChannelFrom8(ch, sc, count);
					else
						SND_PaintChannelFrom16(ch, sc, count);

					ltime += count;
				}

			// if at end of loop, restart
				if (ltime >= ch->end)
				{
					if (sc->loopstart >= 0)
					{
						ch->pos = sc->loopstart;
						ch->end = ltime + sc->length - ch->pos;
					}
					else
					{	// channel just stopped
						ch->sfx = NULL;
						break;
					}
				}
			}
		}

	// transfer out according to DMA format
		S_TransferPaintBuffer(end);
		paintedtime = end;
	}
}

void SND_InitScaletable (void)
{
	int		i, j;

	for (i = 0; i < 32; i++)
	{
		for (j = 0; j < 256; j++)
		/* When compiling with gcc-4.1.0 at optimisations O1 and
		   higher, the tricky signed char type conversion is not
		   guaranteed. Therefore we explicity calculate the signed
		   value from the index as required. From Kevin Shanahan.
		   See: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=26719
		*/
		//	snd_scaletable[i][j] = ((signed char)j) * i * 8;
			snd_scaletable[i][j] = ((j < 128) ? j : j - 0xff) * i * 8;
	}
}


void SND_PaintChannelFrom8 (channel_t *ch, sfxcache_t *sc, int count)
{
	int	data;
	int		*lscale, *rscale;
	unsigned char	*sfx;
	int		i;

	if (ch->leftvol > 255)
		ch->leftvol = 255;
	if (ch->rightvol > 255)
		ch->rightvol = 255;

	lscale = snd_scaletable[ch->leftvol >> 3];
	rscale = snd_scaletable[ch->rightvol >> 3];
	sfx = (unsigned char *) ((signed char *)sc->data + ch->pos);

	for (i = 0; i < count; i++)
	{
		data = sfx[i];
		paintbuffer[i].left += lscale[data] * snd_vol;
		paintbuffer[i].right += rscale[data] * snd_vol;
	}

	ch->pos += count;
}

void SND_PaintChannelFrom16 (channel_t *ch, sfxcache_t *sc, int count)
{
	int	data;
	int	left, right;
	int	leftvol, rightvol;
	signed short	*sfx;
	int	i;

	leftvol = ch->leftvol * snd_vol;
	rightvol = ch->rightvol * snd_vol;
	sfx = (signed short *)sc->data + ch->pos;

	for (i = 0; i < count; i++)
	{
		data = sfx[i];
		left = (data * leftvol) >> 8;
		right = (data * rightvol) >> 8;
		paintbuffer[i].left += left;
		paintbuffer[i].right += right;
	}

	ch->pos += count;
}

