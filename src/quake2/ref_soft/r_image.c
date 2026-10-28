/*
Copyright (C) 1997-2001 Id Software, Inc.

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

#include "r_local.h"
#include "../qcommon/q_perf.h"
#include "../other/q_arena.h"

int		FS_FOpenFile (char *filename, FILE **file);
void	FS_Read (void *buffer, int len, FILE *f);
void	FS_ReadAt (FILE *h, int base, int ofs, void *buffer, int len);

int		r_image_bytes;		// resident image data
qboolean	r_registering;		// ESP32: between R_BeginRegistration and R_EndRegistration
const void *QG_FindAsset (const char *name, int *size);

// ESP32: frees the pixels of an image, unless they are in flash
static void R_FreeImagePixels (image_t *image)
{
	byte *p = image->pixels[image->minmip];

	if (p && Q_Arena_Contains (p))
	{
		r_image_bytes -= Q_Arena_BlockSize (p);
		Q_Arena_Free (p);
	}
}
int		r_wal_minmip;		// ESP32: first mip level to load for the next .wal (see Mod_LoadTexinfo)
// ESP32: the images of the map (walls, sprites, skins) are allocated from the
// top of the arena with the other map data, the pics (kept from map to map)
// from the bottom: a map change then frees one contiguous block
static qboolean	r_image_permanent = true;


#define	MAX_RIMAGES	512	// ESP32: 1024 originally
image_t		r_images[MAX_RIMAGES] EXT_RAM_BSS;
int			numr_images;


/*
===============
R_ImageList_f
===============
*/
void	R_ImageList_f (void)
{
	int		i;
	image_t	*image;
	int		texels;

	ri.Con_Printf (PRINT_ALL, "------------------\n");
	texels = 0;

	for (i=0, image=r_images ; i<numr_images ; i++, image++)
	{
		if (image->registration_sequence <= 0)
			continue;
		texels += image->width*image->height;
		switch (image->type)
		{
		case it_skin:
			ri.Con_Printf (PRINT_ALL, "M");
			break;
		case it_sprite:
			ri.Con_Printf (PRINT_ALL, "S");
			break;
		case it_wall:
			ri.Con_Printf (PRINT_ALL, "W");
			break;
		case it_pic:
			ri.Con_Printf (PRINT_ALL, "P");
			break;
		default:
			ri.Con_Printf (PRINT_ALL, " ");
			break;
		}

		ri.Con_Printf (PRINT_ALL,  " %3i %3i : %s\n",
			image->width, image->height, image->name);
	}
	ri.Con_Printf (PRINT_ALL, "Total texel count: %i\n", texels);
}


/*
=================================================================

PCX LOADING

=================================================================
*/

/*
==============
R_LoadSkyPCX

ESP32: a 256x256 sky side is read and decoded in the surface cache (unused
while a map is loaded), only the half resolution result (16 kB, drawn as
mip level 1 by D_SkySurf) is allocated. NULL if not possible: LoadPCX.
==============
*/
static byte *R_LoadSkyPCX (char *filename, int *width, int *height)
{
	FILE	*h;
	pcx_t	*pcx;
	byte	*sc, *raw, *end, *full, *pix, *out;
	int		len, scsize, x, y, dataByte, runLength;

	len = FS_FOpenFile (filename, &h);
	if (!h)
		return NULL;
	sc = D_BorrowSurfaceCache (&scsize);
	if (!sc || len + 256*256 > scsize)
	{
		if (sc)
			D_ReturnSurfaceCache ();
		fclose (h);
		return NULL;
	}
	FS_ReadAt (h, ftell (h), 0, sc, len);
	fclose (h);
	pcx = (pcx_t *)sc;
	if (pcx->manufacturer != 0x0a || pcx->version != 5 || pcx->encoding != 1
		|| pcx->bits_per_pixel != 8 || LittleShort (pcx->xmax) != 255 || LittleShort (pcx->ymax) != 255)
	{
		D_ReturnSurfaceCache ();
		return NULL;
	}
	// decoded at the end of the surface cache, after the file
	full = sc + scsize - 256*256;
	raw = &pcx->data;
	end = sc + len;
	for (y=0, pix=full ; y<256 ; y++, pix+=256)
	{
		for (x=0 ; x<256 && raw < end ; )
		{
			dataByte = *raw++;
			if ((dataByte & 0xC0) == 0xC0)
			{
				runLength = dataByte & 0x3F;
				dataByte = raw < end ? *raw++ : 0;
			}
			else
				runLength = 1;
			// a run past the end of a line spills into the next one (as LoadPCX)
			while (runLength-- > 0 && pix + x < full + 256*256)
				pix[x++] = dataByte;
		}
	}
	out = Q_Arena_AllocTop (128*128);
	if (out)
	{
		for (y=0 ; y<128 ; y++)
			for (x=0 ; x<128 ; x++)
				out[y*128 + x] = full[y*2*256 + x*2];
	}
	D_ReturnSurfaceCache ();
	if (!out)
		ri.Sys_Error (ERR_DROP, "LoadPCX: out of memory for %s", filename);
	*width = *height = 128;
	return out;
}

/*
==============
LoadPCX
==============
*/
void LoadPCX (char *filename, byte **pic, byte **palette, int *width, int *height)
{
	byte	*raw;
	pcx_t	*pcx;
	int		x, y;
	int		len;
	int		dataByte, runLength;
	byte	*out, *pix;

	*pic = NULL;

	//
	// load the file
	//
	len = ri.FS_LoadFile (filename, (void **)&raw);
	if (!raw)
	{
		ri.Con_Printf (PRINT_DEVELOPER, "Bad pcx file %s\n", filename);
		return;
	}

	//
	// parse the PCX file
	//
	pcx = (pcx_t *)raw;

    pcx->xmin = LittleShort(pcx->xmin);
    pcx->ymin = LittleShort(pcx->ymin);
    pcx->xmax = LittleShort(pcx->xmax);
    pcx->ymax = LittleShort(pcx->ymax);
    pcx->hres = LittleShort(pcx->hres);
    pcx->vres = LittleShort(pcx->vres);
    pcx->bytes_per_line = LittleShort(pcx->bytes_per_line);
    pcx->palette_type = LittleShort(pcx->palette_type);

	raw = &pcx->data;

	if (pcx->manufacturer != 0x0a
		|| pcx->version != 5
		|| pcx->encoding != 1
		|| pcx->bits_per_pixel != 8
		|| pcx->xmax >= 640
		|| pcx->ymax >= 480)
	{
		ri.Con_Printf (PRINT_ALL, "Bad pcx file %s\n", filename);
		return;
	}

	out = r_image_permanent ? Q_Arena_Alloc ( (pcx->ymax+1) * (pcx->xmax+1) )
		: Q_Arena_AllocTop ( (pcx->ymax+1) * (pcx->xmax+1) );
	if (!out)
		ri.Sys_Error (ERR_DROP, "LoadPCX: out of memory for %s", filename);

	*pic = out;

	pix = out;

	if (palette)
	{
		*palette = malloc(768);
		memcpy (*palette, (byte *)pcx + len - 768, 768);
	}

	if (width)
		*width = pcx->xmax+1;
	if (height)
		*height = pcx->ymax+1;

	for (y=0 ; y<=pcx->ymax ; y++, pix += pcx->xmax+1)
	{
		for (x=0 ; x<=pcx->xmax ; )
		{
			dataByte = *raw++;

			if((dataByte & 0xC0) == 0xC0)
			{
				runLength = dataByte & 0x3F;
				dataByte = *raw++;
			}
			else
				runLength = 1;

			while(runLength-- > 0)
				pix[x++] = dataByte;
		}

	}

	if ( raw - (byte *)pcx > len)
	{
		ri.Con_Printf (PRINT_DEVELOPER, "PCX file %s was malformed", filename);
		free (*pic);
		*pic = NULL;
	}

	ri.FS_FreeFile (pcx);
}

/*
=========================================================

TARGA LOADING

=========================================================
*/

typedef struct _TargaHeader {
	unsigned char 	id_length, colormap_type, image_type;
	unsigned short	colormap_index, colormap_length;
	unsigned char	colormap_size;
	unsigned short	x_origin, y_origin, width, height;
	unsigned char	pixel_size, attributes;
} TargaHeader;


/*
=============
LoadTGA
=============
*/
void LoadTGA (char *name, byte **pic, int *width, int *height)
{
	int		columns, rows, numPixels;
	byte	*pixbuf;
	int		row, column;
	byte	*buf_p;
	byte	*buffer;
	int		length;
	TargaHeader		targa_header;
	byte			*targa_rgba;

	*pic = NULL;

	//
	// load the file
	//
	length = ri.FS_LoadFile (name, (void **)&buffer);
	if (!buffer)
	{
		ri.Con_Printf (PRINT_DEVELOPER, "Bad tga file %s\n", name);
		return;
	}

	buf_p = buffer;

	targa_header.id_length = *buf_p++;
	targa_header.colormap_type = *buf_p++;
	targa_header.image_type = *buf_p++;
	
	targa_header.colormap_index = LittleShort ( *((short *)buf_p) );
	buf_p+=2;
	targa_header.colormap_length = LittleShort ( *((short *)buf_p) );
	buf_p+=2;
	targa_header.colormap_size = *buf_p++;
	targa_header.x_origin = LittleShort ( *((short *)buf_p) );
	buf_p+=2;
	targa_header.y_origin = LittleShort ( *((short *)buf_p) );
	buf_p+=2;
	targa_header.width = LittleShort ( *((short *)buf_p) );
	buf_p+=2;
	targa_header.height = LittleShort ( *((short *)buf_p) );
	buf_p+=2;
	targa_header.pixel_size = *buf_p++;
	targa_header.attributes = *buf_p++;

	if (targa_header.image_type!=2 
		&& targa_header.image_type!=10) 
		ri.Sys_Error (ERR_DROP, "LoadTGA: Only type 2 and 10 targa RGB images supported\n");

	if (targa_header.colormap_type !=0 
		|| (targa_header.pixel_size!=32 && targa_header.pixel_size!=24))
		ri.Sys_Error (ERR_DROP, "LoadTGA: Only 32 or 24 bit images supported (no colormaps)\n");

	columns = targa_header.width;
	rows = targa_header.height;
	numPixels = columns * rows;

	if (width)
		*width = columns;
	if (height)
		*height = rows;

	targa_rgba = malloc (numPixels*4);
	*pic = targa_rgba;

	if (targa_header.id_length != 0)
		buf_p += targa_header.id_length;  // skip TARGA image comment
	
	if (targa_header.image_type==2) {  // Uncompressed, RGB images
		for(row=rows-1; row>=0; row--) {
			pixbuf = targa_rgba + row*columns*4;
			for(column=0; column<columns; column++) {
				unsigned char red,green,blue,alphabyte;
				switch (targa_header.pixel_size) {
					case 24:
							
							blue = *buf_p++;
							green = *buf_p++;
							red = *buf_p++;
							*pixbuf++ = red;
							*pixbuf++ = green;
							*pixbuf++ = blue;
							*pixbuf++ = 255;
							break;
					case 32:
							blue = *buf_p++;
							green = *buf_p++;
							red = *buf_p++;
							alphabyte = *buf_p++;
							*pixbuf++ = red;
							*pixbuf++ = green;
							*pixbuf++ = blue;
							*pixbuf++ = alphabyte;
							break;
				}
			}
		}
	}
	else if (targa_header.image_type==10) {   // Runlength encoded RGB images
		unsigned char red,green,blue,alphabyte,packetHeader,packetSize,j;
		for(row=rows-1; row>=0; row--) {
			pixbuf = targa_rgba + row*columns*4;
			for(column=0; column<columns; ) {
				packetHeader= *buf_p++;
				packetSize = 1 + (packetHeader & 0x7f);
				if (packetHeader & 0x80) {        // run-length packet
					switch (targa_header.pixel_size) {
						case 24:
								blue = *buf_p++;
								green = *buf_p++;
								red = *buf_p++;
								alphabyte = 255;
								break;
						case 32:
								blue = *buf_p++;
								green = *buf_p++;
								red = *buf_p++;
								alphabyte = *buf_p++;
								break;
					}
	
					for(j=0;j<packetSize;j++) {
						*pixbuf++=red;
						*pixbuf++=green;
						*pixbuf++=blue;
						*pixbuf++=alphabyte;
						column++;
						if (column==columns) { // run spans across rows
							column=0;
							if (row>0)
								row--;
							else
								goto breakOut;
							pixbuf = targa_rgba + row*columns*4;
						}
					}
				}
				else {                            // non run-length packet
					for(j=0;j<packetSize;j++) {
						switch (targa_header.pixel_size) {
							case 24:
									blue = *buf_p++;
									green = *buf_p++;
									red = *buf_p++;
									*pixbuf++ = red;
									*pixbuf++ = green;
									*pixbuf++ = blue;
									*pixbuf++ = 255;
									break;
							case 32:
									blue = *buf_p++;
									green = *buf_p++;
									red = *buf_p++;
									alphabyte = *buf_p++;
									*pixbuf++ = red;
									*pixbuf++ = green;
									*pixbuf++ = blue;
									*pixbuf++ = alphabyte;
									break;
						}
						column++;
						if (column==columns) { // pixel packet run spans across rows
							column=0;
							if (row>0)
								row--;
							else
								goto breakOut;
							pixbuf = targa_rgba + row*columns*4;
						}						
					}
				}
			}
			breakOut:;
		}
	}

	ri.FS_FreeFile (buffer);
}


//=======================================================

image_t *R_FindFreeImage (void)
{
	image_t		*image;
	int			i;

	// find a free image_t
	for (i=0, image=r_images ; i<numr_images ; i++,image++)
	{
		if (!image->registration_sequence)
			break;
	}
	if (i == numr_images)
	{
		if (numr_images == MAX_RIMAGES)
			ri.Sys_Error (ERR_DROP, "MAX_RIMAGES");
		numr_images++;
	}
	image = &r_images[i];

	return image;
}

/*
================
GL_LoadPic

================
*/
image_t *GL_LoadPic (char *name, byte *pic, int width, int height, imagetype_t type)
{
	image_t		*image;
	int			i, c;

	image = R_FindFreeImage ();
	if (strlen(name) >= sizeof(image->name))
		ri.Sys_Error (ERR_DROP, "Draw_LoadPic: \"%s\" is too long", name);
	strcpy (image->name, name);
	image->registration_sequence = registration_sequence;

	image->width = width;
	image->height = height;
	image->type = type;

	// ESP32: the image keeps the arena block the PCX was decoded into
	c = width*height;
	image->pixels[0] = pic;
	image->transparent = false;
	for (i=0 ; i<c ; i++)
	{
		if (pic[i] == 255)
		{
			image->transparent = true;
			break;
		}
	}
	r_image_bytes += Q_Arena_BlockSize (pic);

	return image;
}

/*
================
R_LoadWal
================
*/
// ESP32: reads the mip levels of a wall texture into the image (from mip
// level image->minmip), FALSE if there is no memory for them. The wall
// textures can be freed when memory is needed (R_PurgeImages) and are read
// again when used (R_ImagePixels).
static qboolean R_LoadWalPixels (image_t *image)
{
	miptex_t	mt;
	FILE		*h;
	int			base, size, mip = image->minmip;
	byte		*p;

	FS_FOpenFile (image->name, &h);
	if (!h)
		return false;
	base = ftell (h);
	FS_Read (&mt, sizeof(mt), h);
	size = image->width*image->height * (mip ? 64+16+4 : 256+64+16+4)/256;
	p = Q_Arena_AllocTop (size);
	if (!p)
	{
		fclose (h);
		return false;
	}
	FS_ReadAt (h, base, LittleLong (mt.offsets[mip]), p, size);
	fclose (h);
	if (mip)
	{
		image->pixels[0] = image->pixels[1] = p;
	}
	else
	{
		image->pixels[0] = p;
		image->pixels[1] = p + image->width*image->height;
	}
	image->pixels[2] = image->pixels[1] + image->width*image->height/4;
	image->pixels[3] = image->pixels[2] + image->width*image->height/16;
	r_image_bytes += Q_Arena_BlockSize (p);
	return true;
}

image_t *R_LoadWal (char *name)
{
	miptex_t	mt;
	FILE		*h;
	image_t		*image;

	FS_FOpenFile (name, &h);
	if (!h)
	{
		ri.Con_Printf (PRINT_ALL, "R_LoadWal: can't load %s\n", name);
		return r_notexture_mip;
	}
	FS_Read (&mt, sizeof(mt), h);
	fclose (h);

	image = R_FindFreeImage ();
	strcpy (image->name, name);
	image->width = LittleLong (mt.width);
	image->height = LittleLong (mt.height);
	image->type = it_wall;
	image->registration_sequence = registration_sequence;

	// ESP32: mip level 0 (3/4 of the data) is not loaded when sw_mipcap
	// keeps the renderer from using it
	image->minmip = r_wal_minmip;
	image->lastframe = r_framecount;
	R_LoadWalPixels (image);		// else when used
	return image;
}

// ESP32: pixels of a mip level of an image, read again if it was freed
byte *R_ImagePixels (image_t *image, int mip)
{
	image->lastframe = r_framecount;
	if (!image->pixels[image->minmip] && image->type == it_wall && !R_LoadWalPixels (image))
		ri.Sys_Error (ERR_DROP, "out of memory for %s", image->name);
	return image->pixels[mip];
}

// Read PCX RLE directly from a PAK entry using a bounded buffer, without
// allocating a second (compressed) copy of the image in PSRAM.
typedef struct {
	FILE *file;
	int remaining, pos, end;
	byte buf[512];
} skinstream_t;

static int R_SkinByte (skinstream_t *s)
{
	if (s->pos == s->end)
	{
		if (s->remaining <= 0) return -1;
		int n = s->remaining < (int)sizeof(s->buf) ? s->remaining : (int)sizeof(s->buf);
		s->end = (int)fread(s->buf, 1, n, s->file);
		s->remaining -= s->end;
		s->pos = 0;
		if (s->end <= 0) return -1;
	}
	return s->buf[s->pos++];
}

static unsigned skin_load_count, skin_evict_count, skin_eager_count;

byte *R_SkinPixels (image_t *image)
{
	if (!image) return NULL;
	if (image->type != it_skin) return image->pixels[0];
	image->lastframe = r_framecount;
	if (image->pixels[0]) return image->pixels[0];

	FILE *f = NULL;
	byte header[128];
	int len = FS_FOpenFile (image->name, &f);
	if (!f) return NULL;
	if (len < 128 || fread (header, 1, 128, f) != 128 ||
		header[0] != 0x0a || header[1] != 5 || header[2] != 1 || header[3] != 8 ||
		1 + (header[8] | header[9] << 8) != image->width ||
		1 + (header[10] | header[11] << 8) != image->height)
	{
		fclose (f);
		return NULL;
	}

	int size = image->width * image->height;
	byte *pic = Q_Arena_AllocTop (size);
	if (!pic)
	{
		fclose (f);
		return NULL; // keep descriptor for a later retry
	}
	memset (pic, 0, size);
	skinstream_t stream = {.file = f, .remaining = len - 128};
	for (int y = 0; y < image->height; y++)
	{
		for (int x = 0; x < image->width; )
		{
			int b = R_SkinByte (&stream), run = 1;
			if (b < 0) goto broken;
			if ((b & 0xc0) == 0xc0)
			{
				run = b & 0x3f;
				b = R_SkinByte (&stream);
				if (b < 0) goto broken;
			}
			// Match LoadPCX: an overlong run spills into the next row,
			// where the following row may overwrite the spilled bytes.
			for (int i = 0; i < run; i++, x++)
				if (y * image->width + x < size)
					pic[y * image->width + x] = (byte)b;
		}
	}
	fclose (f);
	image->pixels[0] = pic;
	image->transparent = false;
	for (int i = 0; i < size; i++)
		if (pic[i] == 255) { image->transparent = true; break; }
	r_image_bytes += Q_Arena_BlockSize (pic);
	skin_load_count++;
	return pic;

broken:
	fclose (f);
	Q_Arena_Free (pic);
	return NULL;
}

// Preload every referenced PCX skin only when ALL of them fit, retaining
// a 512 KiB margin for sound, temp files, and other runtime allocations.
// With tighter PSRAM the skins stay registered but are decoded on demand.
void R_PreloadSkinsIfRoom (void)
{
	unsigned long long total = 0;
	int missing = 0;
	for (int i = 0; i < numr_images; i++)
	{
		image_t *im = &r_images[i];
		if (im->registration_sequence != registration_sequence ||
			im->type != it_skin || im->pixels[0]) continue;
		total += ((((unsigned)im->width * (unsigned)im->height) + 15u) & ~15u) + 16u;
		missing++;
	}
	if (!missing) return;
	const int margin = 512 * 1024;
	int free_now = Q_Arena_FreeSize (), largest = Q_Arena_Largest ();
	if (total + margin > (unsigned)free_now || total + margin > (unsigned)largest)
	{
		ri.Con_Printf (PRINT_ALL, "PCX skins: %d deferred (%llu bytes), free %d largest %d\n",
			missing, total, free_now, largest);
		return;
	}
	int loaded = 0;
	for (int i = 0; i < numr_images; i++)
	{
		image_t *im = &r_images[i];
		if (im->registration_sequence == registration_sequence &&
			im->type == it_skin && !im->pixels[0] && R_SkinPixels (im))
			loaded++;
	}
	skin_eager_count += loaded;
	ri.Con_Printf (PRINT_ALL, "PCX skins: %d/%d preloaded, free %d\n",
		loaded, missing, Q_Arena_FreeSize ());
}

void R_SkinCacheStats (void)
{
	ri.Con_Printf (PRINT_ALL, "PCX cache: loads %u (preload %u), evictions %u\n",
		skin_load_count, skin_eager_count, skin_evict_count);
	skin_load_count = skin_evict_count = skin_eager_count = 0;
}

// Free inactive WAL or PCX skin pixel buffers on allocation pressure.
// Flash-resident skins are mapped in XIP and are never evicted.
void R_PurgeImages (void)
{
	int		i;
	image_t	*image;

	for (i=0, image=r_images ; i<numr_images ; i++, image++)
	{
		if ((image->type != it_wall && image->type != it_skin)
			|| !image->pixels[image->minmip]
			|| (!r_registering && image->lastframe >= r_framecount - 1))
			continue;
		// Unlike WAL, an MD2 skin can point directly into XIP Flash.
		// Never clear those pixels, and never free a buffer in use this frame.
		if (!Q_Arena_Contains (image->pixels[image->minmip]))
			continue;
		if (image->type == it_skin) skin_evict_count++;
		R_FreeImagePixels (image);
		image->pixels[0] = image->pixels[1] = image->pixels[2] = image->pixels[3] = NULL;
	}
}


/*
===============
R_FindImage

Finds or loads the given image
===============
*/
static image_t *R_FindImage_ (char *name, imagetype_t type)
{
	image_t	*image;
	int		i, len;
	byte	*pic, *palette;
	int		width, height;

	if (!name)
		return NULL;	// ri.Sys_Error (ERR_DROP, "R_FindImage: NULL name");
	len = strlen(name);
	if (len<5)
		return NULL;	// ri.Sys_Error (ERR_DROP, "R_FindImage: bad name: %s", name);

	// look for it
#ifdef PLATFORM_RP2350
	// RP2350: the HUD looks its pics up by name every frame; the slot the
	// name had last time is tried first (R_FindImage keeps the hints)
	{
		extern int R_ImageHint (const char *name);
		i = R_ImageHint (name);
		if (i >= 0 && i < numr_images && !strcmp (name, r_images[i].name))
		{
			image = &r_images[i];
			goto found;
		}
	}
#endif
	for (i=0, image=r_images ; i<numr_images ; i++,image++)
	{
		if (!strcmp(name, image->name))
		{
#ifdef PLATFORM_RP2350
found:
#endif
			if (!image->pixels[0] && image->type != it_wall && image->type != it_skin)
			{
				// was flushed by an early free: reclaim the slot and reload
				memset (image, 0, sizeof(*image));
				break;
			}
			if (image->type == it_wall && image->minmip > r_wal_minmip)
			{
				// ESP32: needed with mip level 0 now: reload it in place (the
				// texinfos already loaded keep pointing to this image)
				R_FreeImagePixels (image);
				image->pixels[0] = image->pixels[1] = image->pixels[2] = image->pixels[3] = NULL;
				image->minmip = r_wal_minmip;
				R_LoadWalPixels (image);		// else when used
			}
			image->registration_sequence = registration_sequence;
			return image;
		}
	}

	//
	// load the pic from disk
	//
	pic = NULL;
	palette = NULL;
	if (!strcmp(name+len-4, ".pcx") && QG_FindAsset (name, &width))
	{
		// ESP32: decoded skin in the flash asset partition, used in place
		const byte *a = QG_FindAsset (name, &width);

		image = R_FindFreeImage ();
		strcpy (image->name, name);
		image->registration_sequence = registration_sequence;
		image->width = a[0] | (a[1] << 8);
		image->height = a[2] | (a[3] << 8);
		image->transparent = a[4];
		image->type = type;
		image->pixels[0] = (byte *)a + 8;
		image->minmip = 0;
		return image;
	}
	else if (!strcmp(name+len-4, ".pcx"))
	{
		if (type == it_skin)
		{
			// The file may be in a custom PAK: validate and read only its
			// PCX header now. Decode pixels on the first actual draw.
			FILE *f = NULL;
			byte hdr[128];
			int n = FS_FOpenFile (name, &f);
			int valid = n >= (int)sizeof(hdr) && f &&
				fread (hdr, 1, sizeof(hdr), f) == sizeof(hdr);
			if (f) fclose (f);
			if (!valid || hdr[0] != 0x0a || hdr[1] != 5 ||
				hdr[2] != 1 || hdr[3] != 8)
				return NULL;
			width  = 1 + hdr[8]  + (hdr[9]  << 8);
			height = 1 + hdr[10] + (hdr[11] << 8);
			if (width < 1 || width > 640 || height < 1 || height > 480)
				return NULL;
			image = R_FindFreeImage ();
			memset (image, 0, sizeof(*image));
			strcpy (image->name, name);
			image->registration_sequence = registration_sequence;
			image->type = it_skin;
			image->width = width;
			image->height = height;
			image->lastframe = -2;
			return image;
		}
		r_image_permanent = type == it_pic;
		if (type == it_sky)
			pic = R_LoadSkyPCX (name, &width, &height);
		if (!pic)
			LoadPCX (name, &pic, &palette, &width, &height);
		r_image_permanent = true;
		if (!pic)
			return NULL;	// ri.Sys_Error (ERR_DROP, "R_FindImage: can't load %s", name);
		if (type == it_sky && width == 256 && height == 256)
		{
			// ESP32: skies are kept at half resolution (16 kB a side
			// instead of 64), drawn as mip level 1 (D_SkySurf)
			int		x, y;

			for (y=0 ; y<128 ; y++)
				for (x=0 ; x<128 ; x++)
					pic[y*128 + x] = pic[y*2*256 + x*2];
			width = height = 128;
			Q_Arena_Shrink (pic, width*height);
		}
		image = GL_LoadPic (name, pic, width, height, type);
	}
	else if (!strcmp(name+len-4, ".wal"))
	{
		image = R_LoadWal (name);
	}
	else if (!strcmp(name+len-4, ".tga"))
		return NULL;	// ri.Sys_Error (ERR_DROP, "R_FindImage: can't load %s in software renderer", name);
	else
		return NULL;	// ri.Sys_Error (ERR_DROP, "R_FindImage: bad extension on: %s", name);

	if (palette)
		free(palette);

	return image;
}



/*
===============
R_RegisterSkin
===============
*/
struct image_s *R_RegisterSkin (char *name)
{
	return R_FindImage (name, it_skin);
}


/*
================
R_FreeUnusedImages

Any image that was not touched on this registration sequence
will be freed.
================
*/
void R_FreeUnusedImages (void)
{
	int		i;
	image_t	*image;

	for (i=0, image=r_images ; i<numr_images ; i++, image++)
	{
		if (image->registration_sequence == registration_sequence)
		{
			if (image->pixels[0])
				Com_PageInMemory ((byte *)image->pixels[0], image->width*image->height);
			continue;		// used this sequence
		}
		if (!image->registration_sequence)
			continue;		// free texture
		if (image->type == it_pic)
			continue;		// don't free pics
		// free it
		R_FreeImagePixels (image);	// the other mip levels just follow
		memset (image, 0, sizeof(*image));
	}
}



/*
===============
R_InitImages
===============
*/
void	R_InitImages (void)
{
	registration_sequence = 1;
}

/*
===============
R_ShutdownImages
===============
*/
void	R_ShutdownImages (void)
{
	int		i;
	image_t	*image;

	for (i=0, image=r_images ; i<numr_images ; i++, image++)
	{
		if (!image->registration_sequence)
			continue;		// free texture
		// free it
		R_FreeImagePixels (image);	// the other mip levels just follow
		memset (image, 0, sizeof(*image));
	}
}

// ESP32: timed for the "perf" recorder
#ifdef PLATFORM_RP2350
static short	r_image_hint[256];		// slot + 1, by name hash

static unsigned R_ImageHash (const char *name)
{
	unsigned h = 0;

	while (*name)
		h = h * 31 + (unsigned char)*name++;
	return (h ^ h >> 8) & 255;
}

int R_ImageHint (const char *name)
{
	return r_image_hint[R_ImageHash (name)] - 1;
}
#endif

image_t	*R_FindImage (char *name, imagetype_t type)
{
	image_t	*image;
	int		opens = perf_active ? Perf_EventCount (PERF_EV_OPENS) : 0;

	Perf_Push (PERF_ASSET);
	image = R_FindImage_ (name, type);
	Perf_Pop ();
#ifdef PLATFORM_RP2350
	if (image && name)
		r_image_hint[R_ImageHash (name)] = (short)(image - r_images + 1);
#endif
	if (perf_active && Perf_EventCount (PERF_EV_OPENS) != opens)
		Perf_Count (PERF_EV_IMAGES, 1);
	return image;
}
