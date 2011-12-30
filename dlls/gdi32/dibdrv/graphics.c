/*
 * DIB driver graphics operations.
 *
 * Copyright 2011 Huw Davies
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include "gdi_private.h"
#include "dibdrv.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dib);

static RECT get_device_rect( HDC hdc, int left, int top, int right, int bottom, BOOL rtl_correction )
{
    RECT rect;

    rect.left   = left;
    rect.top    = top;
    rect.right  = right;
    rect.bottom = bottom;
    if (rtl_correction && GetLayout( hdc ) & LAYOUT_RTL)
    {
        /* shift the rectangle so that the right border is included after mirroring */
        /* it would be more correct to do this after LPtoDP but that's not what Windows does */
        rect.left--;
        rect.right--;
    }
    LPtoDP( hdc, (POINT *)&rect, 2 );
    if (rect.left > rect.right)
    {
        int tmp = rect.left;
        rect.left = rect.right;
        rect.right = tmp;
    }
    if (rect.top > rect.bottom)
    {
        int tmp = rect.top;
        rect.top = rect.bottom;
        rect.bottom = tmp;
    }
    return rect;
}

/* Intensities of the 17 glyph levels when drawn with text component of 0xff on a
   black bkgnd.  [A log-log plot of these data gives: y = 77.05 * x^0.4315]. */
static const BYTE ramp[17] =
{
    0,    0x4d, 0x68, 0x7c,
    0x8c, 0x9a, 0xa7, 0xb2,
    0xbd, 0xc7, 0xd0, 0xd9,
    0xe1, 0xe9, 0xf0, 0xf8,
    0xff
};

/* For a give text-color component and a glyph level, calculate the
   range of dst intensities, the min/max corresponding to 0/0xff bkgnd
   components respectively.

   The minimum is a linear interpolation between 0 and the value in
   the ramp table.

   The maximum is a linear interpolation between the value from the
   ramp table read in reverse and 0xff.

   To find the resulting pixel intensity, we note that if the text and
   bkgnd intensities are the same then the result must be that
   intensity.  Otherwise we linearly interpolate between either the
   min or the max value and this intermediate value depending on which
   side of the inequality we lie.
*/

static inline void get_range( BYTE aa, DWORD text_comp, BYTE *min_comp, BYTE *max_comp )
{
    *min_comp = (ramp[aa] * text_comp) / 0xff;
    *max_comp = ramp[16 - aa] + ((0xff - ramp[16 - aa]) * text_comp) / 0xff;
}

static inline void get_aa_ranges( COLORREF col, struct intensity_range intensities[17] )
{
    int i;

    for (i = 0; i < 17; i++)
    {
        get_range( i, GetRValue(col), &intensities[i].r_min, &intensities[i].r_max );
        get_range( i, GetGValue(col), &intensities[i].g_min, &intensities[i].g_max );
        get_range( i, GetBValue(col), &intensities[i].b_min, &intensities[i].b_max );
    }
}

/**********************************************************************
 *                 get_text_bkgnd_masks
 *
 * See the comment above get_pen_bkgnd_masks
 */
static inline void get_text_bkgnd_masks( dibdrv_physdev *pdev, rop_mask *mask )
{
    COLORREF bg = GetBkColor( pdev->dev.hdc );

    mask->and = 0;

    if (pdev->dib.bit_count != 1)
        mask->xor = get_pixel_color( pdev, bg, FALSE );
    else
    {
        COLORREF fg = GetTextColor( pdev->dev.hdc );
        mask->xor = get_pixel_color( pdev, fg, TRUE );
        if (fg != bg) mask->xor = ~mask->xor;
    }
}

static void draw_glyph( dibdrv_physdev *pdev, const POINT *origin, const GLYPHMETRICS *metrics,
                        const struct gdi_image_bits *image, DWORD text_color,
                        const struct intensity_range *ranges, const struct clipped_rects *clipped_rects )
{
    int i;
    RECT rect, clipped_rect;
    POINT src_origin;
    dib_info glyph_dib;

    glyph_dib.bit_count = 8;
    glyph_dib.width     = metrics->gmBlackBoxX;
    glyph_dib.height    = metrics->gmBlackBoxY;
    glyph_dib.stride    = get_dib_stride( metrics->gmBlackBoxX, 8 );
    glyph_dib.bits      = *image;

    rect.left   = origin->x  + metrics->gmptGlyphOrigin.x;
    rect.top    = origin->y  - metrics->gmptGlyphOrigin.y;
    rect.right  = rect.left  + metrics->gmBlackBoxX;
    rect.bottom = rect.top   + metrics->gmBlackBoxY;

    for (i = 0; i < clipped_rects->count; i++)
    {
        if (intersect_rect( &clipped_rect, &rect, clipped_rects->rects + i ))
        {
            src_origin.x = clipped_rect.left - rect.left;
            src_origin.y = clipped_rect.top  - rect.top;

            pdev->dib.funcs->draw_glyph( &pdev->dib, &clipped_rect, &glyph_dib, &src_origin,
                                         text_color, ranges );
        }
    }
}

static const BYTE masks[8] = {0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};
static const int padding[4] = {0, 3, 2, 1};

/***********************************************************************
 *         get_glyph_bitmap
 *
 * Retrieve a 17-level bitmap for the appropiate glyph.
 *
 * For non-antialiased bitmaps convert them to the 17-level format
 * using only values 0 or 16.
 */
static DWORD get_glyph_bitmap( HDC hdc, UINT index, UINT aa_flags, GLYPHMETRICS *metrics,
                               struct gdi_image_bits *image )
{
    UINT ggo_flags = aa_flags | GGO_GLYPH_INDEX;
    static const MAT2 identity = { {0,1}, {0,0}, {0,0}, {0,1} };
    UINT indices[3] = {0, 0, 0x20};
    int i, x, y;
    DWORD ret, size;
    BYTE *buf, *dst, *src;
    int pad, stride;

    image->ptr = NULL;
    image->is_copy = FALSE;
    image->free = free_heap_bits;
    image->param = NULL;

    indices[0] = index;

    for (i = 0; i < sizeof(indices) / sizeof(indices[0]); i++)
    {
        index = indices[i];
        ret = GetGlyphOutlineW( hdc, index, ggo_flags, metrics, 0, NULL, &identity );
        if (ret != GDI_ERROR) break;
    }

    if (ret == GDI_ERROR) return ERROR_NOT_FOUND;
    if (!ret) return ERROR_SUCCESS; /* empty glyph */

    /* We'll convert non-antialiased 1-bpp bitmaps to 8-bpp, so these sizes relate to 8-bpp */
    pad = padding[ metrics->gmBlackBoxX % 4 ];
    stride = get_dib_stride( metrics->gmBlackBoxX, 8 );
    size = metrics->gmBlackBoxY * stride;

    buf = HeapAlloc( GetProcessHeap(), 0, size );
    if (!buf) return ERROR_OUTOFMEMORY;

    ret = GetGlyphOutlineW( hdc, index, ggo_flags, metrics, size, buf, &identity );
    if (ret == GDI_ERROR)
    {
        HeapFree( GetProcessHeap(), 0, buf );
        return ERROR_NOT_FOUND;
    }

    if (aa_flags == GGO_BITMAP)
    {
        for (y = metrics->gmBlackBoxY - 1; y >= 0; y--)
        {
            src = buf + y * get_dib_stride( metrics->gmBlackBoxX, 1 );
            dst = buf + y * stride;

            if (pad) memset( dst + metrics->gmBlackBoxX, 0, pad );

            for (x = metrics->gmBlackBoxX - 1; x >= 0; x--)
                dst[x] = (src[x / 8] & masks[x % 8]) ? 0x10 : 0;
        }
    }
    else if (pad)
    {
        for (y = 0, dst = buf; y < metrics->gmBlackBoxY; y++, dst += stride)
            memset( dst + metrics->gmBlackBoxX, 0, pad );
    }

    image->ptr = buf;
    return ERROR_SUCCESS;
}

BOOL render_aa_text_bitmapinfo( HDC hdc, BITMAPINFO *info, struct gdi_image_bits *bits,
                                struct bitblt_coords *src, INT x, INT y, UINT flags,
                                UINT aa_flags, LPCWSTR str, UINT count, const INT *dx )
{
    dib_info dib;
    UINT i;
    DWORD err;
    BOOL got_pixel;
    COLORREF fg, bg;
    DWORD fg_pixel, bg_pixel;
    struct intensity_range glyph_intensities[17];

    assert( info->bmiHeader.biBitCount > 8 ); /* mono and indexed formats don't support anti-aliasing */

    init_dib_info_from_bitmapinfo( &dib, info, bits->ptr, 0 );

    fg = make_rgb_colorref( hdc, &dib, GetTextColor( hdc ), &got_pixel, &fg_pixel);
    if (!got_pixel) fg_pixel = dib.funcs->colorref_to_pixel( &dib, fg );

    get_aa_ranges( fg, glyph_intensities );

    if (flags & ETO_OPAQUE)
    {
        rop_mask bkgnd_color;

        bg = make_rgb_colorref( hdc, &dib, GetBkColor( hdc ), &got_pixel, &bg_pixel);
        if (!got_pixel) bg_pixel = dib.funcs->colorref_to_pixel( &dib, bg );

        bkgnd_color.and = 0;
        bkgnd_color.xor = bg_pixel;
        dib.funcs->solid_rects( &dib, 1, &src->visrect, bkgnd_color.and, bkgnd_color.xor );
    }

    for (i = 0; i < count; i++)
    {
        GLYPHMETRICS metrics;
        struct gdi_image_bits image;

        err = get_glyph_bitmap( hdc, (UINT)str[i], aa_flags, &metrics, &image );
        if (err) continue;

        if (image.ptr)
        {
            RECT rect, clipped_rect;
            POINT src_origin;
            dib_info glyph_dib;

            glyph_dib.bit_count = 8;
            glyph_dib.width     = metrics.gmBlackBoxX;
            glyph_dib.height    = metrics.gmBlackBoxY;
            glyph_dib.stride    = get_dib_stride( metrics.gmBlackBoxX, 8 );
            glyph_dib.bits      = image;

            rect.left   = x + metrics.gmptGlyphOrigin.x;
            rect.top    = y - metrics.gmptGlyphOrigin.y;
            rect.right  = rect.left + metrics.gmBlackBoxX;
            rect.bottom = rect.top  + metrics.gmBlackBoxY;

            if (intersect_rect( &clipped_rect, &rect, &src->visrect ))
            {
                src_origin.x = clipped_rect.left - rect.left;
                src_origin.y = clipped_rect.top  - rect.top;

                dib.funcs->draw_glyph( &dib, &clipped_rect, &glyph_dib, &src_origin,
                                       fg_pixel, glyph_intensities );
            }
        }
        if (image.free) image.free( &image );

        if (dx)
        {
            if (flags & ETO_PDY)
            {
                x += dx[ i * 2 ];
                y += dx[ i * 2 + 1];
            }
            else
                x += dx[ i ];
        }
        else
        {
            x += metrics.gmCellIncX;
            y += metrics.gmCellIncY;
        }
    }
    return TRUE;
}

/***********************************************************************
 *           dibdrv_ExtTextOut
 */
BOOL dibdrv_ExtTextOut( PHYSDEV dev, INT x, INT y, UINT flags,
                        const RECT *rect, LPCWSTR str, UINT count, const INT *dx )
{
    dibdrv_physdev *pdev = get_dibdrv_pdev(dev);
    struct clipped_rects clipped_rects;
    UINT aa_flags, i;
    POINT origin;
    DWORD text_color, err;
    struct intensity_range ranges[17];

    init_clipped_rects( &clipped_rects );

    if (flags & ETO_OPAQUE)
    {
        rop_mask bkgnd_color;
        get_text_bkgnd_masks( pdev, &bkgnd_color );
        get_clipped_rects( &pdev->dib, rect, pdev->clip, &clipped_rects );
        pdev->dib.funcs->solid_rects( &pdev->dib, clipped_rects.count, clipped_rects.rects,
                                      bkgnd_color.and, bkgnd_color.xor );
    }

    if (count == 0) goto done;

    if (flags & ETO_CLIPPED)
    {
        if (!(flags & ETO_OPAQUE))  /* otherwise we have done it already */
            get_clipped_rects( &pdev->dib, rect, pdev->clip, &clipped_rects );
    }
    else
    {
        free_clipped_rects( &clipped_rects );
        get_clipped_rects( &pdev->dib, NULL, pdev->clip, &clipped_rects );
    }
    if (!clipped_rects.count) return TRUE;

    text_color = get_pixel_color( pdev, GetTextColor( pdev->dev.hdc ), TRUE );
    get_aa_ranges( pdev->dib.funcs->pixel_to_colorref( &pdev->dib, text_color ), ranges );

    aa_flags = get_font_aa_flags( dev->hdc );
    origin.x = x;
    origin.y = y;
    for (i = 0; i < count; i++)
    {
        GLYPHMETRICS metrics;
        struct gdi_image_bits image;

        err = get_glyph_bitmap( dev->hdc, (UINT)str[i], aa_flags, &metrics, &image );
        if (err) continue;

        if (image.ptr) draw_glyph( pdev, &origin, &metrics, &image, text_color, ranges, &clipped_rects );
        if (image.free) image.free( &image );

        if (dx)
        {
            if (flags & ETO_PDY)
            {
                origin.x += dx[ i * 2 ];
                origin.y += dx[ i * 2 + 1];
            }
            else
                origin.x += dx[ i ];
        }
        else
        {
            origin.x += metrics.gmCellIncX;
            origin.y += metrics.gmCellIncY;
        }
    }

done:
    free_clipped_rects( &clipped_rects );
    return TRUE;
}

/***********************************************************************
 *           dibdrv_GetNearestColor
 */
COLORREF dibdrv_GetNearestColor( PHYSDEV dev, COLORREF color )
{
    dibdrv_physdev *pdev = get_dibdrv_pdev( dev );
    DWORD pixel;

    TRACE( "(%p, %08x)\n", dev, color );

    pixel = get_pixel_color( pdev, color, FALSE );
    return pdev->dib.funcs->pixel_to_colorref( &pdev->dib, pixel );
}

/***********************************************************************
 *           dibdrv_GetPixel
 */
COLORREF dibdrv_GetPixel( PHYSDEV dev, INT x, INT y )
{
    dibdrv_physdev *pdev = get_dibdrv_pdev( dev );
    POINT pt;
    DWORD pixel;

    TRACE( "(%p, %d, %d)\n", dev, x, y );

    pt.x = x;
    pt.y = y;
    LPtoDP( dev->hdc, &pt, 1 );

    if (pt.x < 0 || pt.x >= pdev->dib.width ||
        pt.y < 0 || pt.y >= pdev->dib.height)
        return CLR_INVALID;

    pixel = pdev->dib.funcs->get_pixel( &pdev->dib, &pt );
    return pdev->dib.funcs->pixel_to_colorref( &pdev->dib, pixel );
}

/***********************************************************************
 *           dibdrv_LineTo
 */
BOOL dibdrv_LineTo( PHYSDEV dev, INT x, INT y )
{
    PHYSDEV next = GET_NEXT_PHYSDEV( dev, pLineTo );
    dibdrv_physdev *pdev = get_dibdrv_pdev(dev);
    POINT pts[2];
    HRGN region = 0;
    BOOL ret;

    if(defer_pen(pdev)) return next->funcs->pLineTo( next, x, y );

    GetCurrentPositionEx(dev->hdc, pts);
    pts[1].x = x;
    pts[1].y = y;

    LPtoDP(dev->hdc, pts, 2);

    if (pdev->pen_uses_region && !(region = CreateRectRgn( 0, 0, 0, 0 ))) return FALSE;

    reset_dash_origin(pdev);

    ret = pdev->pen_lines(pdev, 2, pts, FALSE, region);

    if (region)
    {
        if (pdev->clip) CombineRgn( region, region, pdev->clip, RGN_AND );
        ret = pen_region( pdev, region );
        DeleteObject( region );
    }
    return ret;
}

/***********************************************************************
 *           get_rop2_from_rop
 *
 * Returns the binary rop that is equivalent to the provided ternary rop
 * if the src bits are ignored.
 */
static inline INT get_rop2_from_rop(INT rop)
{
    return (((rop >> 18) & 0x0c) | ((rop >> 16) & 0x03)) + 1;
}

/***********************************************************************
 *           dibdrv_PatBlt
 */
BOOL dibdrv_PatBlt( PHYSDEV dev, struct bitblt_coords *dst, DWORD rop )
{
    dibdrv_physdev *pdev = get_dibdrv_pdev(dev);

    TRACE("(%p, %d, %d, %d, %d, %06x)\n", dev, dst->x, dst->y, dst->width, dst->height, rop);

    return brush_rect( pdev, &pdev->brush, &dst->visrect, pdev->clip, get_rop2_from_rop(rop) );
}

/***********************************************************************
 *           dibdrv_PaintRgn
 */
BOOL dibdrv_PaintRgn( PHYSDEV dev, HRGN rgn )
{
    dibdrv_physdev *pdev = get_dibdrv_pdev(dev);
    const WINEREGION *region;
    int i;
    RECT rect;

    TRACE("%p, %p\n", dev, rgn);

    region = get_wine_region( rgn );
    if(!region) return FALSE;

    for(i = 0; i < region->numRects; i++)
    {
        rect = get_device_rect( dev->hdc, region->rects[i].left, region->rects[i].top,
                                region->rects[i].right, region->rects[i].bottom, FALSE );
        brush_rect( pdev, &pdev->brush, &rect, pdev->clip, GetROP2( dev->hdc ) );
    }

    release_wine_region( rgn );
    return TRUE;
}

/***********************************************************************
 *           dibdrv_PolyPolygon
 */
BOOL dibdrv_PolyPolygon( PHYSDEV dev, const POINT *pt, const INT *counts, DWORD polygons )
{
    dibdrv_physdev *pdev = get_dibdrv_pdev(dev);
    PHYSDEV next = GET_NEXT_PHYSDEV( dev, pPolyPolygon );
    DWORD total, i, pos;
    BOOL ret = TRUE;
    POINT *points;
    HRGN outline = 0, interior = 0;

    if (defer_pen( pdev )) return next->funcs->pPolyPolygon( next, pt, counts, polygons );

    for (i = total = 0; i < polygons; i++)
    {
        if (counts[i] < 2) return FALSE;
        total += counts[i];
    }

    points = HeapAlloc( GetProcessHeap(), 0, total * sizeof(*pt) );
    if (!points) return FALSE;
    memcpy( points, pt, total * sizeof(*pt) );
    LPtoDP( dev->hdc, points, total );

    if (pdev->brush.style != BS_NULL &&
        !(interior = CreatePolyPolygonRgn( points, counts, polygons, GetPolyFillMode( dev->hdc ))))
    {
        HeapFree( GetProcessHeap(), 0, points );
        return FALSE;
    }

    if (pdev->pen_uses_region) outline = CreateRectRgn( 0, 0, 0, 0 );

    /* if not using a region, paint the interior first so the outline can overlap it */
    if (interior && !outline)
    {
        ret = brush_region( pdev, interior );
        DeleteObject( interior );
        interior = 0;
    }

    for (i = pos = 0; i < polygons; i++)
    {
        reset_dash_origin( pdev );
        pdev->pen_lines( pdev, counts[i], points + pos, TRUE, outline );
        pos += counts[i];
    }

    if (interior)
    {
        CombineRgn( interior, interior, outline, RGN_DIFF );
        ret = brush_region( pdev, interior );
        DeleteObject( interior );
    }
    if (outline)
    {
        if (ret) ret = pen_region( pdev, outline );
        DeleteObject( outline );
    }
    HeapFree( GetProcessHeap(), 0, points );
    return ret;
}

/***********************************************************************
 *           dibdrv_PolyPolyline
 */
BOOL dibdrv_PolyPolyline( PHYSDEV dev, const POINT* pt, const DWORD* counts, DWORD polylines )
{
    dibdrv_physdev *pdev = get_dibdrv_pdev(dev);
    PHYSDEV next = GET_NEXT_PHYSDEV( dev, pPolyPolyline );
    DWORD max_points = 0, i;
    POINT *points;
    BOOL ret = TRUE;
    HRGN outline = 0;

    if (defer_pen( pdev )) return next->funcs->pPolyPolyline( next, pt, counts, polylines );

    for (i = 0; i < polylines; i++)
    {
        if (counts[i] < 2) return FALSE;
        max_points = max( counts[i], max_points );
    }

    points = HeapAlloc( GetProcessHeap(), 0, max_points * sizeof(*pt) );
    if (!points) return FALSE;

    if (pdev->pen_uses_region && !(outline = CreateRectRgn( 0, 0, 0, 0 )))
    {
        HeapFree( GetProcessHeap(), 0, points );
        return FALSE;
    }

    for (i = 0; i < polylines; i++)
    {
        memcpy( points, pt, counts[i] * sizeof(*pt) );
        pt += counts[i];
        LPtoDP( dev->hdc, points, counts[i] );

        reset_dash_origin( pdev );
        pdev->pen_lines( pdev, counts[i], points, FALSE, outline );
    }

    if (outline)
    {
        if (pdev->clip) CombineRgn( outline, outline, pdev->clip, RGN_AND );
        ret = pen_region( pdev, outline );
        DeleteObject( outline );
    }

    HeapFree( GetProcessHeap(), 0, points );
    return ret;
}

/***********************************************************************
 *           dibdrv_Polygon
 */
BOOL dibdrv_Polygon( PHYSDEV dev, const POINT *pt, INT count )
{
    INT counts[1] = { count };

    return dibdrv_PolyPolygon( dev, pt, counts, 1 );
}

/***********************************************************************
 *           dibdrv_Polyline
 */
BOOL dibdrv_Polyline( PHYSDEV dev, const POINT* pt, INT count )
{
    DWORD counts[1] = { count };

    if (count < 0) return FALSE;
    return dibdrv_PolyPolyline( dev, pt, counts, 1 );
}

/***********************************************************************
 *           dibdrv_Rectangle
 */
BOOL dibdrv_Rectangle( PHYSDEV dev, INT left, INT top, INT right, INT bottom )
{
    PHYSDEV next = GET_NEXT_PHYSDEV( dev, pRectangle );
    dibdrv_physdev *pdev = get_dibdrv_pdev(dev);
    RECT rect = get_device_rect( dev->hdc, left, top, right, bottom, TRUE );
    POINT pts[4];
    BOOL ret;
    HRGN outline = 0;

    TRACE("(%p, %d, %d, %d, %d)\n", dev, left, top, right, bottom);

    if(rect.left == rect.right || rect.top == rect.bottom) return TRUE;

    if(defer_pen(pdev))
        return next->funcs->pRectangle( next, left, top, right, bottom );

    if (pdev->pen_style == PS_INSIDEFRAME)
    {
        rect.left   += pdev->pen_width / 2;
        rect.top    += pdev->pen_width / 2;
        rect.right  -= (pdev->pen_width - 1) / 2;
        rect.bottom -= (pdev->pen_width - 1) / 2;
    }

    if (pdev->pen_uses_region && !(outline = CreateRectRgn( 0, 0, 0, 0 ))) return FALSE;

    rect.right--;
    rect.bottom--;
    reset_dash_origin(pdev);

    /* 4 pts going anti-clockwise starting from top-right */
    pts[0].x = pts[3].x = rect.right;
    pts[0].y = pts[1].y = rect.top;
    pts[1].x = pts[2].x = rect.left;
    pts[2].y = pts[3].y = rect.bottom;

    pdev->pen_lines(pdev, 4, pts, TRUE, outline);

    if (outline)
    {
        if (pdev->brush.style != BS_NULL)
        {
            HRGN interior = CreateRectRgnIndirect( &rect );

            CombineRgn( interior, interior, outline, RGN_DIFF );
            brush_region( pdev, interior );
            DeleteObject( interior );
        }
        ret = pen_region( pdev, outline );
        DeleteObject( outline );
    }
    else
    {
        rect.left   += (pdev->pen_width + 1) / 2;
        rect.top    += (pdev->pen_width + 1) / 2;
        rect.right  -= pdev->pen_width / 2;
        rect.bottom -= pdev->pen_width / 2;
        ret = brush_rect( pdev, &pdev->brush, &rect, pdev->clip, GetROP2(dev->hdc) );
    }
    return ret;
}

/***********************************************************************
 *           dibdrv_SetPixel
 */
COLORREF dibdrv_SetPixel( PHYSDEV dev, INT x, INT y, COLORREF color )
{
    dibdrv_physdev *pdev = get_dibdrv_pdev( dev );
    struct clipped_rects clipped_rects;
    RECT rect;
    POINT pt;
    DWORD pixel;

    TRACE( "(%p, %d, %d, %08x)\n", dev, x, y, color );

    pt.x = x;
    pt.y = y;
    LPtoDP( dev->hdc, &pt, 1 );
    rect.left = pt.x;
    rect.top =  pt.y;
    rect.right = rect.left + 1;
    rect.bottom = rect.top + 1;

    /* SetPixel doesn't do the 1bpp massaging like other fg colors */
    pixel = get_pixel_color( pdev, color, FALSE );
    color = pdev->dib.funcs->pixel_to_colorref( &pdev->dib, pixel );

    if (!get_clipped_rects( &pdev->dib, &rect, pdev->clip, &clipped_rects )) return color;
    pdev->dib.funcs->solid_rects( &pdev->dib, clipped_rects.count, clipped_rects.rects, 0, pixel );
    free_clipped_rects( &clipped_rects );
    return color;
}
