//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	All the clipping: columns, horizontal spans, sky columns.
//

// Russian Doom (C) 2016-2017 Julian Nechaevsky


#include <stdio.h>
#include <stdlib.h>

#include "i_system.h"
#include "doomdef.h"
#include "doomstat.h"
#include "r_local.h"
#include "r_sky.h"


// OPTIMIZE: closed two sided lines as single sided

// True if any of the segs textures might be visible.
boolean segtextured;	

// False if the back side is the same plane.
boolean markfloor;	
boolean markceiling;
boolean maskedtexture;

int toptexture;
int bottomtexture;
int midtexture;

angle_t rw_normalangle;
// angle to line origin
int rw_angle1;	

//
// regular wall
//
int rw_x;
int rw_stopx;

angle_t rw_centerangle;
fixed_t rw_offset;
fixed_t rw_distance;
fixed_t rw_scale;
fixed_t rw_scalestep;
fixed_t rw_midtexturemid;
fixed_t rw_toptexturemid;
fixed_t rw_bottomtexturemid;

int worldtop;
int worldbottom;
int worldhigh;
int worldlow;

int64_t pixhigh; // [crispy] WiggleFix
int64_t pixlow;  // [crispy] WiggleFix
fixed_t pixhighstep;
fixed_t pixlowstep;

int64_t topfrac; // [crispy] WiggleFix
fixed_t topstep;

int64_t bottomfrac; // [crispy] WiggleFix
fixed_t bottomstep;


lighttable_t** walllights;

int* maskedtexturecol; // [crispy] 32-bit integer math

// [crispy] WiggleFix: add this code block near the top of r_segs.c
//
// R_FixWiggle()
// Dynamic wall/texture rescaler, AKA "WiggleHack II"
//  by Kurt "kb1" Baumgardner ("kb") and Andrey "Entryway" Budko ("e6y")
//
//  [kb] When the rendered view is positioned, such that the viewer is
//   looking almost parallel down a wall, the result of the scale
//   calculation in R_ScaleFromGlobalAngle becomes very large. And, the
//   taller the wall, the larger that value becomes. If these large
//   values were used as-is, subsequent calculations would overflow,
//   causing full-screen HOM, and possible program crashes.
//
//  Therefore, vanilla Doom clamps this scale calculation, preventing it
//   from becoming larger than 0x400000 (64*FRACUNIT). This number was
//   chosen carefully, to allow reasonably-tight angles, with reasonably
//   tall sectors to be rendered, within the limits of the fixed-point
//   math system being used. When the scale gets clamped, Doom cannot
//   properly render the wall, causing an undesirable wall-bending
//   effect that I call "floor wiggle". Not a crash, but still ugly.
//
//  Modern source ports offer higher video resolutions, which worsens
//   the issue. And, Doom is simply not adjusted for the taller walls
//   found in many PWADs.
//
//  This code attempts to correct these issues, by dynamically
//   adjusting the fixed-point math, and the maximum scale clamp,
//   on a wall-by-wall basis. This has 2 effects:
//
//  1. Floor wiggle is greatly reduced and/or eliminated.
//  2. Overflow is no longer possible, even in levels with maximum
//     height sectors (65535 is the theoretical height, though Doom
//     cannot handle sectors > 32767 units in height.
//
//  The code is not perfect across all situations. Some floor wiggle can
//   still be seen, and some texture strips may be slightly misaligned in
//   extreme cases. These effects cannot be corrected further, without
//   increasing the precision of various renderer variables, and,
//   possibly, creating a noticable performance penalty.
//

static int max_rwscale = 64 * FRACUNIT;
static int heightbits = 12;
static int heightunit = (1 << 12);
static int invhgtbits = 4;

static const struct
{
    int clamp;
    int heightbits;
} scale_values[8] = {
    {2048 * FRACUNIT, 12},
    {1024 * FRACUNIT, 12},
    {1024 * FRACUNIT, 11},
    { 512 * FRACUNIT, 11},
    { 512 * FRACUNIT, 10},
    { 256 * FRACUNIT, 10},
    { 256 * FRACUNIT,  9},
    { 128 * FRACUNIT,  9}
};

void R_FixWiggle (sector_t *sector)
{
    static int	lastheight = 0;
    int height = (sector->ceilingheight - sector->floorheight) >> FRACBITS;

    // disallow negative heights. using 1 forces cache initialization
    if (height < 1)
    height = 1;

    // early out?
    if (height != lastheight)
    {
        lastheight = height;

        // initialize, or handle moving sector
        if (height != sector->cachedheight)
        {
            sector->cachedheight = height;
            sector->scaleindex = 0;
            height >>= 7;

            // calculate adjustment
            while (height >>= 1)
            sector->scaleindex++;
        }

        // fine-tune renderer for this wall
        max_rwscale = scale_values[sector->scaleindex].clamp;
        heightbits = scale_values[sector->scaleindex].heightbits;
        heightunit = (1 << heightbits);
        invhgtbits = FRACBITS - heightbits;
    }
}
 
 
//
// R_RenderMaskedSegRange
//
void
R_RenderMaskedSegRange (drawseg_t* ds, int x1, int x2)
{
    unsigned    index;
    column_t*   col;
    int         lightnum;
    int         texnum;

    // Calculate light table.
    // Use different light tables
    //   for horizontal / vertical / diagonal. Diagonal?
    // OPTIMIZE: get rid of LIGHTSEGSHIFT globally
    curline = ds->curline;
    frontsector = curline->frontsector;
    backsector = curline->backsector;
    texnum = texturetranslation[curline->sidedef->midtexture];

    lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

    if (curline->v1->y == curline->v2->y)
    lightnum--;
    else if (curline->v1->x == curline->v2->x)
    lightnum++;

    if (lightnum < 0)		
    walllights = scalelight[0];
    else if (lightnum >= LIGHTLEVELS)
    walllights = scalelight[LIGHTLEVELS-1];
    else
    walllights = scalelight[lightnum];

    maskedtexturecol = ds->maskedtexturecol;

    rw_scalestep = ds->scalestep;		
    spryscale = ds->scale1 + (x1 - ds->x1)*rw_scalestep;
    mfloorclip = ds->sprbottomclip;
    mceilingclip = ds->sprtopclip;

    // find positioning
    if (curline->linedef->flags & ML_DONTPEGBOTTOM)
    {
        dc_texturemid = frontsector->floorheight > backsector->floorheight ? frontsector->floorheight : backsector->floorheight;
        dc_texturemid = dc_texturemid + textureheight[texnum] - viewz;
    }
    else
    {
        dc_texturemid =frontsector->ceilingheight<backsector->ceilingheight ? frontsector->ceilingheight : backsector->ceilingheight;
        dc_texturemid = dc_texturemid - viewz;
    }
    dc_texturemid += curline->sidedef->rowoffset;

    if (fixedcolormap)
    dc_colormap = fixedcolormap;

    // draw the columns
    for (dc_x = x1 ; dc_x <= x2 ; dc_x++)
    {
        // calculate lighting
        if (maskedtexturecol[dc_x] != INT_MAX) // [crispy] 32-bit integer math
        {
            if (!fixedcolormap)
            {
                index = spryscale>>(LIGHTSCALESHIFT - (detailshift && hires) + hires);

                if (index >= MAXLIGHTSCALE)
                index = MAXLIGHTSCALE-1;

                dc_colormap = walllights[index];
            }

            // [crispy] apply Killough's int64 sprtopscreen overflow fix
            // from winmbf/Source/r_segs.c:174-191
            // killough 3/2/98:
            //
            // This calculation used to overflow and cause crashes in Doom:
            //
            // sprtopscreen = centeryfrac - FixedMul(dc_texturemid, spryscale);
            //
            // This code fixes it, by using double-precision intermediate
            // arithmetic and by skipping the drawing of 2s normals whose
            // mapping to screen coordinates is totally out of range:

            {
                int64_t t = ((int64_t) centeryfrac << FRACBITS) - (int64_t) dc_texturemid * spryscale;

                if (t + (int64_t) textureheight[texnum] * spryscale < 0 || t > (int64_t) SCREENHEIGHT << FRACBITS*2)
                {
                    spryscale += rw_scalestep; // [crispy] MBF had this in the for-loop iterator
                    continue; // skip if the texture is out of screen's range
                }
                sprtopscreen = (int64_t)(t >> FRACBITS); // [crispy] WiggleFix
            }

            dc_iscale = 0xffffffffu / (unsigned)spryscale;

            // draw the texture
            col = (column_t *)( 
            (byte *)R_GetColumn(texnum,maskedtexturecol[dc_x], false) -3);

            R_DrawMaskedColumn (col);
            maskedtexturecol[dc_x] = INT_MAX; // [crispy] 32-bit integer math
        }

        spryscale += rw_scalestep;
    }
}


//
// R_RenderSegLoop
// Draws zero, one, or two textures (and possibly a masked
//  texture) for walls.
// Can draw or mark the starting pixel of floor and ceiling
//  textures.
// CALLED: CORE LOOPING ROUTINE.
//
#define HEIGHTBITS  12
#define HEIGHTUNIT  (1<<HEIGHTBITS)

void R_RenderSegLoop (void)
{
    angle_t     angle;
    unsigned    index;
    int         yl;
    int         yh;
    int         mid;
    fixed_t     texturecolumn;
    int         top;
    int         bottom;

    for ( ; rw_x < rw_stopx ; rw_x++)
    {
        // mark floor / ceiling areas
        yl = (int)((topfrac+heightunit-1)>>heightbits); // [crispy] WiggleFix

        // no space above wall?
        if (yl < ceilingclip[rw_x]+1)
            yl = ceilingclip[rw_x]+1;

        if (markceiling)
        {
            top = ceilingclip[rw_x]+1;
            bottom = yl-1;

            if (bottom >= floorclip[rw_x])
            bottom = floorclip[rw_x]-1;

            if (top <= bottom)
            {
                ceilingplane->top[rw_x] = top;
                ceilingplane->bottom[rw_x] = bottom;
            }
        }

        yh = (int)(bottomfrac>>heightbits); // [crispy] WiggleFix

        if (yh >= floorclip[rw_x])
            yh = floorclip[rw_x]-1;

        if (markfloor)
        {
            top = yh+1;
            bottom = floorclip[rw_x]-1;

            if (top <= ceilingclip[rw_x])
            top = ceilingclip[rw_x]+1;

            if (top <= bottom)
            {
                floorplane->top[rw_x] = top;
                floorplane->bottom[rw_x] = bottom;
            }
        }

        // texturecolumn and lighting are independent of wall tiers
        if (segtextured)
        {
            // calculate texture offset
            angle = (rw_centerangle + xtoviewangle[rw_x])>>ANGLETOFINESHIFT;
            texturecolumn = rw_offset-FixedMul(finetangent[angle],rw_distance);
            texturecolumn >>= FRACBITS;
            // calculate lighting
            index = rw_scale>>(LIGHTSCALESHIFT - (detailshift && hires) + hires);

            if (index >=  MAXLIGHTSCALE )
            index = MAXLIGHTSCALE-1;

            dc_colormap = walllights[index];
            dc_x = rw_x;
            dc_iscale = 0xffffffffu / (unsigned)rw_scale;
        }
        else
        {
            // purely to shut up the compiler
            texturecolumn = 0;
        }

        // draw the wall tiers
        if (midtexture)
        {
            // single sided line
            dc_yl = yl;
            dc_yh = yh;
            dc_texturemid = rw_midtexturemid;
            dc_source = R_GetColumn(midtexture,texturecolumn,true);
            dc_texheight = textureheight[midtexture]>>FRACBITS;
            colfunc ();
            ceilingclip[rw_x] = viewheight;
            floorclip[rw_x] = -1;
        }
        else
        {
            // two sided line
            if (toptexture)
            {
                // top wall
                mid = (int)(pixhigh>>heightbits); // [crispy] WiggleFix

                pixhigh += pixhighstep;

                if (mid >= floorclip[rw_x])
                    mid = floorclip[rw_x]-1;

                if (mid >= yl)
                {
                    dc_yl = yl;
                    dc_yh = mid;
                    dc_texturemid = rw_toptexturemid;
                    dc_source = R_GetColumn(toptexture,texturecolumn,true);
                    dc_texheight = textureheight[toptexture]>>FRACBITS;
                    colfunc ();
                    ceilingclip[rw_x] = mid;
                }
                else
                {
                    ceilingclip[rw_x] = yl-1;
                }
            }
            else
            {
                // no top wall
                if (markceiling)
                ceilingclip[rw_x] = yl-1;
            }

            if (bottomtexture)
            {
                // bottom wall
                mid = (int)((pixlow+heightunit-1)>>heightbits); // [crispy] WiggleFix

                pixlow += pixlowstep;

                // no space above wall?
                if (mid <= ceilingclip[rw_x])
                    mid = ceilingclip[rw_x]+1;

                if (mid <= yh)
                {
                    dc_yl = mid;
                    dc_yh = yh;
                    dc_texturemid = rw_bottomtexturemid;
                    dc_source = R_GetColumn(bottomtexture,texturecolumn,true);
                    dc_texheight = textureheight[bottomtexture]>>FRACBITS;
                    colfunc ();
                    floorclip[rw_x] = mid;
                }
                else
                {
                    floorclip[rw_x] = yh+1;
                }
            }
            else
            {
                // no bottom wall
                if (markfloor)
                    floorclip[rw_x] = yh+1;
            }

            if (maskedtexture)
            {
                // save texturecol
                //  for backdrawing of masked mid texture
                maskedtexturecol[rw_x] = texturecolumn;
            }
        }

    rw_scale += rw_scalestep;
    topfrac += topstep;
    bottomfrac += bottomstep;
    }
}


// [crispy] WiggleFix: move R_ScaleFromGlobalAngle function to r_segs.c,
// above R_StoreWallRange
fixed_t R_ScaleFromGlobalAngle (angle_t visangle)
{
    int     anglea = ANG90 + (visangle - viewangle);
    int     angleb = ANG90 + (visangle - rw_normalangle);
    int     den = FixedMul(rw_distance, finesine[anglea >> ANGLETOFINESHIFT]);
    fixed_t	num = FixedMul(projection, finesine[angleb >> ANGLETOFINESHIFT]);
    fixed_t scale;

    if (den > (num >> 16))
    {
        scale = FixedDiv(num, den);

        // [kb] When this evaluates True, the scale is clamped,
        //  and there will be some wiggling.
        if (scale > max_rwscale)
            scale = max_rwscale;
        else if (scale < 256)
            scale = 256;
    }
    else
    {
        scale = max_rwscale;
    }

    return scale;
}


//
// R_StoreWallRange
// A wall segment will be drawn
//  between start and stop pixels (inclusive).
//
void R_StoreWallRange (int start, int stop)
{
    angle_t     offsetangle;
    fixed_t     vtop;
    int         lightnum;
    int64_t     dx, dy, dx1, dy1; // [crispy] fix long wall wobble

    // [crispy] remove MAXDRAWSEGS Vanilla limit
    if (ds_p == &drawsegs[numdrawsegs])
    {
        int numdrawsegs_old = numdrawsegs;

        if (numdrawsegs_old == MAXDRAWSEGS)
        printf("R_StoreWallRange: Hit MAXDRAWSEGS (%d) Vanilla limit.\n", MAXDRAWSEGS);

        numdrawsegs = numdrawsegs ? 2 * numdrawsegs : MAXDRAWSEGS;
        drawsegs = realloc(drawsegs, numdrawsegs * sizeof(*drawsegs));
        ds_p = drawsegs + numdrawsegs_old;
    }

#ifdef RANGECHECK
    if (start >=viewwidth || start > stop)
    I_Error ("Bad R_RenderWallRange: %i к %i", start , stop);
#endif

    sidedef = curline->sidedef;
    linedef = curline->linedef;

    // mark the segment as visible for auto map
    linedef->flags |= ML_MAPPED;

    // [crispy] (flags & ML_MAPPED) is all we need to know for automap
    if (automapactive)
    return;

    // calculate rw_distance for scale calculation
    rw_normalangle = curline->angle + ANG90;
    offsetangle = abs(rw_normalangle-rw_angle1);

    if (offsetangle > ANG90)
    offsetangle = ANG90;

    // [crispy] fix long wall wobble
    // thank you very much Linguica, e6y and kb1
    // http://www.doomworld.com/vb/post/1340718
    dx = curline->v2->px - curline->v1->px;
    dy = curline->v2->py - curline->v1->py;
    dx1 = viewx - curline->v1->px;
    dy1 = viewy - curline->v1->py;
    rw_distance = (fixed_t)((dy * dx1 - dx * dy1) / curline->length);

    ds_p->x1 = rw_x = start;
    ds_p->x2 = stop;
    ds_p->curline = curline;
    rw_stopx = stop+1;

    // [crispy] WiggleFix: add this line, in r_segs.c:R_StoreWallRange,
    // right before calls to R_ScaleFromGlobalAngle:
    R_FixWiggle(frontsector);

    // calculate scale at both ends and step
    ds_p->scale1 = rw_scale = 
    R_ScaleFromGlobalAngle (viewangle + xtoviewangle[start]);

    if (stop > start )
    {
        ds_p->scale2 = R_ScaleFromGlobalAngle (viewangle + xtoviewangle[stop]);
        ds_p->scalestep = rw_scalestep = (ds_p->scale2 - rw_scale) / (stop-start);
    }
    else
    {
    // UNUSED: try to fix the stretched line bug
#if 0
    if (rw_distance < FRACUNIT/2)
    {
        fixed_t trx,try;
        fixed_t gxt,gyt;

        trx = curline->v1->x - viewx;
        try = curline->v1->y - viewy;

        gxt = FixedMul(trx,viewcos); 
        gyt = -FixedMul(try,viewsin); 
        ds_p->scale1 = FixedDiv(projection, gxt-gyt)<<detailshift;
    }
#endif
    ds_p->scale2 = ds_p->scale1;
    }

    // calculate texture boundaries
    //  and decide if floor / ceiling marks are needed
    worldtop = frontsector->ceilingheight - viewz;
    worldbottom = frontsector->floorheight - viewz;

    midtexture = toptexture = bottomtexture = maskedtexture = 0;
    ds_p->maskedtexturecol = NULL;

    if (!backsector)
    {
        // single sided line
        midtexture = texturetranslation[sidedef->midtexture];
        // a single sided line is terminal, so it must mark ends
        markfloor = markceiling = true;
        if (linedef->flags & ML_DONTPEGBOTTOM)
        {
            vtop = frontsector->floorheight +
            textureheight[sidedef->midtexture];
            // bottom of texture at bottom
            rw_midtexturemid = vtop - viewz;	
        }
        else
        {
            // top of texture at top
            rw_midtexturemid = worldtop;
        }
        rw_midtexturemid += sidedef->rowoffset;

        ds_p->silhouette = SIL_BOTH;
        ds_p->sprtopclip = screenheightarray;
        ds_p->sprbottomclip = negonearray;
        ds_p->bsilheight = INT_MAX;
        ds_p->tsilheight = INT_MIN;
    }
    else
    {
        // two sided line
        ds_p->sprtopclip = ds_p->sprbottomclip = NULL;
        ds_p->silhouette = 0;

        if (frontsector->floorheight > backsector->floorheight)
        {
            ds_p->silhouette = SIL_BOTTOM;
            ds_p->bsilheight = frontsector->floorheight;
        }
        else if (backsector->floorheight > viewz)
        {
            ds_p->silhouette = SIL_BOTTOM;
            ds_p->bsilheight = INT_MAX;
            // ds_p->sprbottomclip = negonearray;
        }

        if (frontsector->ceilingheight < backsector->ceilingheight)
        {
            ds_p->silhouette |= SIL_TOP;
            ds_p->tsilheight = frontsector->ceilingheight;
        }
        else if (backsector->ceilingheight < viewz)
        {
            ds_p->silhouette |= SIL_TOP;
            ds_p->tsilheight = INT_MIN;
            // ds_p->sprtopclip = screenheightarray;
        }

        if (backsector->ceilingheight <= frontsector->floorheight)
        {
            ds_p->sprbottomclip = negonearray;
            ds_p->bsilheight = INT_MAX;
            ds_p->silhouette |= SIL_BOTTOM;
        }

        if (backsector->floorheight >= frontsector->ceilingheight)
        {
            ds_p->sprtopclip = screenheightarray;
            ds_p->tsilheight = INT_MIN;
            ds_p->silhouette |= SIL_TOP;
        }

        worldhigh = backsector->ceilingheight - viewz;
        worldlow = backsector->floorheight - viewz;

        // hack to allow height changes in outdoor areas
        if (frontsector->ceilingpic == skyflatnum && backsector->ceilingpic == skyflatnum)
        {
            worldtop = worldhigh;
        }

        if (worldlow != worldbottom 
            || backsector->floorpic != frontsector->floorpic
            || backsector->lightlevel != frontsector->lightlevel)
        {
            markfloor = true;
        }
        else
        {
            // same plane on both sides
            markfloor = false;
        }

        if (worldhigh != worldtop 
            || backsector->ceilingpic != frontsector->ceilingpic
            || backsector->lightlevel != frontsector->lightlevel)
        {
            markceiling = true;
        }
        else
        {
            // same plane on both sides
            markceiling = false;
        }

        if (backsector->ceilingheight <= frontsector->floorheight
            || backsector->floorheight >= frontsector->ceilingheight)
        {
            // closed door
            markceiling = markfloor = true;
        }

        if (worldhigh < worldtop)
        {
            // top texture
            toptexture = texturetranslation[sidedef->toptexture];
            if (linedef->flags & ML_DONTPEGTOP)
            {
                // top of texture at top
                rw_toptexturemid = worldtop;
            }
            else
            {
                vtop = backsector->ceilingheight + textureheight[sidedef->toptexture];

                // bottom of texture
                rw_toptexturemid = vtop - viewz;	
            }
        }
        if (worldlow > worldbottom)
        {
            // bottom texture
            bottomtexture = texturetranslation[sidedef->bottomtexture];

            if (linedef->flags & ML_DONTPEGBOTTOM )
            {
                // bottom of texture at bottom
                // top of texture at top
                rw_bottomtexturemid = worldtop;
            }
            else    // top of texture at top
            rw_bottomtexturemid = worldlow;
        }

        rw_toptexturemid += sidedef->rowoffset;
        rw_bottomtexturemid += sidedef->rowoffset;

        // allocate space for masked texture tables
        if (sidedef->midtexture)
        {
            // masked midtexture
            maskedtexture = true;
            ds_p->maskedtexturecol = maskedtexturecol = lastopening - rw_x;
            lastopening += rw_stopx - rw_x;
        }
    }

    // calculate rw_offset (only needed for textured lines)
    segtextured = midtexture | toptexture | bottomtexture | maskedtexture;

    if (segtextured)
    {
        offsetangle = rw_normalangle-rw_angle1;

    if (offsetangle > ANG180)
        offsetangle = -offsetangle;

    if (offsetangle > ANG90)
        offsetangle = ANG90;

    // [crispy] fix long wall wobble
    rw_offset = (fixed_t)((dx*dx1 + dy*dy1) / curline->length);

    rw_offset += sidedef->textureoffset + curline->offset;
    rw_centerangle = ANG90 + viewangle - rw_normalangle;

    // calculate light table
    //  use different light tables
    //  for horizontal / vertical / diagonal
    // OPTIMIZE: get rid of LIGHTSEGSHIFT globally
    if (!fixedcolormap)
    {
        lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

        if (curline->v1->y == curline->v2->y)
        lightnum--;
        else if (curline->v1->x == curline->v2->x)
        lightnum++;

        if (lightnum < 0)
        {
            // [JN] Standard light table
            walllights = scalelight[0];

            // [JN] Applying brightmaps to walls...
            if (!vanillaparm && gamevariant != freedoom && gamevariant != freedm)
            {
                // Doom 1
                if (gamemission == doom)
                {
                    if (segtextured == R_TextureNumForName("LITEBLU2"))
                    {
                        walllights = fullbright_notgray[0];
                    }
                    else if (segtextured == R_TextureNumForName("SW2STON2"))
                    {
                        walllights = fullbright_redonly[0];
                    }
                }

                // Doom 2 only
                if (gamemission == doom2 || gamemission == pack_tnt || gamemission == pack_plut)
                {
                    if (segtextured == R_TextureNumForName("SW1BRN1")
                    || segtextured == R_TextureNumForName("SW1STARG")
                    || segtextured == R_TextureNumForName("SW1STON2")
                    || segtextured == R_TextureNumForName("SW2MARB"))
                    {
                        walllights = fullbright_redonly[0];
                    }
                    else if (segtextured == R_TextureNumForName("SW2STON2")
                    || segtextured == R_TextureNumForName("SW2STARG"))
                    {
                        walllights = fullbright_greenonly1[0];
                    }
                }

                // Doom 1 & 2
                if (gamemission == doom || gamemission == doom2 || gamemission == pack_tnt || gamemission == pack_plut)
                {
                    if (segtextured == R_TextureNumForName("SW1BRCOM")
                    || segtextured == R_TextureNumForName("SW1DIRT")
                    || segtextured == R_TextureNumForName("SW1STRTN")
                    || segtextured == R_TextureNumForName("SW2COMP")
                    || segtextured == R_TextureNumForName("SW2PANEL")
                    || segtextured == R_TextureNumForName("SW2SLAD")
                    || segtextured == R_TextureNumForName("SW2WOOD")
                    || segtextured == R_TextureNumForName("WOOD4")
                    || segtextured == R_TextureNumForName("WOODSKUL")
                    || segtextured == R_TextureNumForName("SLADSKUL")
                    || segtextured == R_TextureNumForName("SW1BRIK")
                    || segtextured == R_TextureNumForName("SW1COMM")
                    || segtextured == R_TextureNumForName("SW1MET2")
                    || segtextured == R_TextureNumForName("SW1STON1")
                    || segtextured == R_TextureNumForName("SW1STONE")
                    || segtextured == R_TextureNumForName("SW2BLUE")
                    || segtextured == R_TextureNumForName("SW2GSTON")
                    || segtextured == R_TextureNumForName("SW2ROCK")
                    || segtextured == R_TextureNumForName("SW2STON6")
                    || segtextured == R_TextureNumForName("SW2ZIM")
                    || segtextured == R_TextureNumForName("WOODGARG"))
                    {
                        walllights = fullbright_redonly[0];
                    }
                    else if (segtextured == R_TextureNumForName("COMP2")
                    || segtextured == R_TextureNumForName("COMPUTE2")
                    || segtextured == R_TextureNumForName("SILVER3")
                    || segtextured == R_TextureNumForName("COMPUTE1")
                    || segtextured == R_TextureNumForName("COMPUTE3")
                    || segtextured == R_TextureNumForName("SW2MOD1"))
                    {
                        walllights = fullbright_notgrayorbrown[0];
                    }
                    else if (segtextured == R_TextureNumForName("COMPSTA2")
                    || segtextured == R_TextureNumForName("EXITSIGN")
                    || segtextured == R_TextureNumForName("PLANET1")
                    || segtextured == R_TextureNumForName("SW2EXIT")
                    || segtextured == R_TextureNumForName("SW2GRAY1")
                    || segtextured == R_TextureNumForName("COMPSTA1")
                    || segtextured == R_TextureNumForName("EXITSTON")
                    || segtextured == R_TextureNumForName("SILVER2")
                    || segtextured == R_TextureNumForName("LITEBLU1")
                    || segtextured == R_TextureNumForName("SW2GRAY"))
                    {
                        walllights = fullbright_notgray[0];
                    }
                    else if (segtextured == R_TextureNumForName("SW2BRN1")
                    || segtextured == R_TextureNumForName("SW2STON1")
                    || segtextured == R_TextureNumForName("SW2STONE")
                    || segtextured == R_TextureNumForName("SW2TEK")
                    || segtextured == R_TextureNumForName("SW2BRIK")
                    || segtextured == R_TextureNumForName("SW2BRN2")
                    || segtextured == R_TextureNumForName("SW2COMM")
                    || segtextured == R_TextureNumForName("SW2DIRT")
                    || segtextured == R_TextureNumForName("SW2MET2")
                    || segtextured == R_TextureNumForName("SW2STRTN")
                    || segtextured == R_TextureNumForName("SW2VINE")
                    || segtextured == R_TextureNumForName("PIPEWAL1")
                    || segtextured == R_TextureNumForName("TEKLITE2"))
                    {
                        walllights = fullbright_greenonly1[0];
                    }
                    else if (segtextured == R_TextureNumForName("SW2BRCOM")
                    || segtextured == R_TextureNumForName("SW2BRNGN"))
                    {
                        walllights = fullbright_greenonly2[0];
                    }
                    else if (segtextured == R_TextureNumForName("SW2METAL"))
                    {
                        walllights = fullbright_greenonly3[0];
                    }
                }

                // TNT - Evilution
                if (gamemission == pack_tnt)
                {
                    if (segtextured == R_TextureNumForName("LITEYEL2")
                    || segtextured == R_TextureNumForName("LITEYEL3")
                    || segtextured == R_TextureNumForName("YELMETAL"))
                    {
                        walllights = fullbright_orangeyellow[0];
                    }
                    else if (segtextured == R_TextureNumForName("BTNTMETL")
                    || segtextured == R_TextureNumForName("BTNTSLVR"))
                    {
                        walllights = fullbright_notgrayorbrown[0];
                    }
                    else if (segtextured == R_TextureNumForName("PNK4EXIT")
                    || segtextured == R_TextureNumForName("LITERED2"))
                    {
                        walllights = fullbright_redonly[0];
                    }
                }

                // Plutonia
                if (gamemission == pack_plut)
                {
                    if (segtextured == R_TextureNumForName("SW2SKULL"))
                    {
                        walllights = fullbright_redonly[0];
                    }
                }
            }
        }

        // --------------------------------------------------------------------------------

        else if (lightnum >= LIGHTLEVELS)
        {
            // [JN] Standard light table
            walllights = scalelight[LIGHTLEVELS-1];

            // [JN] Applying brightmaps to walls...
            if (!vanillaparm && gamevariant != freedoom && gamevariant != freedm)
            {
                // Doom 1
                if (gamemission == doom)
                {
                    if (segtextured == R_TextureNumForName("LITEBLU2"))
                    {
                        walllights = fullbright_notgray[LIGHTLEVELS-1];
                    }
                    else if (segtextured == R_TextureNumForName("SW2STON2"))
                    {
                        walllights = fullbright_redonly[LIGHTLEVELS-1];
                    }
                }

                // Doom 2 only
                if (gamemission == doom2 || gamemission == pack_tnt || gamemission == pack_plut)
                {
                    if (segtextured == R_TextureNumForName("SW1BRN1")
                    || segtextured == R_TextureNumForName("SW1STARG")
                    || segtextured == R_TextureNumForName("SW1STON2")
                    || segtextured == R_TextureNumForName("SW2MARB"))
                    {
                        walllights = fullbright_redonly[LIGHTLEVELS-1];
                    }
                    else if (segtextured == R_TextureNumForName("SW2STON2")
                    || segtextured == R_TextureNumForName("SW2STARG"))
                    {
                        walllights = fullbright_greenonly1[LIGHTLEVELS-1];
                    }
                }

                // Doom 1 & 2
                if (gamemission == doom || gamemission == doom2 || gamemission == pack_tnt || gamemission == pack_plut)
                {
                    if (segtextured == R_TextureNumForName("SW1BRCOM")
                    || segtextured == R_TextureNumForName("SW1DIRT")
                    || segtextured == R_TextureNumForName("SW1STRTN")
                    || segtextured == R_TextureNumForName("SW2COMP")
                    || segtextured == R_TextureNumForName("SW2PANEL")
                    || segtextured == R_TextureNumForName("SW2SLAD")
                    || segtextured == R_TextureNumForName("SW2WOOD")
                    || segtextured == R_TextureNumForName("WOOD4")
                    || segtextured == R_TextureNumForName("WOODSKUL")
                    || segtextured == R_TextureNumForName("SLADSKUL")
                    || segtextured == R_TextureNumForName("SW1BRIK")
                    || segtextured == R_TextureNumForName("SW1COMM")
                    || segtextured == R_TextureNumForName("SW1MET2")
                    || segtextured == R_TextureNumForName("SW1STON1")
                    || segtextured == R_TextureNumForName("SW1STONE")
                    || segtextured == R_TextureNumForName("SW2BLUE")
                    || segtextured == R_TextureNumForName("SW2GSTON")
                    || segtextured == R_TextureNumForName("SW2ROCK")
                    || segtextured == R_TextureNumForName("SW2STON6")
                    || segtextured == R_TextureNumForName("SW2ZIM")
                    || segtextured == R_TextureNumForName("WOODGARG"))
                    {
                        walllights = fullbright_redonly[LIGHTLEVELS-1];
                    }
                    else if (segtextured == R_TextureNumForName("COMP2")
                    || segtextured == R_TextureNumForName("COMPUTE2")
                    || segtextured == R_TextureNumForName("SILVER3")
                    || segtextured == R_TextureNumForName("COMPUTE1")
                    || segtextured == R_TextureNumForName("COMPUTE3")
                    || segtextured == R_TextureNumForName("SW2MOD1"))
                    {
                        walllights = fullbright_notgrayorbrown[LIGHTLEVELS-1];
                    }
                    else if (segtextured == R_TextureNumForName("COMPSTA2")
                    || segtextured == R_TextureNumForName("EXITSIGN")
                    || segtextured == R_TextureNumForName("PLANET1")
                    || segtextured == R_TextureNumForName("SW2EXIT")
                    || segtextured == R_TextureNumForName("SW2GRAY1")
                    || segtextured == R_TextureNumForName("COMPSTA1")
                    || segtextured == R_TextureNumForName("EXITSTON")
                    || segtextured == R_TextureNumForName("SILVER2")
                    || segtextured == R_TextureNumForName("LITEBLU1")
                    || segtextured == R_TextureNumForName("SW2GRAY"))
                    {
                        walllights = fullbright_notgray[LIGHTLEVELS-1];
                    }
                    else if (segtextured == R_TextureNumForName("SW2BRN1")
                    || segtextured == R_TextureNumForName("SW2STON1")
                    || segtextured == R_TextureNumForName("SW2STONE")
                    || segtextured == R_TextureNumForName("SW2TEK")
                    || segtextured == R_TextureNumForName("SW2BRIK")
                    || segtextured == R_TextureNumForName("SW2BRN2")
                    || segtextured == R_TextureNumForName("SW2COMM")
                    || segtextured == R_TextureNumForName("SW2DIRT")
                    || segtextured == R_TextureNumForName("SW2MET2")
                    || segtextured == R_TextureNumForName("SW2STRTN")
                    || segtextured == R_TextureNumForName("SW2VINE")
                    || segtextured == R_TextureNumForName("PIPEWAL1")
                    || segtextured == R_TextureNumForName("TEKLITE2"))
                    {
                        walllights = fullbright_greenonly1[LIGHTLEVELS-1];
                    }
                    else if (segtextured == R_TextureNumForName("SW2BRCOM")
                    || segtextured == R_TextureNumForName("SW2BRNGN"))
                    {
                        walllights = fullbright_greenonly2[LIGHTLEVELS-1];
                    }
                    else if (segtextured == R_TextureNumForName("SW2METAL"))
                    {
                        walllights = fullbright_greenonly3[LIGHTLEVELS-1];
                    }
                }

                // TNT - Evilution
                if (gamemission == pack_tnt)
                {
                    if (segtextured == R_TextureNumForName("LITEYEL2")
                    || segtextured == R_TextureNumForName("LITEYEL3")
                    || segtextured == R_TextureNumForName("YELMETAL"))
                    {
                        walllights = fullbright_orangeyellow[LIGHTLEVELS-1];
                    }
                    else if (segtextured == R_TextureNumForName("BTNTMETL")
                    || segtextured == R_TextureNumForName("BTNTSLVR"))
                    {
                        walllights = fullbright_notgrayorbrown[LIGHTLEVELS-1];
                    }
                    else if (segtextured == R_TextureNumForName("PNK4EXIT")
                    || segtextured == R_TextureNumForName("LITERED2"))
                    {
                        walllights = fullbright_redonly[LIGHTLEVELS-1];
                    }
                }

                // Plutonia
                if (gamemission == pack_plut)
                {
                    if (segtextured == R_TextureNumForName("SW2SKULL"))
                    {
                        walllights = fullbright_redonly[LIGHTLEVELS-1];
                    }
                }
            }
        }

        // --------------------------------------------------------------------------------

        else
        {
            // [JN] Standard light table
            walllights = scalelight[lightnum];

            // [JN] Applying brightmaps to walls...
            if (!vanillaparm && gamevariant != freedoom && gamevariant != freedm)
            {
                // Doom 1 only
                if (gamemission == doom)
                {
                    if (segtextured == R_TextureNumForName("LITEBLU2"))
                    {
                        walllights = fullbright_notgray[lightnum];
                    }
                    else if (segtextured == R_TextureNumForName("SW2STON2"))
                    {
                        walllights = fullbright_redonly[lightnum];
                    }
                }

                // Doom 2 only
                if (gamemission == doom2 || gamemission == pack_tnt || gamemission == pack_plut)
                {
                    if (segtextured == R_TextureNumForName("SW1BRN1")
                    || segtextured == R_TextureNumForName("SW1STARG")
                    || segtextured == R_TextureNumForName("SW1STON2")
                    || segtextured == R_TextureNumForName("SW2MARB"))
                    {
                        walllights = fullbright_redonly[lightnum];
                    }
                    else if (segtextured == R_TextureNumForName("SW2STON2")
                    || segtextured == R_TextureNumForName("SW2STARG"))
                    {
                        walllights = fullbright_greenonly1[lightnum];
                    }
                }

                // Doom 1 & 2
                if (gamemission == doom || gamemission == doom2 || gamemission == pack_tnt || gamemission == pack_plut)
                {
                    if (segtextured == R_TextureNumForName("SW1BRCOM")
                    || segtextured == R_TextureNumForName("SW1DIRT")
                    || segtextured == R_TextureNumForName("SW1STRTN")
                    || segtextured == R_TextureNumForName("SW2COMP")
                    || segtextured == R_TextureNumForName("SW2PANEL")
                    || segtextured == R_TextureNumForName("SW2SLAD")
                    || segtextured == R_TextureNumForName("SW2WOOD")
                    || segtextured == R_TextureNumForName("WOOD4")
                    || segtextured == R_TextureNumForName("WOODSKUL")
                    || segtextured == R_TextureNumForName("SLADSKUL")
                    || segtextured == R_TextureNumForName("SW1BRIK")
                    || segtextured == R_TextureNumForName("SW1COMM")
                    || segtextured == R_TextureNumForName("SW1MET2")
                    || segtextured == R_TextureNumForName("SW1STON1")
                    || segtextured == R_TextureNumForName("SW1STONE")
                    || segtextured == R_TextureNumForName("SW2BLUE")
                    || segtextured == R_TextureNumForName("SW2GSTON")
                    || segtextured == R_TextureNumForName("SW2ROCK")
                    || segtextured == R_TextureNumForName("SW2STON6")
                    || segtextured == R_TextureNumForName("SW2ZIM")
                    || segtextured == R_TextureNumForName("WOODGARG"))
                    {
                        walllights = fullbright_redonly[lightnum];
                    }
                    else if (segtextured == R_TextureNumForName("COMP2")
                    || segtextured == R_TextureNumForName("COMPUTE2")
                    || segtextured == R_TextureNumForName("SILVER3")
                    || segtextured == R_TextureNumForName("COMPUTE1")
                    || segtextured == R_TextureNumForName("COMPUTE3")
                    || segtextured == R_TextureNumForName("SW2MOD1"))
                    {
                        walllights = fullbright_notgrayorbrown[lightnum];
                    }
                    else if (segtextured == R_TextureNumForName("COMPSTA2")
                    || segtextured == R_TextureNumForName("EXITSIGN")
                    || segtextured == R_TextureNumForName("PLANET1")
                    || segtextured == R_TextureNumForName("SW2EXIT")
                    || segtextured == R_TextureNumForName("SW2GRAY1")
                    || segtextured == R_TextureNumForName("COMPSTA1")
                    || segtextured == R_TextureNumForName("EXITSTON")
                    || segtextured == R_TextureNumForName("SILVER2")
                    || segtextured == R_TextureNumForName("LITEBLU1")
                    || segtextured == R_TextureNumForName("SW2GRAY"))
                    {
                        walllights = fullbright_notgray[lightnum];
                    }
                    else if (segtextured == R_TextureNumForName("SW2BRN1")
                    || segtextured == R_TextureNumForName("SW2STON1")
                    || segtextured == R_TextureNumForName("SW2STONE")
                    || segtextured == R_TextureNumForName("SW2TEK")
                    || segtextured == R_TextureNumForName("SW2BRIK")
                    || segtextured == R_TextureNumForName("SW2BRN2")
                    || segtextured == R_TextureNumForName("SW2COMM")
                    || segtextured == R_TextureNumForName("SW2DIRT")
                    || segtextured == R_TextureNumForName("SW2MET2")
                    || segtextured == R_TextureNumForName("SW2STRTN")
                    || segtextured == R_TextureNumForName("SW2VINE")
                    || segtextured == R_TextureNumForName("PIPEWAL1")
                    || segtextured == R_TextureNumForName("TEKLITE2"))
                    {
                        walllights = fullbright_greenonly1[lightnum];
                    }
                    else if (segtextured == R_TextureNumForName("SW2BRCOM")
                    || segtextured == R_TextureNumForName("SW2BRNGN"))
                    {
                        walllights = fullbright_greenonly2[lightnum];
                    }
                    else if (segtextured == R_TextureNumForName("SW2METAL"))
                    {
                        walllights = fullbright_greenonly3[lightnum];
                    }
                }

                // TNT - Evilution
                if (gamemission == pack_tnt)
                {
                    if (segtextured == R_TextureNumForName("LITEYEL2")
                    || segtextured == R_TextureNumForName("LITEYEL3")
                    || segtextured == R_TextureNumForName("YELMETAL"))
                    {
                        walllights = fullbright_orangeyellow[lightnum];
                    }
                    else if (segtextured == R_TextureNumForName("BTNTMETL")
                    || segtextured == R_TextureNumForName("BTNTSLVR"))
                    {
                        walllights = fullbright_notgrayorbrown[lightnum];
                    }
                    else if (segtextured == R_TextureNumForName("PNK4EXIT")
                    || segtextured == R_TextureNumForName("LITERED2"))
                    {
                        walllights = fullbright_redonly[lightnum];
                    }
                }

                // Plutonia
                if (gamemission == pack_plut)
                {
                    if (segtextured == R_TextureNumForName("SW2SKULL"))
                    {
                        walllights = fullbright_redonly[lightnum];
                    }
                }
            }
        }
    }
    }

    // if a floor / ceiling plane is on the wrong side
    //  of the view plane, it is definitely invisible
    //  and doesn't need to be marked.

    if (frontsector->floorheight >= viewz)
    {
        // above view plane
        markfloor = false;
    }

    if (frontsector->ceilingheight <= viewz && frontsector->ceilingpic != skyflatnum)
    {
        // below view plane
        markceiling = false;
    }

    // calculate incremental stepping values for texture edges
    worldtop >>= invhgtbits;
    worldbottom >>= invhgtbits;

    topstep = -FixedMul (rw_scalestep, worldtop);
    topfrac = ((int64_t)centeryfrac>>invhgtbits) - (((int64_t)worldtop * rw_scale)>>FRACBITS); // [crispy] WiggleFix

    bottomstep = -FixedMul (rw_scalestep,worldbottom);
    bottomfrac = ((int64_t)centeryfrac>>invhgtbits) - (((int64_t)worldbottom * rw_scale)>>FRACBITS); // [crispy] WiggleFix

    if (backsector)
    {
        worldhigh >>= invhgtbits;
        worldlow >>= invhgtbits;

        if (worldhigh < worldtop)
        {
            pixhigh = ((int64_t)centeryfrac>>invhgtbits) - (((int64_t)worldhigh * rw_scale)>>FRACBITS); // [crispy] WiggleFix
            pixhighstep = -FixedMul (rw_scalestep,worldhigh);
        }

        if (worldlow > worldbottom)
        {
            pixlow = ((int64_t)centeryfrac>>invhgtbits) - (((int64_t)worldlow * rw_scale)>>FRACBITS); // [crispy] WiggleFix
            pixlowstep = -FixedMul (rw_scalestep,worldlow);
        }
    }

    // render it
    if (markceiling)
    ceilingplane = R_CheckPlane (ceilingplane, rw_x, rw_stopx-1);

    if (markfloor)
    floorplane = R_CheckPlane (floorplane, rw_x, rw_stopx-1);

    R_RenderSegLoop ();

    // save sprite clipping info
    if ( ((ds_p->silhouette & SIL_TOP) || maskedtexture) && !ds_p->sprtopclip)
    {
        memcpy (lastopening, ceilingclip+start, sizeof(*lastopening)*(rw_stopx-start));
        ds_p->sprtopclip = lastopening - start;
        lastopening += rw_stopx - start;
    }

    if ( ((ds_p->silhouette & SIL_BOTTOM) || maskedtexture) && !ds_p->sprbottomclip)
    {
        memcpy (lastopening, floorclip+start, sizeof(*lastopening)*(rw_stopx-start));
        ds_p->sprbottomclip = lastopening - start;
        lastopening += rw_stopx - start;	
    }

    if (maskedtexture && !(ds_p->silhouette&SIL_TOP))
    {
        ds_p->silhouette |= SIL_TOP;
        ds_p->tsilheight = INT_MIN;
    }
    if (maskedtexture && !(ds_p->silhouette&SIL_BOTTOM))
    {
        ds_p->silhouette |= SIL_BOTTOM;
        ds_p->bsilheight = INT_MAX;
    }
    ds_p++;
}

