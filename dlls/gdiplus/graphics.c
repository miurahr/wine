/*
 * Copyright (C) 2007 Google (Evan Stade)
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

#include <stdarg.h>
#include <math.h>
#include <limits.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "wingdi.h"
#include "wine/unicode.h"

#define COBJMACROS
#include "objbase.h"
#include "ocidl.h"
#include "olectl.h"
#include "ole2.h"

#include "winreg.h"
#include "shlwapi.h"

#include "gdiplus.h"
#include "gdiplus_private.h"
#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdiplus);

/* looks-right constants */
#define ANCHOR_WIDTH (2.0)
#define MAX_ITERS (50)

/* Converts angle (in degrees) to x/y coordinates */
static void deg2xy(REAL angle, REAL x_0, REAL y_0, REAL *x, REAL *y)
{
    REAL radAngle, hypotenuse;

    radAngle = deg2rad(angle);
    hypotenuse = 50.0; /* arbitrary */

    *x = x_0 + cos(radAngle) * hypotenuse;
    *y = y_0 + sin(radAngle) * hypotenuse;
}

/* Converts from gdiplus path point type to gdi path point type. */
static BYTE convert_path_point_type(BYTE type)
{
    BYTE ret;

    switch(type & PathPointTypePathTypeMask){
        case PathPointTypeBezier:
            ret = PT_BEZIERTO;
            break;
        case PathPointTypeLine:
            ret = PT_LINETO;
            break;
        case PathPointTypeStart:
            ret = PT_MOVETO;
            break;
        default:
            ERR("Bad point type\n");
            return 0;
    }

    if(type & PathPointTypeCloseSubpath)
        ret |= PT_CLOSEFIGURE;

    return ret;
}

static COLORREF get_gdi_brush_color(const GpBrush *brush)
{
    ARGB argb;

    switch (brush->bt)
    {
        case BrushTypeSolidColor:
        {
            const GpSolidFill *sf = (const GpSolidFill *)brush;
            argb = sf->color;
            break;
        }
        case BrushTypeHatchFill:
        {
            const GpHatch *hatch = (const GpHatch *)brush;
            argb = hatch->forecol;
            break;
        }
        case BrushTypeLinearGradient:
        {
            const GpLineGradient *line = (const GpLineGradient *)brush;
            argb = line->startcolor;
            break;
        }
        case BrushTypePathGradient:
        {
            const GpPathGradient *grad = (const GpPathGradient *)brush;
            argb = grad->centercolor;
            break;
        }
        default:
            FIXME("unhandled brush type %d\n", brush->bt);
            argb = 0;
            break;
    }
    return ARGB2COLORREF(argb);
}

static HBITMAP create_hatch_bitmap(const GpHatch *hatch)
{
    HBITMAP hbmp;
    BITMAPINFOHEADER bmih;
    DWORD *bits;
    int x, y;

    bmih.biSize = sizeof(bmih);
    bmih.biWidth = 8;
    bmih.biHeight = 8;
    bmih.biPlanes = 1;
    bmih.biBitCount = 32;
    bmih.biCompression = BI_RGB;
    bmih.biSizeImage = 0;

    hbmp = CreateDIBSection(0, (BITMAPINFO *)&bmih, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (hbmp)
    {
        const char *hatch_data;

        if (get_hatch_data(hatch->hatchstyle, &hatch_data) == Ok)
        {
            for (y = 0; y < 8; y++)
            {
                for (x = 0; x < 8; x++)
                {
                    if (hatch_data[y] & (0x80 >> x))
                        bits[y * 8 + x] = hatch->forecol;
                    else
                        bits[y * 8 + x] = hatch->backcol;
                }
            }
        }
        else
        {
            FIXME("Unimplemented hatch style %d\n", hatch->hatchstyle);

            for (y = 0; y < 64; y++)
                bits[y] = hatch->forecol;
        }
    }

    return hbmp;
}

static GpStatus create_gdi_logbrush(const GpBrush *brush, LOGBRUSH *lb)
{
    switch (brush->bt)
    {
        case BrushTypeSolidColor:
        {
            const GpSolidFill *sf = (const GpSolidFill *)brush;
            lb->lbStyle = BS_SOLID;
            lb->lbColor = ARGB2COLORREF(sf->color);
            lb->lbHatch = 0;
            return Ok;
        }

        case BrushTypeHatchFill:
        {
            const GpHatch *hatch = (const GpHatch *)brush;
            HBITMAP hbmp;

            hbmp = create_hatch_bitmap(hatch);
            if (!hbmp) return OutOfMemory;

            lb->lbStyle = BS_PATTERN;
            lb->lbColor = 0;
            lb->lbHatch = (ULONG_PTR)hbmp;
            return Ok;
        }

        default:
            FIXME("unhandled brush type %d\n", brush->bt);
            lb->lbStyle = BS_SOLID;
            lb->lbColor = get_gdi_brush_color(brush);
            lb->lbHatch = 0;
            return Ok;
    }
}

static GpStatus free_gdi_logbrush(LOGBRUSH *lb)
{
    switch (lb->lbStyle)
    {
        case BS_PATTERN:
            DeleteObject((HGDIOBJ)(ULONG_PTR)lb->lbHatch);
            break;
    }
    return Ok;
}

static HBRUSH create_gdi_brush(const GpBrush *brush)
{
    LOGBRUSH lb;
    HBRUSH gdibrush;

    if (create_gdi_logbrush(brush, &lb) != Ok) return 0;

    gdibrush = CreateBrushIndirect(&lb);
    free_gdi_logbrush(&lb);

    return gdibrush;
}

static INT prepare_dc(GpGraphics *graphics, GpPen *pen)
{
    LOGBRUSH lb;
    HPEN gdipen;
    REAL width;
    INT save_state, i, numdashes;
    GpPointF pt[2];
    DWORD dash_array[MAX_DASHLEN];

    save_state = SaveDC(graphics->hdc);

    EndPath(graphics->hdc);

    if(pen->unit == UnitPixel){
        width = pen->width;
    }
    else{
        /* Get an estimate for the amount the pen width is affected by the world
         * transform. (This is similar to what some of the wine drivers do.) */
        pt[0].X = 0.0;
        pt[0].Y = 0.0;
        pt[1].X = 1.0;
        pt[1].Y = 1.0;
        GdipTransformMatrixPoints(graphics->worldtrans, pt, 2);
        width = sqrt((pt[1].X - pt[0].X) * (pt[1].X - pt[0].X) +
                     (pt[1].Y - pt[0].Y) * (pt[1].Y - pt[0].Y)) / sqrt(2.0);

        width *= units_to_pixels(pen->width, pen->unit == UnitWorld ? graphics->unit : pen->unit, graphics->xres);
    }

    if(pen->dash == DashStyleCustom){
        numdashes = min(pen->numdashes, MAX_DASHLEN);

        TRACE("dashes are: ");
        for(i = 0; i < numdashes; i++){
            dash_array[i] = gdip_round(width * pen->dashes[i]);
            TRACE("%d, ", dash_array[i]);
        }
        TRACE("\n and the pen style is %x\n", pen->style);

        create_gdi_logbrush(pen->brush, &lb);
        gdipen = ExtCreatePen(pen->style, gdip_round(width), &lb,
                              numdashes, dash_array);
        free_gdi_logbrush(&lb);
    }
    else
    {
        create_gdi_logbrush(pen->brush, &lb);
        gdipen = ExtCreatePen(pen->style, gdip_round(width), &lb, 0, NULL);
        free_gdi_logbrush(&lb);
    }

    SelectObject(graphics->hdc, gdipen);

    return save_state;
}

static void restore_dc(GpGraphics *graphics, INT state)
{
    DeleteObject(SelectObject(graphics->hdc, GetStockObject(NULL_PEN)));
    RestoreDC(graphics->hdc, state);
}

static GpStatus get_graphics_transform(GpGraphics *graphics, GpCoordinateSpace dst_space,
        GpCoordinateSpace src_space, GpMatrix **matrix);

/* This helper applies all the changes that the points listed in ptf need in
 * order to be drawn on the device context.  In the end, this should include at
 * least:
 *  -scaling by page unit
 *  -applying world transformation
 *  -converting from float to int
 * Native gdiplus uses gdi32 to do all this (via SetMapMode, SetViewportExtEx,
 * SetWindowExtEx, SetWorldTransform, etc.) but we cannot because we are using
 * gdi to draw, and these functions would irreparably mess with line widths.
 */
static void transform_and_round_points(GpGraphics *graphics, POINT *pti,
    GpPointF *ptf, INT count)
{
    REAL scale_x, scale_y;
    GpMatrix *matrix;
    int i;

    scale_x = units_to_pixels(1.0, graphics->unit, graphics->xres);
    scale_y = units_to_pixels(1.0, graphics->unit, graphics->yres);

    /* apply page scale */
    if(graphics->unit != UnitDisplay)
    {
        scale_x *= graphics->scale;
        scale_y *= graphics->scale;
    }

    GdipCloneMatrix(graphics->worldtrans, &matrix);
    GdipScaleMatrix(matrix, scale_x, scale_y, MatrixOrderAppend);
    GdipTransformMatrixPoints(matrix, ptf, count);
    GdipDeleteMatrix(matrix);

    for(i = 0; i < count; i++){
        pti[i].x = gdip_round(ptf[i].X);
        pti[i].y = gdip_round(ptf[i].Y);
    }
}

static void gdi_alpha_blend(GpGraphics *graphics, INT dst_x, INT dst_y, INT dst_width, INT dst_height,
                            HDC hdc, INT src_x, INT src_y, INT src_width, INT src_height)
{
    if (GetDeviceCaps(graphics->hdc, SHADEBLENDCAPS) == SB_NONE)
    {
        TRACE("alpha blending not supported by device, fallback to StretchBlt\n");

        StretchBlt(graphics->hdc, dst_x, dst_y, dst_width, dst_height,
                   hdc, src_x, src_y, src_width, src_height, SRCCOPY);
    }
    else
    {
        BLENDFUNCTION bf;

        bf.BlendOp = AC_SRC_OVER;
        bf.BlendFlags = 0;
        bf.SourceConstantAlpha = 255;
        bf.AlphaFormat = AC_SRC_ALPHA;

        GdiAlphaBlend(graphics->hdc, dst_x, dst_y, dst_width, dst_height,
                      hdc, src_x, src_y, src_width, src_height, bf);
    }
}

/* Draw non-premultiplied ARGB data to the given graphics object */
static GpStatus alpha_blend_pixels(GpGraphics *graphics, INT dst_x, INT dst_y,
    const BYTE *src, INT src_width, INT src_height, INT src_stride)
{
    if (graphics->image && graphics->image->type == ImageTypeBitmap)
    {
        GpBitmap *dst_bitmap = (GpBitmap*)graphics->image;
        INT x, y;

        for (x=0; x<src_width; x++)
        {
            for (y=0; y<src_height; y++)
            {
                ARGB dst_color, src_color;
                GdipBitmapGetPixel(dst_bitmap, x+dst_x, y+dst_y, &dst_color);
                src_color = ((ARGB*)(src + src_stride * y))[x];
                GdipBitmapSetPixel(dst_bitmap, x+dst_x, y+dst_y, color_over(dst_color, src_color));
            }
        }

        return Ok;
    }
    else if (graphics->image && graphics->image->type == ImageTypeMetafile)
    {
        ERR("This should not be used for metafiles; fix caller\n");
        return NotImplemented;
    }
    else
    {
        HDC hdc;
        HBITMAP hbitmap;
        BITMAPINFOHEADER bih;
        BYTE *temp_bits;

        hdc = CreateCompatibleDC(0);

        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = src_width;
        bih.biHeight = -src_height;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        bih.biSizeImage = 0;
        bih.biXPelsPerMeter = 0;
        bih.biYPelsPerMeter = 0;
        bih.biClrUsed = 0;
        bih.biClrImportant = 0;

        hbitmap = CreateDIBSection(hdc, (BITMAPINFO*)&bih, DIB_RGB_COLORS,
            (void**)&temp_bits, NULL, 0);

        convert_32bppARGB_to_32bppPARGB(src_width, src_height, temp_bits,
            4 * src_width, src, src_stride);

        SelectObject(hdc, hbitmap);
        gdi_alpha_blend(graphics, dst_x, dst_y, src_width, src_height,
                        hdc, 0, 0, src_width, src_height);
        DeleteDC(hdc);
        DeleteObject(hbitmap);

        return Ok;
    }
}

static GpStatus alpha_blend_pixels_hrgn(GpGraphics *graphics, INT dst_x, INT dst_y,
    const BYTE *src, INT src_width, INT src_height, INT src_stride, HRGN hregion)
{
    GpStatus stat=Ok;

    if (graphics->image && graphics->image->type == ImageTypeBitmap)
    {
        int i, size;
        RGNDATA *rgndata;
        RECT *rects;

        size = GetRegionData(hregion, 0, NULL);

        rgndata = GdipAlloc(size);
        if (!rgndata)
            return OutOfMemory;

        GetRegionData(hregion, size, rgndata);

        rects = (RECT*)&rgndata->Buffer;

        for (i=0; stat == Ok && i<rgndata->rdh.nCount; i++)
        {
            stat = alpha_blend_pixels(graphics, rects[i].left, rects[i].top,
                &src[(rects[i].left - dst_x) * 4 + (rects[i].top - dst_y) * src_stride],
                rects[i].right - rects[i].left, rects[i].bottom - rects[i].top,
                src_stride);
        }

        GdipFree(rgndata);

        return stat;
    }
    else if (graphics->image && graphics->image->type == ImageTypeMetafile)
    {
        ERR("This should not be used for metafiles; fix caller\n");
        return NotImplemented;
    }
    else
    {
        int save;

        save = SaveDC(graphics->hdc);

        ExtSelectClipRgn(graphics->hdc, hregion, RGN_AND);

        stat = alpha_blend_pixels(graphics, dst_x, dst_y, src, src_width,
            src_height, src_stride);

        RestoreDC(graphics->hdc, save);

        return stat;
    }
}

static ARGB blend_colors(ARGB start, ARGB end, REAL position)
{
    ARGB result=0;
    ARGB i;
    INT a1, a2, a3;

    a1 = (start >> 24) & 0xff;
    a2 = (end >> 24) & 0xff;

    a3 = (int)(a1*(1.0f - position)+a2*(position));

    result |= a3 << 24;

    for (i=0xff; i<=0xff0000; i = i << 8)
        result |= (int)((start&i)*(1.0f - position)+(end&i)*(position))&i;
    return result;
}

static ARGB blend_line_gradient(GpLineGradient* brush, REAL position)
{
    REAL blendfac;

    /* clamp to between 0.0 and 1.0, using the wrap mode */
    if (brush->wrap == WrapModeTile)
    {
        position = fmodf(position, 1.0f);
        if (position < 0.0f) position += 1.0f;
    }
    else /* WrapModeFlip* */
    {
        position = fmodf(position, 2.0f);
        if (position < 0.0f) position += 2.0f;
        if (position > 1.0f) position = 2.0f - position;
    }

    if (brush->blendcount == 1)
        blendfac = position;
    else
    {
        int i=1;
        REAL left_blendpos, left_blendfac, right_blendpos, right_blendfac;
        REAL range;

        /* locate the blend positions surrounding this position */
        while (position > brush->blendpos[i])
            i++;

        /* interpolate between the blend positions */
        left_blendpos = brush->blendpos[i-1];
        left_blendfac = brush->blendfac[i-1];
        right_blendpos = brush->blendpos[i];
        right_blendfac = brush->blendfac[i];
        range = right_blendpos - left_blendpos;
        blendfac = (left_blendfac * (right_blendpos - position) +
                    right_blendfac * (position - left_blendpos)) / range;
    }

    if (brush->pblendcount == 0)
        return blend_colors(brush->startcolor, brush->endcolor, blendfac);
    else
    {
        int i=1;
        ARGB left_blendcolor, right_blendcolor;
        REAL left_blendpos, right_blendpos;

        /* locate the blend colors surrounding this position */
        while (blendfac > brush->pblendpos[i])
            i++;

        /* interpolate between the blend colors */
        left_blendpos = brush->pblendpos[i-1];
        left_blendcolor = brush->pblendcolor[i-1];
        right_blendpos = brush->pblendpos[i];
        right_blendcolor = brush->pblendcolor[i];
        blendfac = (blendfac - left_blendpos) / (right_blendpos - left_blendpos);
        return blend_colors(left_blendcolor, right_blendcolor, blendfac);
    }
}

static ARGB transform_color(ARGB color, const ColorMatrix *matrix)
{
    REAL val[5], res[4];
    int i, j;
    unsigned char a, r, g, b;

    val[0] = ((color >> 16) & 0xff) / 255.0; /* red */
    val[1] = ((color >> 8) & 0xff) / 255.0; /* green */
    val[2] = (color & 0xff) / 255.0; /* blue */
    val[3] = ((color >> 24) & 0xff) / 255.0; /* alpha */
    val[4] = 1.0; /* translation */

    for (i=0; i<4; i++)
    {
        res[i] = 0.0;

        for (j=0; j<5; j++)
            res[i] += matrix->m[j][i] * val[j];
    }

    a = min(max(floorf(res[3]*255.0), 0.0), 255.0);
    r = min(max(floorf(res[0]*255.0), 0.0), 255.0);
    g = min(max(floorf(res[1]*255.0), 0.0), 255.0);
    b = min(max(floorf(res[2]*255.0), 0.0), 255.0);

    return (a << 24) | (r << 16) | (g << 8) | b;
}

static int color_is_gray(ARGB color)
{
    unsigned char r, g, b;

    r = (color >> 16) & 0xff;
    g = (color >> 8) & 0xff;
    b = color & 0xff;

    return (r == g) && (g == b);
}

static void apply_image_attributes(const GpImageAttributes *attributes, LPBYTE data,
    UINT width, UINT height, INT stride, ColorAdjustType type)
{
    UINT x, y, i;

    if (attributes->colorkeys[type].enabled ||
        attributes->colorkeys[ColorAdjustTypeDefault].enabled)
    {
        const struct color_key *key;
        BYTE min_blue, min_green, min_red;
        BYTE max_blue, max_green, max_red;

        if (attributes->colorkeys[type].enabled)
            key = &attributes->colorkeys[type];
        else
            key = &attributes->colorkeys[ColorAdjustTypeDefault];

        min_blue = key->low&0xff;
        min_green = (key->low>>8)&0xff;
        min_red = (key->low>>16)&0xff;

        max_blue = key->high&0xff;
        max_green = (key->high>>8)&0xff;
        max_red = (key->high>>16)&0xff;

        for (x=0; x<width; x++)
            for (y=0; y<height; y++)
            {
                ARGB *src_color;
                BYTE blue, green, red;
                src_color = (ARGB*)(data + stride * y + sizeof(ARGB) * x);
                blue = *src_color&0xff;
                green = (*src_color>>8)&0xff;
                red = (*src_color>>16)&0xff;
                if (blue >= min_blue && green >= min_green && red >= min_red &&
                    blue <= max_blue && green <= max_green && red <= max_red)
                    *src_color = 0x00000000;
            }
    }

    if (attributes->colorremaptables[type].enabled ||
        attributes->colorremaptables[ColorAdjustTypeDefault].enabled)
    {
        const struct color_remap_table *table;

        if (attributes->colorremaptables[type].enabled)
            table = &attributes->colorremaptables[type];
        else
            table = &attributes->colorremaptables[ColorAdjustTypeDefault];

        for (x=0; x<width; x++)
            for (y=0; y<height; y++)
            {
                ARGB *src_color;
                src_color = (ARGB*)(data + stride * y + sizeof(ARGB) * x);
                for (i=0; i<table->mapsize; i++)
                {
                    if (*src_color == table->colormap[i].oldColor.Argb)
                    {
                        *src_color = table->colormap[i].newColor.Argb;
                        break;
                    }
                }
            }
    }

    if (attributes->colormatrices[type].enabled ||
        attributes->colormatrices[ColorAdjustTypeDefault].enabled)
    {
        const struct color_matrix *colormatrices;

        if (attributes->colormatrices[type].enabled)
            colormatrices = &attributes->colormatrices[type];
        else
            colormatrices = &attributes->colormatrices[ColorAdjustTypeDefault];

        for (x=0; x<width; x++)
            for (y=0; y<height; y++)
            {
                ARGB *src_color;
                src_color = (ARGB*)(data + stride * y + sizeof(ARGB) * x);

                if (colormatrices->flags == ColorMatrixFlagsDefault ||
                    !color_is_gray(*src_color))
                {
                    *src_color = transform_color(*src_color, &colormatrices->colormatrix);
                }
                else if (colormatrices->flags == ColorMatrixFlagsAltGray)
                {
                    *src_color = transform_color(*src_color, &colormatrices->graymatrix);
                }
            }
    }

    if (attributes->gamma_enabled[type] ||
        attributes->gamma_enabled[ColorAdjustTypeDefault])
    {
        REAL gamma;

        if (attributes->gamma_enabled[type])
            gamma = attributes->gamma[type];
        else
            gamma = attributes->gamma[ColorAdjustTypeDefault];

        for (x=0; x<width; x++)
            for (y=0; y<height; y++)
            {
                ARGB *src_color;
                BYTE blue, green, red;
                src_color = (ARGB*)(data + stride * y + sizeof(ARGB) * x);

                blue = *src_color&0xff;
                green = (*src_color>>8)&0xff;
                red = (*src_color>>16)&0xff;

                /* FIXME: We should probably use a table for this. */
                blue = floorf(powf(blue / 255.0, gamma) * 255.0);
                green = floorf(powf(green / 255.0, gamma) * 255.0);
                red = floorf(powf(red / 255.0, gamma) * 255.0);

                *src_color = (*src_color & 0xff000000) | (red << 16) | (green << 8) | blue;
            }
    }
}

/* Given a bitmap and its source rectangle, find the smallest rectangle in the
 * bitmap that contains all the pixels we may need to draw it. */
static void get_bitmap_sample_size(InterpolationMode interpolation, WrapMode wrap,
    GpBitmap* bitmap, REAL srcx, REAL srcy, REAL srcwidth, REAL srcheight,
    GpRect *rect)
{
    INT left, top, right, bottom;

    switch (interpolation)
    {
    case InterpolationModeHighQualityBilinear:
    case InterpolationModeHighQualityBicubic:
    /* FIXME: Include a greater range for the prefilter? */
    case InterpolationModeBicubic:
    case InterpolationModeBilinear:
        left = (INT)(floorf(srcx));
        top = (INT)(floorf(srcy));
        right = (INT)(ceilf(srcx+srcwidth));
        bottom = (INT)(ceilf(srcy+srcheight));
        break;
    case InterpolationModeNearestNeighbor:
    default:
        left = gdip_round(srcx);
        top = gdip_round(srcy);
        right = gdip_round(srcx+srcwidth);
        bottom = gdip_round(srcy+srcheight);
        break;
    }

    if (wrap == WrapModeClamp)
    {
        if (left < 0)
            left = 0;
        if (top < 0)
            top = 0;
        if (right >= bitmap->width)
            right = bitmap->width-1;
        if (bottom >= bitmap->height)
            bottom = bitmap->height-1;
    }
    else
    {
        /* In some cases we can make the rectangle smaller here, but the logic
         * is hard to get right, and tiling suggests we're likely to use the
         * entire source image. */
        if (left < 0 || right >= bitmap->width)
        {
            left = 0;
            right = bitmap->width-1;
        }

        if (top < 0 || bottom >= bitmap->height)
        {
            top = 0;
            bottom = bitmap->height-1;
        }
    }

    rect->X = left;
    rect->Y = top;
    rect->Width = right - left + 1;
    rect->Height = bottom - top + 1;
}

static ARGB sample_bitmap_pixel(GDIPCONST GpRect *src_rect, LPBYTE bits, UINT width,
    UINT height, INT x, INT y, GDIPCONST GpImageAttributes *attributes)
{
    if (attributes->wrap == WrapModeClamp)
    {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return attributes->outside_color;
    }
    else
    {
        /* Tiling. Make sure co-ordinates are positive as it simplifies the math. */
        if (x < 0)
            x = width*2 + x % (width * 2);
        if (y < 0)
            y = height*2 + y % (height * 2);

        if ((attributes->wrap & 1) == 1)
        {
            /* Flip X */
            if ((x / width) % 2 == 0)
                x = x % width;
            else
                x = width - 1 - x % width;
        }
        else
            x = x % width;

        if ((attributes->wrap & 2) == 2)
        {
            /* Flip Y */
            if ((y / height) % 2 == 0)
                y = y % height;
            else
                y = height - 1 - y % height;
        }
        else
            y = y % height;
    }

    if (x < src_rect->X || y < src_rect->Y || x >= src_rect->X + src_rect->Width || y >= src_rect->Y + src_rect->Height)
    {
        ERR("out of range pixel requested\n");
        return 0xffcd0084;
    }

    return ((DWORD*)(bits))[(x - src_rect->X) + (y - src_rect->Y) * src_rect->Width];
}

static ARGB resample_bitmap_pixel(GDIPCONST GpRect *src_rect, LPBYTE bits, UINT width,
    UINT height, GpPointF *point, GDIPCONST GpImageAttributes *attributes,
    InterpolationMode interpolation)
{
    static int fixme;

    switch (interpolation)
    {
    default:
        if (!fixme++)
            FIXME("Unimplemented interpolation %i\n", interpolation);
        /* fall-through */
    case InterpolationModeBilinear:
    {
        REAL leftxf, topyf;
        INT leftx, rightx, topy, bottomy;
        ARGB topleft, topright, bottomleft, bottomright;
        ARGB top, bottom;
        float x_offset;

        leftxf = floorf(point->X);
        leftx = (INT)leftxf;
        rightx = (INT)ceilf(point->X);
        topyf = floorf(point->Y);
        topy = (INT)topyf;
        bottomy = (INT)ceilf(point->Y);

        if (leftx == rightx && topy == bottomy)
            return sample_bitmap_pixel(src_rect, bits, width, height,
                leftx, topy, attributes);

        topleft = sample_bitmap_pixel(src_rect, bits, width, height,
            leftx, topy, attributes);
        topright = sample_bitmap_pixel(src_rect, bits, width, height,
            rightx, topy, attributes);
        bottomleft = sample_bitmap_pixel(src_rect, bits, width, height,
            leftx, bottomy, attributes);
        bottomright = sample_bitmap_pixel(src_rect, bits, width, height,
            rightx, bottomy, attributes);

        x_offset = point->X - leftxf;
        top = blend_colors(topleft, topright, x_offset);
        bottom = blend_colors(bottomleft, bottomright, x_offset);

        return blend_colors(top, bottom, point->Y - topyf);
    }
    case InterpolationModeNearestNeighbor:
        return sample_bitmap_pixel(src_rect, bits, width, height,
            gdip_round(point->X), gdip_round(point->Y), attributes);
    }
}

static REAL intersect_line_scanline(const GpPointF *p1, const GpPointF *p2, REAL y)
{
    return (p1->X - p2->X) * (p2->Y - y) / (p2->Y - p1->Y) + p2->X;
}

static INT brush_can_fill_path(GpBrush *brush)
{
    switch (brush->bt)
    {
    case BrushTypeSolidColor:
        return 1;
    case BrushTypeHatchFill:
    {
        GpHatch *hatch = (GpHatch*)brush;
        return ((hatch->forecol & 0xff000000) == 0xff000000) &&
               ((hatch->backcol & 0xff000000) == 0xff000000);
    }
    case BrushTypeLinearGradient:
    case BrushTypeTextureFill:
    /* Gdi32 isn't much help with these, so we should use brush_fill_pixels instead. */
    default:
        return 0;
    }
}

static void brush_fill_path(GpGraphics *graphics, GpBrush* brush)
{
    switch (brush->bt)
    {
    case BrushTypeSolidColor:
    {
        GpSolidFill *fill = (GpSolidFill*)brush;
        HBITMAP bmp = ARGB2BMP(fill->color);

        if (bmp)
        {
            RECT rc;
            /* partially transparent fill */

            SelectClipPath(graphics->hdc, RGN_AND);
            if (GetClipBox(graphics->hdc, &rc) != NULLREGION)
            {
                HDC hdc = CreateCompatibleDC(NULL);

                if (!hdc) break;

                SelectObject(hdc, bmp);
                gdi_alpha_blend(graphics, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                                hdc, 0, 0, 1, 1);
                DeleteDC(hdc);
            }

            DeleteObject(bmp);
            break;
        }
        /* else fall through */
    }
    default:
    {
        HBRUSH gdibrush, old_brush;

        gdibrush = create_gdi_brush(brush);
        if (!gdibrush) return;

        old_brush = SelectObject(graphics->hdc, gdibrush);
        FillPath(graphics->hdc);
        SelectObject(graphics->hdc, old_brush);
        DeleteObject(gdibrush);
        break;
    }
    }
}

static INT brush_can_fill_pixels(GpBrush *brush)
{
    switch (brush->bt)
    {
    case BrushTypeSolidColor:
    case BrushTypeHatchFill:
    case BrushTypeLinearGradient:
    case BrushTypeTextureFill:
    case BrushTypePathGradient:
        return 1;
    default:
        return 0;
    }
}

static GpStatus brush_fill_pixels(GpGraphics *graphics, GpBrush *brush,
    DWORD *argb_pixels, GpRect *fill_area, UINT cdwStride)
{
    switch (brush->bt)
    {
    case BrushTypeSolidColor:
    {
        int x, y;
        GpSolidFill *fill = (GpSolidFill*)brush;
        for (x=0; x<fill_area->Width; x++)
            for (y=0; y<fill_area->Height; y++)
                argb_pixels[x + y*cdwStride] = fill->color;
        return Ok;
    }
    case BrushTypeHatchFill:
    {
        int x, y;
        GpHatch *fill = (GpHatch*)brush;
        const char *hatch_data;

        if (get_hatch_data(fill->hatchstyle, &hatch_data) != Ok)
            return NotImplemented;

        for (x=0; x<fill_area->Width; x++)
            for (y=0; y<fill_area->Height; y++)
            {
                int hx, hy;

                /* FIXME: Account for the rendering origin */
                hx = (x + fill_area->X) % 8;
                hy = (y + fill_area->Y) % 8;

                if ((hatch_data[7-hy] & (0x80 >> hx)) != 0)
                    argb_pixels[x + y*cdwStride] = fill->forecol;
                else
                    argb_pixels[x + y*cdwStride] = fill->backcol;
            }

        return Ok;
    }
    case BrushTypeLinearGradient:
    {
        GpLineGradient *fill = (GpLineGradient*)brush;
        GpPointF draw_points[3], line_points[3];
        GpStatus stat;
        static const GpRectF box_1 = { 0.0, 0.0, 1.0, 1.0 };
        GpMatrix *world_to_gradient; /* FIXME: Store this in the brush? */
        int x, y;

        draw_points[0].X = fill_area->X;
        draw_points[0].Y = fill_area->Y;
        draw_points[1].X = fill_area->X+1;
        draw_points[1].Y = fill_area->Y;
        draw_points[2].X = fill_area->X;
        draw_points[2].Y = fill_area->Y+1;

        /* Transform the points to a co-ordinate space where X is the point's
         * position in the gradient, 0.0 being the start point and 1.0 the
         * end point. */
        stat = GdipTransformPoints(graphics, CoordinateSpaceWorld,
            CoordinateSpaceDevice, draw_points, 3);

        if (stat == Ok)
        {
            line_points[0] = fill->startpoint;
            line_points[1] = fill->endpoint;
            line_points[2].X = fill->startpoint.X + (fill->startpoint.Y - fill->endpoint.Y);
            line_points[2].Y = fill->startpoint.Y + (fill->endpoint.X - fill->startpoint.X);

            stat = GdipCreateMatrix3(&box_1, line_points, &world_to_gradient);
        }

        if (stat == Ok)
        {
            stat = GdipInvertMatrix(world_to_gradient);

            if (stat == Ok)
                stat = GdipTransformMatrixPoints(world_to_gradient, draw_points, 3);

            GdipDeleteMatrix(world_to_gradient);
        }

        if (stat == Ok)
        {
            REAL x_delta = draw_points[1].X - draw_points[0].X;
            REAL y_delta = draw_points[2].X - draw_points[0].X;

            for (y=0; y<fill_area->Height; y++)
            {
                for (x=0; x<fill_area->Width; x++)
                {
                    REAL pos = draw_points[0].X + x * x_delta + y * y_delta;

                    argb_pixels[x + y*cdwStride] = blend_line_gradient(fill, pos);
                }
            }
        }

        return stat;
    }
    case BrushTypeTextureFill:
    {
        GpTexture *fill = (GpTexture*)brush;
        GpPointF draw_points[3];
        GpStatus stat;
        GpMatrix *world_to_texture;
        int x, y;
        GpBitmap *bitmap;
        int src_stride;
        GpRect src_area;

        if (fill->image->type != ImageTypeBitmap)
        {
            FIXME("metafile texture brushes not implemented\n");
            return NotImplemented;
        }

        bitmap = (GpBitmap*)fill->image;
        src_stride = sizeof(ARGB) * bitmap->width;

        src_area.X = src_area.Y = 0;
        src_area.Width = bitmap->width;
        src_area.Height = bitmap->height;

        draw_points[0].X = fill_area->X;
        draw_points[0].Y = fill_area->Y;
        draw_points[1].X = fill_area->X+1;
        draw_points[1].Y = fill_area->Y;
        draw_points[2].X = fill_area->X;
        draw_points[2].Y = fill_area->Y+1;

        /* Transform the points to the co-ordinate space of the bitmap. */
        stat = GdipTransformPoints(graphics, CoordinateSpaceWorld,
            CoordinateSpaceDevice, draw_points, 3);

        if (stat == Ok)
        {
            stat = GdipCloneMatrix(fill->transform, &world_to_texture);
        }

        if (stat == Ok)
        {
            stat = GdipInvertMatrix(world_to_texture);

            if (stat == Ok)
                stat = GdipTransformMatrixPoints(world_to_texture, draw_points, 3);

            GdipDeleteMatrix(world_to_texture);
        }

        if (stat == Ok && !fill->bitmap_bits)
        {
            BitmapData lockeddata;

            fill->bitmap_bits = GdipAlloc(sizeof(ARGB) * bitmap->width * bitmap->height);
            if (!fill->bitmap_bits)
                stat = OutOfMemory;

            if (stat == Ok)
            {
                lockeddata.Width = bitmap->width;
                lockeddata.Height = bitmap->height;
                lockeddata.Stride = src_stride;
                lockeddata.PixelFormat = PixelFormat32bppARGB;
                lockeddata.Scan0 = fill->bitmap_bits;

                stat = GdipBitmapLockBits(bitmap, &src_area, ImageLockModeRead|ImageLockModeUserInputBuf,
                    PixelFormat32bppARGB, &lockeddata);
            }

            if (stat == Ok)
                stat = GdipBitmapUnlockBits(bitmap, &lockeddata);

            if (stat == Ok)
                apply_image_attributes(fill->imageattributes, fill->bitmap_bits,
                    bitmap->width, bitmap->height,
                    src_stride, ColorAdjustTypeBitmap);

            if (stat != Ok)
            {
                GdipFree(fill->bitmap_bits);
                fill->bitmap_bits = NULL;
            }
        }

        if (stat == Ok)
        {
            REAL x_dx = draw_points[1].X - draw_points[0].X;
            REAL x_dy = draw_points[1].Y - draw_points[0].Y;
            REAL y_dx = draw_points[2].X - draw_points[0].X;
            REAL y_dy = draw_points[2].Y - draw_points[0].Y;

            for (y=0; y<fill_area->Height; y++)
            {
                for (x=0; x<fill_area->Width; x++)
                {
                    GpPointF point;
                    point.X = draw_points[0].X + x * x_dx + y * y_dx;
                    point.Y = draw_points[0].Y + y * x_dy + y * y_dy;

                    argb_pixels[x + y*cdwStride] = resample_bitmap_pixel(
                        &src_area, fill->bitmap_bits, bitmap->width, bitmap->height,
                        &point, fill->imageattributes, graphics->interpolation);
                }
            }
        }

        return stat;
    }
    case BrushTypePathGradient:
    {
        GpPathGradient *fill = (GpPathGradient*)brush;
        GpPath *flat_path;
        GpMatrix *world_to_device;
        GpStatus stat;
        int i, figure_start=0;
        GpPointF start_point, end_point, center_point;
        BYTE type;
        REAL min_yf, max_yf, line1_xf, line2_xf;
        INT min_y, max_y, min_x, max_x;
        INT x, y;
        ARGB outer_color;
        static int transform_fixme_once;

        if (fill->focus.X != 0.0 || fill->focus.Y != 0.0)
        {
            static int once;
            if (!once++)
                FIXME("path gradient focus not implemented\n");
        }

        if (fill->gamma)
        {
            static int once;
            if (!once++)
                FIXME("path gradient gamma correction not implemented\n");
        }

        if (fill->blendcount)
        {
            static int once;
            if (!once++)
                FIXME("path gradient blend not implemented\n");
        }

        if (fill->pblendcount)
        {
            static int once;
            if (!once++)
                FIXME("path gradient preset blend not implemented\n");
        }

        if (!transform_fixme_once)
        {
            BOOL is_identity=TRUE;
            GdipIsMatrixIdentity(fill->transform, &is_identity);
            if (!is_identity)
            {
                FIXME("path gradient transform not implemented\n");
                transform_fixme_once = 1;
            }
        }

        stat = GdipClonePath(fill->path, &flat_path);

        if (stat != Ok)
            return stat;

        stat = get_graphics_transform(graphics, CoordinateSpaceDevice,
            CoordinateSpaceWorld, &world_to_device);
        if (stat == Ok)
        {
            stat = GdipTransformPath(flat_path, world_to_device);

            if (stat == Ok)
            {
                center_point = fill->center;
                stat = GdipTransformMatrixPoints(world_to_device, &center_point, 1);
            }

            if (stat == Ok)
                stat = GdipFlattenPath(flat_path, NULL, 0.5);

            GdipDeleteMatrix(world_to_device);
        }

        if (stat != Ok)
        {
            GdipDeletePath(flat_path);
            return stat;
        }

        for (i=0; i<flat_path->pathdata.Count; i++)
        {
            int start_center_line=0, end_center_line=0;
            int seen_start=0, seen_end=0, seen_center=0;
            REAL center_distance;
            ARGB start_color, end_color;
            REAL dy, dx;

            type = flat_path->pathdata.Types[i];

            if ((type&PathPointTypePathTypeMask) == PathPointTypeStart)
                figure_start = i;

            start_point = flat_path->pathdata.Points[i];

            start_color = fill->surroundcolors[min(i, fill->surroundcolorcount-1)];

            if ((type&PathPointTypeCloseSubpath) == PathPointTypeCloseSubpath || i+1 >= flat_path->pathdata.Count)
            {
                end_point = flat_path->pathdata.Points[figure_start];
                end_color = fill->surroundcolors[min(figure_start, fill->surroundcolorcount-1)];
            }
            else if ((flat_path->pathdata.Types[i+1] & PathPointTypePathTypeMask) == PathPointTypeLine)
            {
                end_point = flat_path->pathdata.Points[i+1];
                end_color = fill->surroundcolors[min(i+1, fill->surroundcolorcount-1)];
            }
            else
                continue;

            outer_color = start_color;

            min_yf = center_point.Y;
            if (min_yf > start_point.Y) min_yf = start_point.Y;
            if (min_yf > end_point.Y) min_yf = end_point.Y;

            if (min_yf < fill_area->Y)
                min_y = fill_area->Y;
            else
                min_y = (INT)ceil(min_yf);

            max_yf = center_point.Y;
            if (max_yf < start_point.Y) max_yf = start_point.Y;
            if (max_yf < end_point.Y) max_yf = end_point.Y;

            if (max_yf > fill_area->Y + fill_area->Height)
                max_y = fill_area->Y + fill_area->Height;
            else
                max_y = (INT)ceil(max_yf);

            dy = end_point.Y - start_point.Y;
            dx = end_point.X - start_point.X;

            /* This is proportional to the distance from start-end line to center point. */
            center_distance = dy * (start_point.X - center_point.X) +
                dx * (center_point.Y - start_point.Y);

            for (y=min_y; y<max_y; y++)
            {
                REAL yf = (REAL)y;

                if (!seen_start && yf >= start_point.Y)
                {
                    seen_start = 1;
                    start_center_line ^= 1;
                }
                if (!seen_end && yf >= end_point.Y)
                {
                    seen_end = 1;
                    end_center_line ^= 1;
                }
                if (!seen_center && yf >= center_point.Y)
                {
                    seen_center = 1;
                    start_center_line ^= 1;
                    end_center_line ^= 1;
                }

                if (start_center_line)
                    line1_xf = intersect_line_scanline(&start_point, &center_point, yf);
                else
                    line1_xf = intersect_line_scanline(&start_point, &end_point, yf);

                if (end_center_line)
                    line2_xf = intersect_line_scanline(&end_point, &center_point, yf);
                else
                    line2_xf = intersect_line_scanline(&start_point, &end_point, yf);

                if (line1_xf < line2_xf)
                {
                    min_x = (INT)ceil(line1_xf);
                    max_x = (INT)ceil(line2_xf);
                }
                else
                {
                    min_x = (INT)ceil(line2_xf);
                    max_x = (INT)ceil(line1_xf);
                }

                if (min_x < fill_area->X)
                    min_x = fill_area->X;
                if (max_x > fill_area->X + fill_area->Width)
                    max_x = fill_area->X + fill_area->Width;

                for (x=min_x; x<max_x; x++)
                {
                    REAL xf = (REAL)x;
                    REAL distance;

                    if (start_color != end_color)
                    {
                        REAL blend_amount, pdy, pdx;
                        pdy = yf - center_point.Y;
                        pdx = xf - center_point.X;
                        blend_amount = ( (center_point.Y - start_point.Y) * pdx + (start_point.X - center_point.X) * pdy ) / ( dy * pdx - dx * pdy );
                        outer_color = blend_colors(start_color, end_color, blend_amount);
                    }

                    distance = (end_point.Y - start_point.Y) * (start_point.X - xf) +
                        (end_point.X - start_point.X) * (yf - start_point.Y);

                    distance = distance / center_distance;

                    argb_pixels[(x-fill_area->X) + (y-fill_area->Y)*cdwStride] =
                        blend_colors(outer_color, fill->centercolor, distance);
                }
            }
        }

        GdipDeletePath(flat_path);
        return stat;
    }
    default:
        return NotImplemented;
    }
}

/* GdipDrawPie/GdipFillPie helper function */
static void draw_pie(GpGraphics *graphics, REAL x, REAL y, REAL width,
    REAL height, REAL startAngle, REAL sweepAngle)
{
    GpPointF ptf[4];
    POINT pti[4];

    ptf[0].X = x;
    ptf[0].Y = y;
    ptf[1].X = x + width;
    ptf[1].Y = y + height;

    deg2xy(startAngle+sweepAngle, x + width / 2.0, y + width / 2.0, &ptf[2].X, &ptf[2].Y);
    deg2xy(startAngle, x + width / 2.0, y + width / 2.0, &ptf[3].X, &ptf[3].Y);

    transform_and_round_points(graphics, pti, ptf, 4);

    Pie(graphics->hdc, pti[0].x, pti[0].y, pti[1].x, pti[1].y, pti[2].x,
        pti[2].y, pti[3].x, pti[3].y);
}

/* Draws the linecap the specified color and size on the hdc.  The linecap is in
 * direction of the line from x1, y1 to x2, y2 and is anchored on x2, y2. Probably
 * should not be called on an hdc that has a path you care about. */
static void draw_cap(GpGraphics *graphics, COLORREF color, GpLineCap cap, REAL size,
    const GpCustomLineCap *custom, REAL x1, REAL y1, REAL x2, REAL y2)
{
    HGDIOBJ oldbrush = NULL, oldpen = NULL;
    GpMatrix *matrix = NULL;
    HBRUSH brush = NULL;
    HPEN pen = NULL;
    PointF ptf[4], *custptf = NULL;
    POINT pt[4], *custpt = NULL;
    BYTE *tp = NULL;
    REAL theta, dsmall, dbig, dx, dy = 0.0;
    INT i, count;
    LOGBRUSH lb;
    BOOL customstroke;

    if((x1 == x2) && (y1 == y2))
        return;

    theta = gdiplus_atan2(y2 - y1, x2 - x1);

    customstroke = (cap == LineCapCustom) && custom && (!custom->fill);
    if(!customstroke){
        brush = CreateSolidBrush(color);
        lb.lbStyle = BS_SOLID;
        lb.lbColor = color;
        lb.lbHatch = 0;
        pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_FLAT |
                           PS_JOIN_MITER, 1, &lb, 0,
                           NULL);
        oldbrush = SelectObject(graphics->hdc, brush);
        oldpen = SelectObject(graphics->hdc, pen);
    }

    switch(cap){
        case LineCapFlat:
            break;
        case LineCapSquare:
        case LineCapSquareAnchor:
        case LineCapDiamondAnchor:
            size = size * (cap & LineCapNoAnchor ? ANCHOR_WIDTH : 1.0) / 2.0;
            if(cap == LineCapDiamondAnchor){
                dsmall = cos(theta + M_PI_2) * size;
                dbig = sin(theta + M_PI_2) * size;
            }
            else{
                dsmall = cos(theta + M_PI_4) * size;
                dbig = sin(theta + M_PI_4) * size;
            }

            ptf[0].X = x2 - dsmall;
            ptf[1].X = x2 + dbig;

            ptf[0].Y = y2 - dbig;
            ptf[3].Y = y2 + dsmall;

            ptf[1].Y = y2 - dsmall;
            ptf[2].Y = y2 + dbig;

            ptf[3].X = x2 - dbig;
            ptf[2].X = x2 + dsmall;

            transform_and_round_points(graphics, pt, ptf, 4);
            Polygon(graphics->hdc, pt, 4);

            break;
        case LineCapArrowAnchor:
            size = size * 4.0 / sqrt(3.0);

            dx = cos(M_PI / 6.0 + theta) * size;
            dy = sin(M_PI / 6.0 + theta) * size;

            ptf[0].X = x2 - dx;
            ptf[0].Y = y2 - dy;

            dx = cos(- M_PI / 6.0 + theta) * size;
            dy = sin(- M_PI / 6.0 + theta) * size;

            ptf[1].X = x2 - dx;
            ptf[1].Y = y2 - dy;

            ptf[2].X = x2;
            ptf[2].Y = y2;

            transform_and_round_points(graphics, pt, ptf, 3);
            Polygon(graphics->hdc, pt, 3);

            break;
        case LineCapRoundAnchor:
            dx = dy = ANCHOR_WIDTH * size / 2.0;

            ptf[0].X = x2 - dx;
            ptf[0].Y = y2 - dy;
            ptf[1].X = x2 + dx;
            ptf[1].Y = y2 + dy;

            transform_and_round_points(graphics, pt, ptf, 2);
            Ellipse(graphics->hdc, pt[0].x, pt[0].y, pt[1].x, pt[1].y);

            break;
        case LineCapTriangle:
            size = size / 2.0;
            dx = cos(M_PI_2 + theta) * size;
            dy = sin(M_PI_2 + theta) * size;

            ptf[0].X = x2 - dx;
            ptf[0].Y = y2 - dy;
            ptf[1].X = x2 + dx;
            ptf[1].Y = y2 + dy;

            dx = cos(theta) * size;
            dy = sin(theta) * size;

            ptf[2].X = x2 + dx;
            ptf[2].Y = y2 + dy;

            transform_and_round_points(graphics, pt, ptf, 3);
            Polygon(graphics->hdc, pt, 3);

            break;
        case LineCapRound:
            dx = dy = size / 2.0;

            ptf[0].X = x2 - dx;
            ptf[0].Y = y2 - dy;
            ptf[1].X = x2 + dx;
            ptf[1].Y = y2 + dy;

            dx = -cos(M_PI_2 + theta) * size;
            dy = -sin(M_PI_2 + theta) * size;

            ptf[2].X = x2 - dx;
            ptf[2].Y = y2 - dy;
            ptf[3].X = x2 + dx;
            ptf[3].Y = y2 + dy;

            transform_and_round_points(graphics, pt, ptf, 4);
            Pie(graphics->hdc, pt[0].x, pt[0].y, pt[1].x, pt[1].y, pt[2].x,
                pt[2].y, pt[3].x, pt[3].y);

            break;
        case LineCapCustom:
            if(!custom)
                break;

            count = custom->pathdata.Count;
            custptf = GdipAlloc(count * sizeof(PointF));
            custpt = GdipAlloc(count * sizeof(POINT));
            tp = GdipAlloc(count);

            if(!custptf || !custpt || !tp || (GdipCreateMatrix(&matrix) != Ok))
                goto custend;

            memcpy(custptf, custom->pathdata.Points, count * sizeof(PointF));

            GdipScaleMatrix(matrix, size, size, MatrixOrderAppend);
            GdipRotateMatrix(matrix, (180.0 / M_PI) * (theta - M_PI_2),
                             MatrixOrderAppend);
            GdipTranslateMatrix(matrix, x2, y2, MatrixOrderAppend);
            GdipTransformMatrixPoints(matrix, custptf, count);

            transform_and_round_points(graphics, custpt, custptf, count);

            for(i = 0; i < count; i++)
                tp[i] = convert_path_point_type(custom->pathdata.Types[i]);

            if(custom->fill){
                BeginPath(graphics->hdc);
                PolyDraw(graphics->hdc, custpt, tp, count);
                EndPath(graphics->hdc);
                StrokeAndFillPath(graphics->hdc);
            }
            else
                PolyDraw(graphics->hdc, custpt, tp, count);

custend:
            GdipFree(custptf);
            GdipFree(custpt);
            GdipFree(tp);
            GdipDeleteMatrix(matrix);
            break;
        default:
            break;
    }

    if(!customstroke){
        SelectObject(graphics->hdc, oldbrush);
        SelectObject(graphics->hdc, oldpen);
        DeleteObject(brush);
        DeleteObject(pen);
    }
}

/* Shortens the line by the given percent by changing x2, y2.
 * If percent is > 1.0 then the line will change direction.
 * If percent is negative it can lengthen the line. */
static void shorten_line_percent(REAL x1, REAL  y1, REAL *x2, REAL *y2, REAL percent)
{
    REAL dist, theta, dx, dy;

    if((y1 == *y2) && (x1 == *x2))
        return;

    dist = sqrt((*x2 - x1) * (*x2 - x1) + (*y2 - y1) * (*y2 - y1)) * -percent;
    theta = gdiplus_atan2((*y2 - y1), (*x2 - x1));
    dx = cos(theta) * dist;
    dy = sin(theta) * dist;

    *x2 = *x2 + dx;
    *y2 = *y2 + dy;
}

/* Shortens the line by the given amount by changing x2, y2.
 * If the amount is greater than the distance, the line will become length 0.
 * If the amount is negative, it can lengthen the line. */
static void shorten_line_amt(REAL x1, REAL y1, REAL *x2, REAL *y2, REAL amt)
{
    REAL dx, dy, percent;

    dx = *x2 - x1;
    dy = *y2 - y1;
    if(dx == 0 && dy == 0)
        return;

    percent = amt / sqrt(dx * dx + dy * dy);
    if(percent >= 1.0){
        *x2 = x1;
        *y2 = y1;
        return;
    }

    shorten_line_percent(x1, y1, x2, y2, percent);
}

/* Draws lines between the given points, and if caps is true then draws an endcap
 * at the end of the last line. */
static GpStatus draw_polyline(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF * pt, INT count, BOOL caps)
{
    POINT *pti = NULL;
    GpPointF *ptcopy = NULL;
    GpStatus status = GenericError;

    if(!count)
        return Ok;

    pti = GdipAlloc(count * sizeof(POINT));
    ptcopy = GdipAlloc(count * sizeof(GpPointF));

    if(!pti || !ptcopy){
        status = OutOfMemory;
        goto end;
    }

    memcpy(ptcopy, pt, count * sizeof(GpPointF));

    if(caps){
        if(pen->endcap == LineCapArrowAnchor)
            shorten_line_amt(ptcopy[count-2].X, ptcopy[count-2].Y,
                             &ptcopy[count-1].X, &ptcopy[count-1].Y, pen->width);
        else if((pen->endcap == LineCapCustom) && pen->customend)
            shorten_line_amt(ptcopy[count-2].X, ptcopy[count-2].Y,
                             &ptcopy[count-1].X, &ptcopy[count-1].Y,
                             pen->customend->inset * pen->width);

        if(pen->startcap == LineCapArrowAnchor)
            shorten_line_amt(ptcopy[1].X, ptcopy[1].Y,
                             &ptcopy[0].X, &ptcopy[0].Y, pen->width);
        else if((pen->startcap == LineCapCustom) && pen->customstart)
            shorten_line_amt(ptcopy[1].X, ptcopy[1].Y,
                             &ptcopy[0].X, &ptcopy[0].Y,
                             pen->customstart->inset * pen->width);

        draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->endcap, pen->width, pen->customend,
                 pt[count - 2].X, pt[count - 2].Y, pt[count - 1].X, pt[count - 1].Y);
        draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->startcap, pen->width, pen->customstart,
                         pt[1].X, pt[1].Y, pt[0].X, pt[0].Y);
    }

    transform_and_round_points(graphics, pti, ptcopy, count);

    if(Polyline(graphics->hdc, pti, count))
        status = Ok;

end:
    GdipFree(pti);
    GdipFree(ptcopy);

    return status;
}

/* Conducts a linear search to find the bezier points that will back off
 * the endpoint of the curve by a distance of amt. Linear search works
 * better than binary in this case because there are multiple solutions,
 * and binary searches often find a bad one. I don't think this is what
 * Windows does but short of rendering the bezier without GDI's help it's
 * the best we can do. If rev then work from the start of the passed points
 * instead of the end. */
static void shorten_bezier_amt(GpPointF * pt, REAL amt, BOOL rev)
{
    GpPointF origpt[4];
    REAL percent = 0.00, dx, dy, origx, origy, diff = -1.0;
    INT i, first = 0, second = 1, third = 2, fourth = 3;

    if(rev){
        first = 3;
        second = 2;
        third = 1;
        fourth = 0;
    }

    origx = pt[fourth].X;
    origy = pt[fourth].Y;
    memcpy(origpt, pt, sizeof(GpPointF) * 4);

    for(i = 0; (i < MAX_ITERS) && (diff < amt); i++){
        /* reset bezier points to original values */
        memcpy(pt, origpt, sizeof(GpPointF) * 4);
        /* Perform magic on bezier points. Order is important here.*/
        shorten_line_percent(pt[third].X, pt[third].Y, &pt[fourth].X, &pt[fourth].Y, percent);
        shorten_line_percent(pt[second].X, pt[second].Y, &pt[third].X, &pt[third].Y, percent);
        shorten_line_percent(pt[third].X, pt[third].Y, &pt[fourth].X, &pt[fourth].Y, percent);
        shorten_line_percent(pt[first].X, pt[first].Y, &pt[second].X, &pt[second].Y, percent);
        shorten_line_percent(pt[second].X, pt[second].Y, &pt[third].X, &pt[third].Y, percent);
        shorten_line_percent(pt[third].X, pt[third].Y, &pt[fourth].X, &pt[fourth].Y, percent);

        dx = pt[fourth].X - origx;
        dy = pt[fourth].Y - origy;

        diff = sqrt(dx * dx + dy * dy);
        percent += 0.0005 * amt;
    }
}

/* Draws bezier curves between given points, and if caps is true then draws an
 * endcap at the end of the last line. */
static GpStatus draw_polybezier(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF * pt, INT count, BOOL caps)
{
    POINT *pti;
    GpPointF *ptcopy;
    GpStatus status = GenericError;

    if(!count)
        return Ok;

    pti = GdipAlloc(count * sizeof(POINT));
    ptcopy = GdipAlloc(count * sizeof(GpPointF));

    if(!pti || !ptcopy){
        status = OutOfMemory;
        goto end;
    }

    memcpy(ptcopy, pt, count * sizeof(GpPointF));

    if(caps){
        if(pen->endcap == LineCapArrowAnchor)
            shorten_bezier_amt(&ptcopy[count-4], pen->width, FALSE);
        else if((pen->endcap == LineCapCustom) && pen->customend)
            shorten_bezier_amt(&ptcopy[count-4], pen->width * pen->customend->inset,
                               FALSE);

        if(pen->startcap == LineCapArrowAnchor)
            shorten_bezier_amt(ptcopy, pen->width, TRUE);
        else if((pen->startcap == LineCapCustom) && pen->customstart)
            shorten_bezier_amt(ptcopy, pen->width * pen->customstart->inset, TRUE);

        /* the direction of the line cap is parallel to the direction at the
         * end of the bezier (which, if it has been shortened, is not the same
         * as the direction from pt[count-2] to pt[count-1]) */
        draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->endcap, pen->width, pen->customend,
            pt[count - 1].X - (ptcopy[count - 1].X - ptcopy[count - 2].X),
            pt[count - 1].Y - (ptcopy[count - 1].Y - ptcopy[count - 2].Y),
            pt[count - 1].X, pt[count - 1].Y);

        draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->startcap, pen->width, pen->customstart,
            pt[0].X - (ptcopy[0].X - ptcopy[1].X),
            pt[0].Y - (ptcopy[0].Y - ptcopy[1].Y), pt[0].X, pt[0].Y);
    }

    transform_and_round_points(graphics, pti, ptcopy, count);

    PolyBezier(graphics->hdc, pti, count);

    status = Ok;

end:
    GdipFree(pti);
    GdipFree(ptcopy);

    return status;
}

/* Draws a combination of bezier curves and lines between points. */
static GpStatus draw_poly(GpGraphics *graphics, GpPen *pen, GDIPCONST GpPointF * pt,
    GDIPCONST BYTE * types, INT count, BOOL caps)
{
    POINT *pti = GdipAlloc(count * sizeof(POINT));
    BYTE *tp = GdipAlloc(count);
    GpPointF *ptcopy = GdipAlloc(count * sizeof(GpPointF));
    INT i, j;
    GpStatus status = GenericError;

    if(!count){
        status = Ok;
        goto end;
    }
    if(!pti || !tp || !ptcopy){
        status = OutOfMemory;
        goto end;
    }

    for(i = 1; i < count; i++){
        if((types[i] & PathPointTypePathTypeMask) == PathPointTypeBezier){
            if((i + 2 >= count) || !(types[i + 1] & PathPointTypeBezier)
                || !(types[i + 1] & PathPointTypeBezier)){
                ERR("Bad bezier points\n");
                goto end;
            }
            i += 2;
        }
    }

    memcpy(ptcopy, pt, count * sizeof(GpPointF));

    /* If we are drawing caps, go through the points and adjust them accordingly,
     * and draw the caps. */
    if(caps){
        switch(types[count - 1] & PathPointTypePathTypeMask){
            case PathPointTypeBezier:
                if(pen->endcap == LineCapArrowAnchor)
                    shorten_bezier_amt(&ptcopy[count - 4], pen->width, FALSE);
                else if((pen->endcap == LineCapCustom) && pen->customend)
                    shorten_bezier_amt(&ptcopy[count - 4],
                                       pen->width * pen->customend->inset, FALSE);

                draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->endcap, pen->width, pen->customend,
                    pt[count - 1].X - (ptcopy[count - 1].X - ptcopy[count - 2].X),
                    pt[count - 1].Y - (ptcopy[count - 1].Y - ptcopy[count - 2].Y),
                    pt[count - 1].X, pt[count - 1].Y);

                break;
            case PathPointTypeLine:
                if(pen->endcap == LineCapArrowAnchor)
                    shorten_line_amt(ptcopy[count - 2].X, ptcopy[count - 2].Y,
                                     &ptcopy[count - 1].X, &ptcopy[count - 1].Y,
                                     pen->width);
                else if((pen->endcap == LineCapCustom) && pen->customend)
                    shorten_line_amt(ptcopy[count - 2].X, ptcopy[count - 2].Y,
                                     &ptcopy[count - 1].X, &ptcopy[count - 1].Y,
                                     pen->customend->inset * pen->width);

                draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->endcap, pen->width, pen->customend,
                         pt[count - 2].X, pt[count - 2].Y, pt[count - 1].X,
                         pt[count - 1].Y);

                break;
            default:
                ERR("Bad path last point\n");
                goto end;
        }

        /* Find start of points */
        for(j = 1; j < count && ((types[j] & PathPointTypePathTypeMask)
            == PathPointTypeStart); j++);

        switch(types[j] & PathPointTypePathTypeMask){
            case PathPointTypeBezier:
                if(pen->startcap == LineCapArrowAnchor)
                    shorten_bezier_amt(&ptcopy[j - 1], pen->width, TRUE);
                else if((pen->startcap == LineCapCustom) && pen->customstart)
                    shorten_bezier_amt(&ptcopy[j - 1],
                                       pen->width * pen->customstart->inset, TRUE);

                draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->startcap, pen->width, pen->customstart,
                    pt[j - 1].X - (ptcopy[j - 1].X - ptcopy[j].X),
                    pt[j - 1].Y - (ptcopy[j - 1].Y - ptcopy[j].Y),
                    pt[j - 1].X, pt[j - 1].Y);

                break;
            case PathPointTypeLine:
                if(pen->startcap == LineCapArrowAnchor)
                    shorten_line_amt(ptcopy[j].X, ptcopy[j].Y,
                                     &ptcopy[j - 1].X, &ptcopy[j - 1].Y,
                                     pen->width);
                else if((pen->startcap == LineCapCustom) && pen->customstart)
                    shorten_line_amt(ptcopy[j].X, ptcopy[j].Y,
                                     &ptcopy[j - 1].X, &ptcopy[j - 1].Y,
                                     pen->customstart->inset * pen->width);

                draw_cap(graphics, get_gdi_brush_color(pen->brush), pen->startcap, pen->width, pen->customstart,
                         pt[j].X, pt[j].Y, pt[j - 1].X,
                         pt[j - 1].Y);

                break;
            default:
                ERR("Bad path points\n");
                goto end;
        }
    }

    transform_and_round_points(graphics, pti, ptcopy, count);

    for(i = 0; i < count; i++){
        tp[i] = convert_path_point_type(types[i]);
    }

    PolyDraw(graphics->hdc, pti, tp, count);

    status = Ok;

end:
    GdipFree(pti);
    GdipFree(ptcopy);
    GdipFree(tp);

    return status;
}

GpStatus trace_path(GpGraphics *graphics, GpPath *path)
{
    GpStatus result;

    BeginPath(graphics->hdc);
    result = draw_poly(graphics, NULL, path->pathdata.Points,
                       path->pathdata.Types, path->pathdata.Count, FALSE);
    EndPath(graphics->hdc);
    return result;
}

typedef struct _GraphicsContainerItem {
    struct list entry;
    GraphicsContainer contid;

    SmoothingMode smoothing;
    CompositingQuality compqual;
    InterpolationMode interpolation;
    CompositingMode compmode;
    TextRenderingHint texthint;
    REAL scale;
    GpUnit unit;
    PixelOffsetMode pixeloffset;
    UINT textcontrast;
    GpMatrix* worldtrans;
    GpRegion* clip;
    INT origin_x, origin_y;
} GraphicsContainerItem;

static GpStatus init_container(GraphicsContainerItem** container,
        GDIPCONST GpGraphics* graphics){
    GpStatus sts;

    *container = GdipAlloc(sizeof(GraphicsContainerItem));
    if(!(*container))
        return OutOfMemory;

    (*container)->contid = graphics->contid + 1;

    (*container)->smoothing = graphics->smoothing;
    (*container)->compqual = graphics->compqual;
    (*container)->interpolation = graphics->interpolation;
    (*container)->compmode = graphics->compmode;
    (*container)->texthint = graphics->texthint;
    (*container)->scale = graphics->scale;
    (*container)->unit = graphics->unit;
    (*container)->textcontrast = graphics->textcontrast;
    (*container)->pixeloffset = graphics->pixeloffset;
    (*container)->origin_x = graphics->origin_x;
    (*container)->origin_y = graphics->origin_y;

    sts = GdipCloneMatrix(graphics->worldtrans, &(*container)->worldtrans);
    if(sts != Ok){
        GdipFree(*container);
        *container = NULL;
        return sts;
    }

    sts = GdipCloneRegion(graphics->clip, &(*container)->clip);
    if(sts != Ok){
        GdipDeleteMatrix((*container)->worldtrans);
        GdipFree(*container);
        *container = NULL;
        return sts;
    }

    return Ok;
}

static void delete_container(GraphicsContainerItem* container){
    GdipDeleteMatrix(container->worldtrans);
    GdipDeleteRegion(container->clip);
    GdipFree(container);
}

static GpStatus restore_container(GpGraphics* graphics,
        GDIPCONST GraphicsContainerItem* container){
    GpStatus sts;
    GpMatrix *newTrans;
    GpRegion *newClip;

    sts = GdipCloneMatrix(container->worldtrans, &newTrans);
    if(sts != Ok)
        return sts;

    sts = GdipCloneRegion(container->clip, &newClip);
    if(sts != Ok){
        GdipDeleteMatrix(newTrans);
        return sts;
    }

    GdipDeleteMatrix(graphics->worldtrans);
    graphics->worldtrans = newTrans;

    GdipDeleteRegion(graphics->clip);
    graphics->clip = newClip;

    graphics->contid = container->contid - 1;

    graphics->smoothing = container->smoothing;
    graphics->compqual = container->compqual;
    graphics->interpolation = container->interpolation;
    graphics->compmode = container->compmode;
    graphics->texthint = container->texthint;
    graphics->scale = container->scale;
    graphics->unit = container->unit;
    graphics->textcontrast = container->textcontrast;
    graphics->pixeloffset = container->pixeloffset;
    graphics->origin_x = container->origin_x;
    graphics->origin_y = container->origin_y;

    return Ok;
}

static GpStatus get_graphics_bounds(GpGraphics* graphics, GpRectF* rect)
{
    RECT wnd_rect;
    GpStatus stat=Ok;
    GpUnit unit;

    if(graphics->hwnd) {
        if(!GetClientRect(graphics->hwnd, &wnd_rect))
            return GenericError;

        rect->X = wnd_rect.left;
        rect->Y = wnd_rect.top;
        rect->Width = wnd_rect.right - wnd_rect.left;
        rect->Height = wnd_rect.bottom - wnd_rect.top;
    }else if (graphics->image){
        stat = GdipGetImageBounds(graphics->image, rect, &unit);
        if (stat == Ok && unit != UnitPixel)
            FIXME("need to convert from unit %i\n", unit);
    }else{
        rect->X = 0;
        rect->Y = 0;
        rect->Width = GetDeviceCaps(graphics->hdc, HORZRES);
        rect->Height = GetDeviceCaps(graphics->hdc, VERTRES);
    }

    return stat;
}

/* on success, rgn will contain the region of the graphics object which
 * is visible after clipping has been applied */
static GpStatus get_visible_clip_region(GpGraphics *graphics, GpRegion *rgn)
{
    GpStatus stat;
    GpRectF rectf;
    GpRegion* tmp;

    if((stat = get_graphics_bounds(graphics, &rectf)) != Ok)
        return stat;

    if((stat = GdipCreateRegion(&tmp)) != Ok)
        return stat;

    if((stat = GdipCombineRegionRect(tmp, &rectf, CombineModeReplace)) != Ok)
        goto end;

    if((stat = GdipCombineRegionRegion(tmp, graphics->clip, CombineModeIntersect)) != Ok)
        goto end;

    stat = GdipCombineRegionRegion(rgn, tmp, CombineModeReplace);

end:
    GdipDeleteRegion(tmp);
    return stat;
}

static void get_font_hfont(GpGraphics *graphics, GDIPCONST GpFont *font, HFONT *hfont)
{
    HDC hdc = CreateCompatibleDC(0);
    GpPointF pt[3];
    REAL angle, rel_width, rel_height, font_height, font_to_pixel_scale;
    LOGFONTW lfw;
    HFONT unscaled_font;
    TEXTMETRICW textmet;

    font_to_pixel_scale = units_scale(UnitPoint, UnitPixel, font->family->dpi);

    if (font->unit == UnitPixel)
        font_height = font->emSize * font_to_pixel_scale;
    else
    {
        REAL unit_scale, res;

        res = (graphics->unit == UnitDisplay || graphics->unit == UnitPixel) ? graphics->xres : graphics->yres;
        unit_scale = units_scale(font->unit, graphics->unit, res);

        font_height = font->emSize * font_to_pixel_scale * unit_scale;
        if (graphics->unit != UnitDisplay)
            font_height /= graphics->scale;
    }

    pt[0].X = 0.0;
    pt[0].Y = 0.0;
    pt[1].X = 1.0;
    pt[1].Y = 0.0;
    pt[2].X = 0.0;
    pt[2].Y = 1.0;
    if (graphics)
        GdipTransformPoints(graphics, CoordinateSpaceDevice, CoordinateSpaceWorld, pt, 3);
    angle = -gdiplus_atan2((pt[1].Y - pt[0].Y), (pt[1].X - pt[0].X));
    rel_width = sqrt((pt[1].Y-pt[0].Y)*(pt[1].Y-pt[0].Y)+
                     (pt[1].X-pt[0].X)*(pt[1].X-pt[0].X));
    rel_height = sqrt((pt[2].Y-pt[0].Y)*(pt[2].Y-pt[0].Y)+
                      (pt[2].X-pt[0].X)*(pt[2].X-pt[0].X));

    get_log_fontW(font, graphics, &lfw);
    lfw.lfHeight = gdip_round(font_height * rel_height);
    unscaled_font = CreateFontIndirectW(&lfw);

    SelectObject(hdc, unscaled_font);
    GetTextMetricsW(hdc, &textmet);

    lfw.lfWidth = gdip_round(textmet.tmAveCharWidth * rel_width / rel_height);
    lfw.lfEscapement = lfw.lfOrientation = gdip_round((angle / M_PI) * 1800.0);

    *hfont = CreateFontIndirectW(&lfw);

    DeleteDC(hdc);
    DeleteObject(unscaled_font);
}

GpStatus WINGDIPAPI GdipCreateFromHDC(HDC hdc, GpGraphics **graphics)
{
    TRACE("(%p, %p)\n", hdc, graphics);

    return GdipCreateFromHDC2(hdc, NULL, graphics);
}

GpStatus WINGDIPAPI GdipCreateFromHDC2(HDC hdc, HANDLE hDevice, GpGraphics **graphics)
{
    GpStatus retval;

    TRACE("(%p, %p, %p)\n", hdc, hDevice, graphics);

    if(hDevice != NULL) {
        FIXME("Don't know how to handle parameter hDevice\n");
        return NotImplemented;
    }

    if(hdc == NULL)
        return OutOfMemory;

    if(graphics == NULL)
        return InvalidParameter;

    *graphics = GdipAlloc(sizeof(GpGraphics));
    if(!*graphics)  return OutOfMemory;

    if((retval = GdipCreateMatrix(&(*graphics)->worldtrans)) != Ok){
        GdipFree(*graphics);
        return retval;
    }

    if((retval = GdipCreateRegion(&(*graphics)->clip)) != Ok){
        GdipFree((*graphics)->worldtrans);
        GdipFree(*graphics);
        return retval;
    }

    (*graphics)->hdc = hdc;
    (*graphics)->hwnd = WindowFromDC(hdc);
    (*graphics)->owndc = FALSE;
    (*graphics)->smoothing = SmoothingModeDefault;
    (*graphics)->compqual = CompositingQualityDefault;
    (*graphics)->interpolation = InterpolationModeBilinear;
    (*graphics)->pixeloffset = PixelOffsetModeDefault;
    (*graphics)->compmode = CompositingModeSourceOver;
    (*graphics)->unit = UnitDisplay;
    (*graphics)->scale = 1.0;
    (*graphics)->xres = GetDeviceCaps(hdc, LOGPIXELSX);
    (*graphics)->yres = GetDeviceCaps(hdc, LOGPIXELSY);
    (*graphics)->busy = FALSE;
    (*graphics)->textcontrast = 4;
    list_init(&(*graphics)->containers);
    (*graphics)->contid = 0;

    TRACE("<-- %p\n", *graphics);

    return Ok;
}

GpStatus graphics_from_image(GpImage *image, GpGraphics **graphics)
{
    GpStatus retval;

    *graphics = GdipAlloc(sizeof(GpGraphics));
    if(!*graphics)  return OutOfMemory;

    if((retval = GdipCreateMatrix(&(*graphics)->worldtrans)) != Ok){
        GdipFree(*graphics);
        return retval;
    }

    if((retval = GdipCreateRegion(&(*graphics)->clip)) != Ok){
        GdipFree((*graphics)->worldtrans);
        GdipFree(*graphics);
        return retval;
    }

    (*graphics)->hdc = NULL;
    (*graphics)->hwnd = NULL;
    (*graphics)->owndc = FALSE;
    (*graphics)->image = image;
    (*graphics)->smoothing = SmoothingModeDefault;
    (*graphics)->compqual = CompositingQualityDefault;
    (*graphics)->interpolation = InterpolationModeBilinear;
    (*graphics)->pixeloffset = PixelOffsetModeDefault;
    (*graphics)->compmode = CompositingModeSourceOver;
    (*graphics)->unit = UnitDisplay;
    (*graphics)->scale = 1.0;
    (*graphics)->xres = image->xres;
    (*graphics)->yres = image->yres;
    (*graphics)->busy = FALSE;
    (*graphics)->textcontrast = 4;
    list_init(&(*graphics)->containers);
    (*graphics)->contid = 0;

    TRACE("<-- %p\n", *graphics);

    return Ok;
}

GpStatus WINGDIPAPI GdipCreateFromHWND(HWND hwnd, GpGraphics **graphics)
{
    GpStatus ret;
    HDC hdc;

    TRACE("(%p, %p)\n", hwnd, graphics);

    hdc = GetDC(hwnd);

    if((ret = GdipCreateFromHDC(hdc, graphics)) != Ok)
    {
        ReleaseDC(hwnd, hdc);
        return ret;
    }

    (*graphics)->hwnd = hwnd;
    (*graphics)->owndc = TRUE;

    return Ok;
}

/* FIXME: no icm handling */
GpStatus WINGDIPAPI GdipCreateFromHWNDICM(HWND hwnd, GpGraphics **graphics)
{
    TRACE("(%p, %p)\n", hwnd, graphics);

    return GdipCreateFromHWND(hwnd, graphics);
}

GpStatus WINGDIPAPI GdipCreateMetafileFromEmf(HENHMETAFILE hemf, BOOL delete,
    GpMetafile **metafile)
{
    IStream *stream = NULL;
    UINT read;
    ENHMETAHEADER *copy;
    GpStatus retval = Ok;

    TRACE("(%p,%i,%p)\n", hemf, delete, metafile);

    if(!hemf || !metafile)
        return InvalidParameter;

    read = GetEnhMetaFileBits(hemf, 0, NULL);
    copy = GdipAlloc(read);
    GetEnhMetaFileBits(hemf, read, (BYTE *)copy);

    if(CreateStreamOnHGlobal(copy, TRUE, &stream) != S_OK){
        ERR("could not make stream\n");
        GdipFree(copy);
        retval = GenericError;
        goto err;
    }

    *metafile = GdipAlloc(sizeof(GpMetafile));
    if(!*metafile){
        retval = OutOfMemory;
        goto err;
    }

    if(OleLoadPicture(stream, 0, FALSE, &IID_IPicture,
        (LPVOID*) &((*metafile)->image.picture)) != S_OK)
    {
        retval = GenericError;
        goto err;
    }


    (*metafile)->image.type = ImageTypeMetafile;
    memcpy(&(*metafile)->image.format, &ImageFormatWMF, sizeof(GUID));
    (*metafile)->image.palette = NULL;
    (*metafile)->image.xres = (REAL)copy->szlDevice.cx;
    (*metafile)->image.yres = (REAL)copy->szlDevice.cy;
    (*metafile)->bounds.X = (REAL)copy->rclBounds.left;
    (*metafile)->bounds.Y = (REAL)copy->rclBounds.top;
    (*metafile)->bounds.Width = (REAL)(copy->rclBounds.right - copy->rclBounds.left);
    (*metafile)->bounds.Height = (REAL)(copy->rclBounds.bottom - copy->rclBounds.top);
    (*metafile)->unit = UnitPixel;

    if(delete)
        DeleteEnhMetaFile(hemf);

    TRACE("<-- %p\n", *metafile);

err:
    if (retval != Ok)
        GdipFree(*metafile);
    IStream_Release(stream);
    return retval;
}

GpStatus WINGDIPAPI GdipCreateMetafileFromWmf(HMETAFILE hwmf, BOOL delete,
    GDIPCONST WmfPlaceableFileHeader * placeable, GpMetafile **metafile)
{
    UINT read;
    BYTE *copy;
    HENHMETAFILE hemf;
    GpStatus retval = Ok;

    TRACE("(%p, %d, %p, %p)\n", hwmf, delete, placeable, metafile);

    if(!hwmf || !metafile || !placeable)
        return InvalidParameter;

    *metafile = NULL;
    read = GetMetaFileBitsEx(hwmf, 0, NULL);
    if(!read)
        return GenericError;
    copy = GdipAlloc(read);
    GetMetaFileBitsEx(hwmf, read, copy);

    hemf = SetWinMetaFileBits(read, copy, NULL, NULL);
    GdipFree(copy);

    retval = GdipCreateMetafileFromEmf(hemf, FALSE, metafile);

    if (retval == Ok)
    {
        (*metafile)->image.xres = (REAL)placeable->Inch;
        (*metafile)->image.yres = (REAL)placeable->Inch;
        (*metafile)->bounds.X = ((REAL)placeable->BoundingBox.Left) / ((REAL)placeable->Inch);
        (*metafile)->bounds.Y = ((REAL)placeable->BoundingBox.Top) / ((REAL)placeable->Inch);
        (*metafile)->bounds.Width = (REAL)(placeable->BoundingBox.Right -
                                           placeable->BoundingBox.Left);
        (*metafile)->bounds.Height = (REAL)(placeable->BoundingBox.Bottom -
                                            placeable->BoundingBox.Top);

        if (delete) DeleteMetaFile(hwmf);
    }
    return retval;
}

GpStatus WINGDIPAPI GdipCreateMetafileFromWmfFile(GDIPCONST WCHAR *file,
    GDIPCONST WmfPlaceableFileHeader * placeable, GpMetafile **metafile)
{
    HMETAFILE hmf = GetMetaFileW(file);

    TRACE("(%s, %p, %p)\n", debugstr_w(file), placeable, metafile);

    if(!hmf) return InvalidParameter;

    return GdipCreateMetafileFromWmf(hmf, TRUE, placeable, metafile);
}

GpStatus WINGDIPAPI GdipCreateMetafileFromFile(GDIPCONST WCHAR *file,
    GpMetafile **metafile)
{
    FIXME("(%p, %p): stub\n", file, metafile);
    return NotImplemented;
}

GpStatus WINGDIPAPI GdipCreateMetafileFromStream(IStream *stream,
    GpMetafile **metafile)
{
    FIXME("(%p, %p): stub\n", stream, metafile);
    return NotImplemented;
}

GpStatus WINGDIPAPI GdipCreateStreamOnFile(GDIPCONST WCHAR * filename,
    UINT access, IStream **stream)
{
    DWORD dwMode;
    HRESULT ret;

    TRACE("(%s, %u, %p)\n", debugstr_w(filename), access, stream);

    if(!stream || !filename)
        return InvalidParameter;

    if(access & GENERIC_WRITE)
        dwMode = STGM_SHARE_DENY_WRITE | STGM_WRITE | STGM_CREATE;
    else if(access & GENERIC_READ)
        dwMode = STGM_SHARE_DENY_WRITE | STGM_READ | STGM_FAILIFTHERE;
    else
        return InvalidParameter;

    ret = SHCreateStreamOnFileW(filename, dwMode, stream);

    return hresult_to_status(ret);
}

GpStatus WINGDIPAPI GdipDeleteGraphics(GpGraphics *graphics)
{
    GraphicsContainerItem *cont, *next;
    GpStatus stat;
    TRACE("(%p)\n", graphics);

    if(!graphics) return InvalidParameter;
    if(graphics->busy) return ObjectBusy;

    if (graphics->image && graphics->image->type == ImageTypeMetafile)
    {
        stat = METAFILE_GraphicsDeleted((GpMetafile*)graphics->image);
        if (stat != Ok)
            return stat;
    }

    if(graphics->owndc)
        ReleaseDC(graphics->hwnd, graphics->hdc);

    LIST_FOR_EACH_ENTRY_SAFE(cont, next, &graphics->containers, GraphicsContainerItem, entry){
        list_remove(&cont->entry);
        delete_container(cont);
    }

    GdipDeleteRegion(graphics->clip);
    GdipDeleteMatrix(graphics->worldtrans);
    GdipFree(graphics);

    return Ok;
}

GpStatus WINGDIPAPI GdipDrawArc(GpGraphics *graphics, GpPen *pen, REAL x,
    REAL y, REAL width, REAL height, REAL startAngle, REAL sweepAngle)
{
    INT save_state, num_pts;
    GpPointF points[MAX_ARC_PTS];
    GpStatus retval;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x, y,
          width, height, startAngle, sweepAngle);

    if(!graphics || !pen || width <= 0 || height <= 0)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    num_pts = arc2polybezier(points, x, y, width, height, startAngle, sweepAngle);

    save_state = prepare_dc(graphics, pen);

    retval = draw_polybezier(graphics, pen, points, num_pts, TRUE);

    restore_dc(graphics, save_state);

    return retval;
}

GpStatus WINGDIPAPI GdipDrawArcI(GpGraphics *graphics, GpPen *pen, INT x,
    INT y, INT width, INT height, REAL startAngle, REAL sweepAngle)
{
    TRACE("(%p, %p, %d, %d, %d, %d, %.2f, %.2f)\n", graphics, pen, x, y,
          width, height, startAngle, sweepAngle);

    return GdipDrawArc(graphics,pen,(REAL)x,(REAL)y,(REAL)width,(REAL)height,startAngle,sweepAngle);
}

GpStatus WINGDIPAPI GdipDrawBezier(GpGraphics *graphics, GpPen *pen, REAL x1,
    REAL y1, REAL x2, REAL y2, REAL x3, REAL y3, REAL x4, REAL y4)
{
    INT save_state;
    GpPointF pt[4];
    GpStatus retval;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x1, y1,
          x2, y2, x3, y3, x4, y4);

    if(!graphics || !pen)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    pt[0].X = x1;
    pt[0].Y = y1;
    pt[1].X = x2;
    pt[1].Y = y2;
    pt[2].X = x3;
    pt[2].Y = y3;
    pt[3].X = x4;
    pt[3].Y = y4;

    save_state = prepare_dc(graphics, pen);

    retval = draw_polybezier(graphics, pen, pt, 4, TRUE);

    restore_dc(graphics, save_state);

    return retval;
}

GpStatus WINGDIPAPI GdipDrawBezierI(GpGraphics *graphics, GpPen *pen, INT x1,
    INT y1, INT x2, INT y2, INT x3, INT y3, INT x4, INT y4)
{
    INT save_state;
    GpPointF pt[4];
    GpStatus retval;

    TRACE("(%p, %p, %d, %d, %d, %d, %d, %d, %d, %d)\n", graphics, pen, x1, y1,
          x2, y2, x3, y3, x4, y4);

    if(!graphics || !pen)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    pt[0].X = x1;
    pt[0].Y = y1;
    pt[1].X = x2;
    pt[1].Y = y2;
    pt[2].X = x3;
    pt[2].Y = y3;
    pt[3].X = x4;
    pt[3].Y = y4;

    save_state = prepare_dc(graphics, pen);

    retval = draw_polybezier(graphics, pen, pt, 4, TRUE);

    restore_dc(graphics, save_state);

    return retval;
}

GpStatus WINGDIPAPI GdipDrawBeziers(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count)
{
    INT i;
    GpStatus ret;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(!graphics || !pen || !points || (count <= 0))
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    for(i = 0; i < floor(count / 4); i++){
        ret = GdipDrawBezier(graphics, pen,
                             points[4*i].X, points[4*i].Y,
                             points[4*i + 1].X, points[4*i + 1].Y,
                             points[4*i + 2].X, points[4*i + 2].Y,
                             points[4*i + 3].X, points[4*i + 3].Y);
        if(ret != Ok)
            return ret;
    }

    return Ok;
}

GpStatus WINGDIPAPI GdipDrawBeziersI(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count)
{
    GpPointF *pts;
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(!graphics || !pen || !points || (count <= 0))
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    pts = GdipAlloc(sizeof(GpPointF) * count);
    if(!pts)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        pts[i].X = (REAL)points[i].X;
        pts[i].Y = (REAL)points[i].Y;
    }

    ret = GdipDrawBeziers(graphics,pen,pts,count);

    GdipFree(pts);

    return ret;
}

GpStatus WINGDIPAPI GdipDrawClosedCurve(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    return GdipDrawClosedCurve2(graphics, pen, points, count, 1.0);
}

GpStatus WINGDIPAPI GdipDrawClosedCurveI(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    return GdipDrawClosedCurve2I(graphics, pen, points, count, 1.0);
}

GpStatus WINGDIPAPI GdipDrawClosedCurve2(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count, REAL tension)
{
    GpPath *path;
    GpStatus stat;

    TRACE("(%p, %p, %p, %d, %.2f)\n", graphics, pen, points, count, tension);

    if(!graphics || !pen || !points || count <= 0)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if((stat = GdipCreatePath(FillModeAlternate, &path)) != Ok)
        return stat;

    stat = GdipAddPathClosedCurve2(path, points, count, tension);
    if(stat != Ok){
        GdipDeletePath(path);
        return stat;
    }

    stat = GdipDrawPath(graphics, pen, path);

    GdipDeletePath(path);

    return stat;
}

GpStatus WINGDIPAPI GdipDrawClosedCurve2I(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count, REAL tension)
{
    GpPointF *ptf;
    GpStatus stat;
    INT i;

    TRACE("(%p, %p, %p, %d, %.2f)\n", graphics, pen, points, count, tension);

    if(!points || count <= 0)
        return InvalidParameter;

    ptf = GdipAlloc(sizeof(GpPointF)*count);
    if(!ptf)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        ptf[i].X = (REAL)points[i].X;
        ptf[i].Y = (REAL)points[i].Y;
    }

    stat = GdipDrawClosedCurve2(graphics, pen, ptf, count, tension);

    GdipFree(ptf);

    return stat;
}

GpStatus WINGDIPAPI GdipDrawCurve(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    return GdipDrawCurve2(graphics,pen,points,count,1.0);
}

GpStatus WINGDIPAPI GdipDrawCurveI(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count)
{
    GpPointF *pointsF;
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(!points)
        return InvalidParameter;

    pointsF = GdipAlloc(sizeof(GpPointF)*count);
    if(!pointsF)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        pointsF[i].X = (REAL)points[i].X;
        pointsF[i].Y = (REAL)points[i].Y;
    }

    ret = GdipDrawCurve(graphics,pen,pointsF,count);
    GdipFree(pointsF);

    return ret;
}

/* Approximates cardinal spline with Bezier curves. */
GpStatus WINGDIPAPI GdipDrawCurve2(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count, REAL tension)
{
    /* PolyBezier expects count*3-2 points. */
    INT i, len_pt = count*3-2, save_state;
    GpPointF *pt;
    REAL x1, x2, y1, y2;
    GpStatus retval;

    TRACE("(%p, %p, %p, %d, %.2f)\n", graphics, pen, points, count, tension);

    if(!graphics || !pen)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(count < 2)
        return InvalidParameter;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    pt = GdipAlloc(len_pt * sizeof(GpPointF));
    if(!pt)
        return OutOfMemory;

    tension = tension * TENSION_CONST;

    calc_curve_bezier_endp(points[0].X, points[0].Y, points[1].X, points[1].Y,
        tension, &x1, &y1);

    pt[0].X = points[0].X;
    pt[0].Y = points[0].Y;
    pt[1].X = x1;
    pt[1].Y = y1;

    for(i = 0; i < count-2; i++){
        calc_curve_bezier(&(points[i]), tension, &x1, &y1, &x2, &y2);

        pt[3*i+2].X = x1;
        pt[3*i+2].Y = y1;
        pt[3*i+3].X = points[i+1].X;
        pt[3*i+3].Y = points[i+1].Y;
        pt[3*i+4].X = x2;
        pt[3*i+4].Y = y2;
    }

    calc_curve_bezier_endp(points[count-1].X, points[count-1].Y,
        points[count-2].X, points[count-2].Y, tension, &x1, &y1);

    pt[len_pt-2].X = x1;
    pt[len_pt-2].Y = y1;
    pt[len_pt-1].X = points[count-1].X;
    pt[len_pt-1].Y = points[count-1].Y;

    save_state = prepare_dc(graphics, pen);

    retval = draw_polybezier(graphics, pen, pt, len_pt, TRUE);

    GdipFree(pt);
    restore_dc(graphics, save_state);

    return retval;
}

GpStatus WINGDIPAPI GdipDrawCurve2I(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count, REAL tension)
{
    GpPointF *pointsF;
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d, %.2f)\n", graphics, pen, points, count, tension);

    if(!points)
        return InvalidParameter;

    pointsF = GdipAlloc(sizeof(GpPointF)*count);
    if(!pointsF)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        pointsF[i].X = (REAL)points[i].X;
        pointsF[i].Y = (REAL)points[i].Y;
    }

    ret = GdipDrawCurve2(graphics,pen,pointsF,count,tension);
    GdipFree(pointsF);

    return ret;
}

GpStatus WINGDIPAPI GdipDrawCurve3(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPointF *points, INT count, INT offset, INT numberOfSegments,
    REAL tension)
{
    TRACE("(%p, %p, %p, %d, %d, %d, %.2f)\n", graphics, pen, points, count, offset, numberOfSegments, tension);

    if(offset >= count || numberOfSegments > count - offset - 1 || numberOfSegments <= 0){
        return InvalidParameter;
    }

    return GdipDrawCurve2(graphics, pen, points + offset, numberOfSegments + 1, tension);
}

GpStatus WINGDIPAPI GdipDrawCurve3I(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpPoint *points, INT count, INT offset, INT numberOfSegments,
    REAL tension)
{
    TRACE("(%p, %p, %p, %d, %d, %d, %.2f)\n", graphics, pen, points, count, offset, numberOfSegments, tension);

    if(count < 0){
        return OutOfMemory;
    }

    if(offset >= count || numberOfSegments > count - offset - 1 || numberOfSegments <= 0){
        return InvalidParameter;
    }

    return GdipDrawCurve2I(graphics, pen, points + offset, numberOfSegments + 1, tension);
}

GpStatus WINGDIPAPI GdipDrawEllipse(GpGraphics *graphics, GpPen *pen, REAL x,
    REAL y, REAL width, REAL height)
{
    INT save_state;
    GpPointF ptf[2];
    POINT pti[2];

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x, y, width, height);

    if(!graphics || !pen)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    ptf[0].X = x;
    ptf[0].Y = y;
    ptf[1].X = x + width;
    ptf[1].Y = y + height;

    save_state = prepare_dc(graphics, pen);
    SelectObject(graphics->hdc, GetStockObject(NULL_BRUSH));

    transform_and_round_points(graphics, pti, ptf, 2);

    Ellipse(graphics->hdc, pti[0].x, pti[0].y, pti[1].x, pti[1].y);

    restore_dc(graphics, save_state);

    return Ok;
}

GpStatus WINGDIPAPI GdipDrawEllipseI(GpGraphics *graphics, GpPen *pen, INT x,
    INT y, INT width, INT height)
{
    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, pen, x, y, width, height);

    return GdipDrawEllipse(graphics,pen,(REAL)x,(REAL)y,(REAL)width,(REAL)height);
}


GpStatus WINGDIPAPI GdipDrawImage(GpGraphics *graphics, GpImage *image, REAL x, REAL y)
{
    UINT width, height;

    TRACE("(%p, %p, %.2f, %.2f)\n", graphics, image, x, y);

    if(!graphics || !image)
        return InvalidParameter;

    GdipGetImageWidth(image, &width);
    GdipGetImageHeight(image, &height);

    return GdipDrawImagePointRect(graphics, image, x, y,
                                  0.0, 0.0, (REAL)width, (REAL)height, UnitPixel);
}

GpStatus WINGDIPAPI GdipDrawImageI(GpGraphics *graphics, GpImage *image, INT x,
    INT y)
{
    TRACE("(%p, %p, %d, %d)\n", graphics, image, x, y);

    return GdipDrawImage(graphics, image, (REAL)x, (REAL)y);
}

GpStatus WINGDIPAPI GdipDrawImagePointRect(GpGraphics *graphics, GpImage *image,
    REAL x, REAL y, REAL srcx, REAL srcy, REAL srcwidth, REAL srcheight,
    GpUnit srcUnit)
{
    GpPointF points[3];
    REAL scale_x, scale_y, width, height;

    TRACE("(%p, %p, %f, %f, %f, %f, %f, %f, %d)\n", graphics, image, x, y, srcx, srcy, srcwidth, srcheight, srcUnit);

    scale_x = units_scale(srcUnit, graphics->unit, graphics->xres);
    scale_x *= graphics->xres / image->xres;
    scale_y = units_scale(srcUnit, graphics->unit, graphics->yres);
    scale_y *= graphics->yres / image->yres;
    width = srcwidth * scale_x;
    height = srcheight * scale_y;

    points[0].X = points[2].X = x;
    points[0].Y = points[1].Y = y;
    points[1].X = x + width;
    points[2].Y = y + height;

    return GdipDrawImagePointsRect(graphics, image, points, 3, srcx, srcy,
        srcwidth, srcheight, srcUnit, NULL, NULL, NULL);
}

GpStatus WINGDIPAPI GdipDrawImagePointRectI(GpGraphics *graphics, GpImage *image,
    INT x, INT y, INT srcx, INT srcy, INT srcwidth, INT srcheight,
    GpUnit srcUnit)
{
    return GdipDrawImagePointRect(graphics, image, x, y, srcx, srcy, srcwidth, srcheight, srcUnit);
}

GpStatus WINGDIPAPI GdipDrawImagePoints(GpGraphics *graphics, GpImage *image,
    GDIPCONST GpPointF *dstpoints, INT count)
{
    UINT width, height;

    TRACE("(%p, %p, %p, %d)\n", graphics, image, dstpoints, count);

    if(!image)
        return InvalidParameter;

    GdipGetImageWidth(image, &width);
    GdipGetImageHeight(image, &height);

    return GdipDrawImagePointsRect(graphics, image, dstpoints, count, 0, 0,
        width, height, UnitPixel, NULL, NULL, NULL);
}

GpStatus WINGDIPAPI GdipDrawImagePointsI(GpGraphics *graphics, GpImage *image,
    GDIPCONST GpPoint *dstpoints, INT count)
{
    GpPointF ptf[3];

    TRACE("(%p, %p, %p, %d)\n", graphics, image, dstpoints, count);

    if (count != 3 || !dstpoints)
        return InvalidParameter;

    ptf[0].X = (REAL)dstpoints[0].X;
    ptf[0].Y = (REAL)dstpoints[0].Y;
    ptf[1].X = (REAL)dstpoints[1].X;
    ptf[1].Y = (REAL)dstpoints[1].Y;
    ptf[2].X = (REAL)dstpoints[2].X;
    ptf[2].Y = (REAL)dstpoints[2].Y;

    return GdipDrawImagePoints(graphics, image, ptf, count);
}

GpStatus WINGDIPAPI GdipDrawImagePointsRect(GpGraphics *graphics, GpImage *image,
     GDIPCONST GpPointF *points, INT count, REAL srcx, REAL srcy, REAL srcwidth,
     REAL srcheight, GpUnit srcUnit, GDIPCONST GpImageAttributes* imageAttributes,
     DrawImageAbort callback, VOID * callbackData)
{
    GpPointF ptf[4];
    POINT pti[4];
    GpStatus stat;

    TRACE("(%p, %p, %p, %d, %f, %f, %f, %f, %d, %p, %p, %p)\n", graphics, image, points,
          count, srcx, srcy, srcwidth, srcheight, srcUnit, imageAttributes, callback,
          callbackData);

    if (count > 3)
        return NotImplemented;

    if(!graphics || !image || !points || count != 3)
         return InvalidParameter;

    TRACE("%s %s %s\n", debugstr_pointf(&points[0]), debugstr_pointf(&points[1]),
        debugstr_pointf(&points[2]));

    memcpy(ptf, points, 3 * sizeof(GpPointF));
    ptf[3].X = ptf[2].X + ptf[1].X - ptf[0].X;
    ptf[3].Y = ptf[2].Y + ptf[1].Y - ptf[0].Y;
    if (!srcwidth || !srcheight || ptf[3].X == ptf[0].X || ptf[3].Y == ptf[0].Y)
        return Ok;
    transform_and_round_points(graphics, pti, ptf, 4);

    TRACE("%s %s %s %s\n", wine_dbgstr_point(&pti[0]), wine_dbgstr_point(&pti[1]),
        wine_dbgstr_point(&pti[2]), wine_dbgstr_point(&pti[3]));

    srcx = units_to_pixels(srcx, srcUnit, image->xres);
    srcy = units_to_pixels(srcy, srcUnit, image->yres);
    srcwidth = units_to_pixels(srcwidth, srcUnit, image->xres);
    srcheight = units_to_pixels(srcheight, srcUnit, image->yres);
    TRACE("src pixels: %f,%f %fx%f\n", srcx, srcy, srcwidth, srcheight);

    if (image->picture)
    {
        if (!graphics->hdc)
        {
            FIXME("graphics object has no HDC\n");
        }

        if(IPicture_Render(image->picture, graphics->hdc,
            pti[0].x, pti[0].y, pti[1].x - pti[0].x, pti[2].y - pti[0].y,
            srcx, srcy, srcwidth, srcheight, NULL) != S_OK)
        {
            if(callback)
                callback(callbackData);
            return GenericError;
        }
    }
    else if (image->type == ImageTypeBitmap)
    {
        GpBitmap* bitmap = (GpBitmap*)image;
        int use_software=0;

        if (imageAttributes ||
            (graphics->image && graphics->image->type == ImageTypeBitmap) ||
            ptf[1].Y != ptf[0].Y || ptf[2].X != ptf[0].X ||
            ptf[1].X - ptf[0].X != srcwidth || ptf[2].Y - ptf[0].Y != srcheight ||
            srcx < 0 || srcy < 0 ||
            srcx + srcwidth > bitmap->width || srcy + srcheight > bitmap->height)
            use_software = 1;

        if (use_software)
        {
            RECT dst_area;
            GpRect src_area;
            int i, x, y, src_stride, dst_stride;
            GpMatrix *dst_to_src;
            REAL m11, m12, m21, m22, mdx, mdy;
            LPBYTE src_data, dst_data;
            BitmapData lockeddata;
            InterpolationMode interpolation = graphics->interpolation;
            GpPointF dst_to_src_points[3] = {{0.0, 0.0}, {1.0, 0.0}, {0.0, 1.0}};
            REAL x_dx, x_dy, y_dx, y_dy;
            static const GpImageAttributes defaultImageAttributes = {WrapModeClamp, 0, FALSE};

            if (!imageAttributes)
                imageAttributes = &defaultImageAttributes;

            dst_area.left = dst_area.right = pti[0].x;
            dst_area.top = dst_area.bottom = pti[0].y;
            for (i=1; i<4; i++)
            {
                if (dst_area.left > pti[i].x) dst_area.left = pti[i].x;
                if (dst_area.right < pti[i].x) dst_area.right = pti[i].x;
                if (dst_area.top > pti[i].y) dst_area.top = pti[i].y;
                if (dst_area.bottom < pti[i].y) dst_area.bottom = pti[i].y;
            }

            TRACE("dst_area: %s\n", wine_dbgstr_rect(&dst_area));

            m11 = (ptf[1].X - ptf[0].X) / srcwidth;
            m21 = (ptf[2].X - ptf[0].X) / srcheight;
            mdx = ptf[0].X - m11 * srcx - m21 * srcy;
            m12 = (ptf[1].Y - ptf[0].Y) / srcwidth;
            m22 = (ptf[2].Y - ptf[0].Y) / srcheight;
            mdy = ptf[0].Y - m12 * srcx - m22 * srcy;

            stat = GdipCreateMatrix2(m11, m12, m21, m22, mdx, mdy, &dst_to_src);
            if (stat != Ok) return stat;

            stat = GdipInvertMatrix(dst_to_src);
            if (stat != Ok)
            {
                GdipDeleteMatrix(dst_to_src);
                return stat;
            }

            dst_data = GdipAlloc(sizeof(ARGB) * (dst_area.right - dst_area.left) * (dst_area.bottom - dst_area.top));
            if (!dst_data)
            {
                GdipDeleteMatrix(dst_to_src);
                return OutOfMemory;
            }

            dst_stride = sizeof(ARGB) * (dst_area.right - dst_area.left);

            get_bitmap_sample_size(interpolation, imageAttributes->wrap,
                bitmap, srcx, srcy, srcwidth, srcheight, &src_area);

            TRACE("src_area: %d x %d\n", src_area.Width, src_area.Height);

            src_data = GdipAlloc(sizeof(ARGB) * src_area.Width * src_area.Height);
            if (!src_data)
            {
                GdipFree(dst_data);
                GdipDeleteMatrix(dst_to_src);
                return OutOfMemory;
            }
            src_stride = sizeof(ARGB) * src_area.Width;

            /* Read the bits we need from the source bitmap into an ARGB buffer. */
            lockeddata.Width = src_area.Width;
            lockeddata.Height = src_area.Height;
            lockeddata.Stride = src_stride;
            lockeddata.PixelFormat = PixelFormat32bppARGB;
            lockeddata.Scan0 = src_data;

            stat = GdipBitmapLockBits(bitmap, &src_area, ImageLockModeRead|ImageLockModeUserInputBuf,
                PixelFormat32bppARGB, &lockeddata);

            if (stat == Ok)
                stat = GdipBitmapUnlockBits(bitmap, &lockeddata);

            if (stat != Ok)
            {
                if (src_data != dst_data)
                    GdipFree(src_data);
                GdipFree(dst_data);
                GdipDeleteMatrix(dst_to_src);
                return stat;
            }

            apply_image_attributes(imageAttributes, src_data,
                src_area.Width, src_area.Height,
                src_stride, ColorAdjustTypeBitmap);

            /* Transform the bits as needed to the destination. */
            GdipTransformMatrixPoints(dst_to_src, dst_to_src_points, 3);

            x_dx = dst_to_src_points[1].X - dst_to_src_points[0].X;
            x_dy = dst_to_src_points[1].Y - dst_to_src_points[0].Y;
            y_dx = dst_to_src_points[2].X - dst_to_src_points[0].X;
            y_dy = dst_to_src_points[2].Y - dst_to_src_points[0].Y;

            for (x=dst_area.left; x<dst_area.right; x++)
            {
                for (y=dst_area.top; y<dst_area.bottom; y++)
                {
                    GpPointF src_pointf;
                    ARGB *dst_color;

                    src_pointf.X = dst_to_src_points[0].X + x * x_dx + y * y_dx;
                    src_pointf.Y = dst_to_src_points[0].Y + x * x_dy + y * y_dy;

                    dst_color = (ARGB*)(dst_data + dst_stride * (y - dst_area.top) + sizeof(ARGB) * (x - dst_area.left));

                    if (src_pointf.X >= srcx && src_pointf.X < srcx + srcwidth && src_pointf.Y >= srcy && src_pointf.Y < srcy+srcheight)
                        *dst_color = resample_bitmap_pixel(&src_area, src_data, bitmap->width, bitmap->height, &src_pointf, imageAttributes, interpolation);
                    else
                        *dst_color = 0;
                }
            }

            GdipDeleteMatrix(dst_to_src);

            GdipFree(src_data);

            stat = alpha_blend_pixels(graphics, dst_area.left, dst_area.top,
                dst_data, dst_area.right - dst_area.left, dst_area.bottom - dst_area.top, dst_stride);

            GdipFree(dst_data);

            return stat;
        }
        else
        {
            HDC hdc;
            int temp_hdc=0, temp_bitmap=0;
            HBITMAP hbitmap, old_hbm=NULL;

            if (!(bitmap->format == PixelFormat16bppRGB555 ||
                  bitmap->format == PixelFormat24bppRGB ||
                  bitmap->format == PixelFormat32bppRGB ||
                  bitmap->format == PixelFormat32bppPARGB))
            {
                BITMAPINFOHEADER bih;
                BYTE *temp_bits;
                PixelFormat dst_format;

                /* we can't draw a bitmap of this format directly */
                hdc = CreateCompatibleDC(0);
                temp_hdc = 1;
                temp_bitmap = 1;

                bih.biSize = sizeof(BITMAPINFOHEADER);
                bih.biWidth = bitmap->width;
                bih.biHeight = -bitmap->height;
                bih.biPlanes = 1;
                bih.biBitCount = 32;
                bih.biCompression = BI_RGB;
                bih.biSizeImage = 0;
                bih.biXPelsPerMeter = 0;
                bih.biYPelsPerMeter = 0;
                bih.biClrUsed = 0;
                bih.biClrImportant = 0;

                hbitmap = CreateDIBSection(hdc, (BITMAPINFO*)&bih, DIB_RGB_COLORS,
                    (void**)&temp_bits, NULL, 0);

                if (bitmap->format & (PixelFormatAlpha|PixelFormatPAlpha))
                    dst_format = PixelFormat32bppPARGB;
                else
                    dst_format = PixelFormat32bppRGB;

                convert_pixels(bitmap->width, bitmap->height,
                    bitmap->width*4, temp_bits, dst_format,
                    bitmap->stride, bitmap->bits, bitmap->format,
                    bitmap->image.palette);
            }
            else
            {
                if (bitmap->hbitmap)
                    hbitmap = bitmap->hbitmap;
                else
                {
                    GdipCreateHBITMAPFromBitmap(bitmap, &hbitmap, 0);
                    temp_bitmap = 1;
                }

                hdc = bitmap->hdc;
                temp_hdc = (hdc == 0);
            }

            if (temp_hdc)
            {
                if (!hdc) hdc = CreateCompatibleDC(0);
                old_hbm = SelectObject(hdc, hbitmap);
            }

            if (bitmap->format & (PixelFormatAlpha|PixelFormatPAlpha))
            {
                gdi_alpha_blend(graphics, pti[0].x, pti[0].y, pti[1].x - pti[0].x, pti[2].y - pti[0].y,
                                hdc, srcx, srcy, srcwidth, srcheight);
            }
            else
            {
                StretchBlt(graphics->hdc, pti[0].x, pti[0].y, pti[1].x-pti[0].x, pti[2].y-pti[0].y,
                    hdc, srcx, srcy, srcwidth, srcheight, SRCCOPY);
            }

            if (temp_hdc)
            {
                SelectObject(hdc, old_hbm);
                DeleteDC(hdc);
            }

            if (temp_bitmap)
                DeleteObject(hbitmap);
        }
    }
    else
    {
        ERR("GpImage with no IPicture or HBITMAP?!\n");
        return NotImplemented;
    }

    return Ok;
}

GpStatus WINGDIPAPI GdipDrawImagePointsRectI(GpGraphics *graphics, GpImage *image,
     GDIPCONST GpPoint *points, INT count, INT srcx, INT srcy, INT srcwidth,
     INT srcheight, GpUnit srcUnit, GDIPCONST GpImageAttributes* imageAttributes,
     DrawImageAbort callback, VOID * callbackData)
{
    GpPointF pointsF[3];
    INT i;

    TRACE("(%p, %p, %p, %d, %d, %d, %d, %d, %d, %p, %p, %p)\n", graphics, image, points, count,
          srcx, srcy, srcwidth, srcheight, srcUnit, imageAttributes, callback,
          callbackData);

    if(!points || count!=3)
        return InvalidParameter;

    for(i = 0; i < count; i++){
        pointsF[i].X = (REAL)points[i].X;
        pointsF[i].Y = (REAL)points[i].Y;
    }

    return GdipDrawImagePointsRect(graphics, image, pointsF, count, (REAL)srcx, (REAL)srcy,
                                   (REAL)srcwidth, (REAL)srcheight, srcUnit, imageAttributes,
                                   callback, callbackData);
}

GpStatus WINGDIPAPI GdipDrawImageRectRect(GpGraphics *graphics, GpImage *image,
    REAL dstx, REAL dsty, REAL dstwidth, REAL dstheight, REAL srcx, REAL srcy,
    REAL srcwidth, REAL srcheight, GpUnit srcUnit,
    GDIPCONST GpImageAttributes* imageattr, DrawImageAbort callback,
    VOID * callbackData)
{
    GpPointF points[3];

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %d, %p, %p, %p)\n",
          graphics, image, dstx, dsty, dstwidth, dstheight, srcx, srcy,
          srcwidth, srcheight, srcUnit, imageattr, callback, callbackData);

    points[0].X = dstx;
    points[0].Y = dsty;
    points[1].X = dstx + dstwidth;
    points[1].Y = dsty;
    points[2].X = dstx;
    points[2].Y = dsty + dstheight;

    return GdipDrawImagePointsRect(graphics, image, points, 3, srcx, srcy,
               srcwidth, srcheight, srcUnit, imageattr, callback, callbackData);
}

GpStatus WINGDIPAPI GdipDrawImageRectRectI(GpGraphics *graphics, GpImage *image,
	INT dstx, INT dsty, INT dstwidth, INT dstheight, INT srcx, INT srcy,
	INT srcwidth, INT srcheight, GpUnit srcUnit,
	GDIPCONST GpImageAttributes* imageAttributes, DrawImageAbort callback,
	VOID * callbackData)
{
    GpPointF points[3];

    TRACE("(%p, %p, %d, %d, %d, %d, %d, %d, %d, %d, %d, %p, %p, %p)\n",
          graphics, image, dstx, dsty, dstwidth, dstheight, srcx, srcy,
          srcwidth, srcheight, srcUnit, imageAttributes, callback, callbackData);

    points[0].X = dstx;
    points[0].Y = dsty;
    points[1].X = dstx + dstwidth;
    points[1].Y = dsty;
    points[2].X = dstx;
    points[2].Y = dsty + dstheight;

    return GdipDrawImagePointsRect(graphics, image, points, 3, srcx, srcy,
               srcwidth, srcheight, srcUnit, imageAttributes, callback, callbackData);
}

GpStatus WINGDIPAPI GdipDrawImageRect(GpGraphics *graphics, GpImage *image,
    REAL x, REAL y, REAL width, REAL height)
{
    RectF bounds;
    GpUnit unit;
    GpStatus ret;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, image, x, y, width, height);

    if(!graphics || !image)
        return InvalidParameter;

    ret = GdipGetImageBounds(image, &bounds, &unit);
    if(ret != Ok)
        return ret;

    return GdipDrawImageRectRect(graphics, image, x, y, width, height,
                                 bounds.X, bounds.Y, bounds.Width, bounds.Height,
                                 unit, NULL, NULL, NULL);
}

GpStatus WINGDIPAPI GdipDrawImageRectI(GpGraphics *graphics, GpImage *image,
    INT x, INT y, INT width, INT height)
{
    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, image, x, y, width, height);

    return GdipDrawImageRect(graphics, image, (REAL)x, (REAL)y, (REAL)width, (REAL)height);
}

GpStatus WINGDIPAPI GdipDrawLine(GpGraphics *graphics, GpPen *pen, REAL x1,
    REAL y1, REAL x2, REAL y2)
{
    INT save_state;
    GpPointF pt[2];
    GpStatus retval;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x1, y1, x2, y2);

    if(!pen || !graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    pt[0].X = x1;
    pt[0].Y = y1;
    pt[1].X = x2;
    pt[1].Y = y2;

    save_state = prepare_dc(graphics, pen);

    retval = draw_polyline(graphics, pen, pt, 2, TRUE);

    restore_dc(graphics, save_state);

    return retval;
}

GpStatus WINGDIPAPI GdipDrawLineI(GpGraphics *graphics, GpPen *pen, INT x1,
    INT y1, INT x2, INT y2)
{
    INT save_state;
    GpPointF pt[2];
    GpStatus retval;

    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, pen, x1, y1, x2, y2);

    if(!pen || !graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    pt[0].X = (REAL)x1;
    pt[0].Y = (REAL)y1;
    pt[1].X = (REAL)x2;
    pt[1].Y = (REAL)y2;

    save_state = prepare_dc(graphics, pen);

    retval = draw_polyline(graphics, pen, pt, 2, TRUE);

    restore_dc(graphics, save_state);

    return retval;
}

GpStatus WINGDIPAPI GdipDrawLines(GpGraphics *graphics, GpPen *pen, GDIPCONST
    GpPointF *points, INT count)
{
    INT save_state;
    GpStatus retval;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(!pen || !graphics || (count < 2))
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    save_state = prepare_dc(graphics, pen);

    retval = draw_polyline(graphics, pen, points, count, TRUE);

    restore_dc(graphics, save_state);

    return retval;
}

GpStatus WINGDIPAPI GdipDrawLinesI(GpGraphics *graphics, GpPen *pen, GDIPCONST
    GpPoint *points, INT count)
{
    INT save_state;
    GpStatus retval;
    GpPointF *ptf = NULL;
    int i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(!pen || !graphics || (count < 2))
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    ptf = GdipAlloc(count * sizeof(GpPointF));
    if(!ptf) return OutOfMemory;

    for(i = 0; i < count; i ++){
        ptf[i].X = (REAL) points[i].X;
        ptf[i].Y = (REAL) points[i].Y;
    }

    save_state = prepare_dc(graphics, pen);

    retval = draw_polyline(graphics, pen, ptf, count, TRUE);

    restore_dc(graphics, save_state);

    GdipFree(ptf);
    return retval;
}

GpStatus WINGDIPAPI GdipDrawPath(GpGraphics *graphics, GpPen *pen, GpPath *path)
{
    INT save_state;
    GpStatus retval;

    TRACE("(%p, %p, %p)\n", graphics, pen, path);

    if(!pen || !graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    save_state = prepare_dc(graphics, pen);

    retval = draw_poly(graphics, pen, path->pathdata.Points,
                       path->pathdata.Types, path->pathdata.Count, TRUE);

    restore_dc(graphics, save_state);

    return retval;
}

GpStatus WINGDIPAPI GdipDrawPie(GpGraphics *graphics, GpPen *pen, REAL x,
    REAL y, REAL width, REAL height, REAL startAngle, REAL sweepAngle)
{
    INT save_state;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x, y,
            width, height, startAngle, sweepAngle);

    if(!graphics || !pen)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    save_state = prepare_dc(graphics, pen);
    SelectObject(graphics->hdc, GetStockObject(NULL_BRUSH));

    draw_pie(graphics, x, y, width, height, startAngle, sweepAngle);

    restore_dc(graphics, save_state);

    return Ok;
}

GpStatus WINGDIPAPI GdipDrawPieI(GpGraphics *graphics, GpPen *pen, INT x,
    INT y, INT width, INT height, REAL startAngle, REAL sweepAngle)
{
    TRACE("(%p, %p, %d, %d, %d, %d, %.2f, %.2f)\n", graphics, pen, x, y,
            width, height, startAngle, sweepAngle);

    return GdipDrawPie(graphics,pen,(REAL)x,(REAL)y,(REAL)width,(REAL)height,startAngle,sweepAngle);
}

GpStatus WINGDIPAPI GdipDrawRectangle(GpGraphics *graphics, GpPen *pen, REAL x,
    REAL y, REAL width, REAL height)
{
    INT save_state;
    GpPointF ptf[4];
    POINT pti[4];

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, pen, x, y, width, height);

    if(!pen || !graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    ptf[0].X = x;
    ptf[0].Y = y;
    ptf[1].X = x + width;
    ptf[1].Y = y;
    ptf[2].X = x + width;
    ptf[2].Y = y + height;
    ptf[3].X = x;
    ptf[3].Y = y + height;

    save_state = prepare_dc(graphics, pen);
    SelectObject(graphics->hdc, GetStockObject(NULL_BRUSH));

    transform_and_round_points(graphics, pti, ptf, 4);
    Polygon(graphics->hdc, pti, 4);

    restore_dc(graphics, save_state);

    return Ok;
}

GpStatus WINGDIPAPI GdipDrawRectangleI(GpGraphics *graphics, GpPen *pen, INT x,
    INT y, INT width, INT height)
{
    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, pen, x, y, width, height);

    return GdipDrawRectangle(graphics,pen,(REAL)x,(REAL)y,(REAL)width,(REAL)height);
}

GpStatus WINGDIPAPI GdipDrawRectangles(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpRectF* rects, INT count)
{
    GpPointF *ptf;
    POINT *pti;
    INT save_state, i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, rects, count);

    if(!graphics || !pen || !rects || count < 1)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    ptf = GdipAlloc(4 * count * sizeof(GpPointF));
    pti = GdipAlloc(4 * count * sizeof(POINT));

    if(!ptf || !pti){
        GdipFree(ptf);
        GdipFree(pti);
        return OutOfMemory;
    }

    for(i = 0; i < count; i++){
        ptf[4 * i + 3].X = ptf[4 * i].X = rects[i].X;
        ptf[4 * i + 1].Y = ptf[4 * i].Y = rects[i].Y;
        ptf[4 * i + 2].X = ptf[4 * i + 1].X = rects[i].X + rects[i].Width;
        ptf[4 * i + 3].Y = ptf[4 * i + 2].Y = rects[i].Y + rects[i].Height;
    }

    save_state = prepare_dc(graphics, pen);
    SelectObject(graphics->hdc, GetStockObject(NULL_BRUSH));

    transform_and_round_points(graphics, pti, ptf, 4 * count);

    for(i = 0; i < count; i++)
        Polygon(graphics->hdc, &pti[4 * i], 4);

    restore_dc(graphics, save_state);

    GdipFree(ptf);
    GdipFree(pti);

    return Ok;
}

GpStatus WINGDIPAPI GdipDrawRectanglesI(GpGraphics *graphics, GpPen *pen,
    GDIPCONST GpRect* rects, INT count)
{
    GpRectF *rectsF;
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, rects, count);

    if(!rects || count<=0)
        return InvalidParameter;

    rectsF = GdipAlloc(sizeof(GpRectF) * count);
    if(!rectsF)
        return OutOfMemory;

    for(i = 0;i < count;i++){
        rectsF[i].X      = (REAL)rects[i].X;
        rectsF[i].Y      = (REAL)rects[i].Y;
        rectsF[i].Width  = (REAL)rects[i].Width;
        rectsF[i].Height = (REAL)rects[i].Height;
    }

    ret = GdipDrawRectangles(graphics, pen, rectsF, count);
    GdipFree(rectsF);

    return ret;
}

GpStatus WINGDIPAPI GdipFillClosedCurve2(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPointF *points, INT count, REAL tension, GpFillMode fill)
{
    GpPath *path;
    GpStatus stat;

    TRACE("(%p, %p, %p, %d, %.2f, %d)\n", graphics, brush, points,
            count, tension, fill);

    if(!graphics || !brush || !points)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(count == 1)    /* Do nothing */
        return Ok;

    stat = GdipCreatePath(fill, &path);
    if(stat != Ok)
        return stat;

    stat = GdipAddPathClosedCurve2(path, points, count, tension);
    if(stat != Ok){
        GdipDeletePath(path);
        return stat;
    }

    stat = GdipFillPath(graphics, brush, path);
    if(stat != Ok){
        GdipDeletePath(path);
        return stat;
    }

    GdipDeletePath(path);

    return Ok;
}

GpStatus WINGDIPAPI GdipFillClosedCurve2I(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPoint *points, INT count, REAL tension, GpFillMode fill)
{
    GpPointF *ptf;
    GpStatus stat;
    INT i;

    TRACE("(%p, %p, %p, %d, %.2f, %d)\n", graphics, brush, points,
            count, tension, fill);

    if(!points || count == 0)
        return InvalidParameter;

    if(count == 1)    /* Do nothing */
        return Ok;

    ptf = GdipAlloc(sizeof(GpPointF)*count);
    if(!ptf)
        return OutOfMemory;

    for(i = 0;i < count;i++){
        ptf[i].X = (REAL)points[i].X;
        ptf[i].Y = (REAL)points[i].Y;
    }

    stat = GdipFillClosedCurve2(graphics, brush, ptf, count, tension, fill);

    GdipFree(ptf);

    return stat;
}

GpStatus WINGDIPAPI GdipFillClosedCurve(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPointF *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, brush, points, count);
    return GdipFillClosedCurve2(graphics, brush, points, count,
               0.5f, FillModeAlternate);
}

GpStatus WINGDIPAPI GdipFillClosedCurveI(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPoint *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, brush, points, count);
    return GdipFillClosedCurve2I(graphics, brush, points, count,
               0.5f, FillModeAlternate);
}

GpStatus WINGDIPAPI GdipFillEllipse(GpGraphics *graphics, GpBrush *brush, REAL x,
    REAL y, REAL width, REAL height)
{
    GpStatus stat;
    GpPath *path;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, brush, x, y, width, height);

    if(!graphics || !brush)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    stat = GdipCreatePath(FillModeAlternate, &path);

    if (stat == Ok)
    {
        stat = GdipAddPathEllipse(path, x, y, width, height);

        if (stat == Ok)
            stat = GdipFillPath(graphics, brush, path);

        GdipDeletePath(path);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillEllipseI(GpGraphics *graphics, GpBrush *brush, INT x,
    INT y, INT width, INT height)
{
    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, brush, x, y, width, height);

    return GdipFillEllipse(graphics,brush,(REAL)x,(REAL)y,(REAL)width,(REAL)height);
}

static GpStatus GDI32_GdipFillPath(GpGraphics *graphics, GpBrush *brush, GpPath *path)
{
    INT save_state;
    GpStatus retval;

    if(!graphics->hdc || !brush_can_fill_path(brush))
        return NotImplemented;

    save_state = SaveDC(graphics->hdc);
    EndPath(graphics->hdc);
    SetPolyFillMode(graphics->hdc, (path->fill == FillModeAlternate ? ALTERNATE
                                                                    : WINDING));

    BeginPath(graphics->hdc);
    retval = draw_poly(graphics, NULL, path->pathdata.Points,
                       path->pathdata.Types, path->pathdata.Count, FALSE);

    if(retval != Ok)
        goto end;

    EndPath(graphics->hdc);
    brush_fill_path(graphics, brush);

    retval = Ok;

end:
    RestoreDC(graphics->hdc, save_state);

    return retval;
}

static GpStatus SOFTWARE_GdipFillPath(GpGraphics *graphics, GpBrush *brush, GpPath *path)
{
    GpStatus stat;
    GpRegion *rgn;

    if (!brush_can_fill_pixels(brush))
        return NotImplemented;

    /* FIXME: This could probably be done more efficiently without regions. */

    stat = GdipCreateRegionPath(path, &rgn);

    if (stat == Ok)
    {
        stat = GdipFillRegion(graphics, brush, rgn);

        GdipDeleteRegion(rgn);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillPath(GpGraphics *graphics, GpBrush *brush, GpPath *path)
{
    GpStatus stat = NotImplemented;

    TRACE("(%p, %p, %p)\n", graphics, brush, path);

    if(!brush || !graphics || !path)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->image)
        stat = GDI32_GdipFillPath(graphics, brush, path);

    if (stat == NotImplemented)
        stat = SOFTWARE_GdipFillPath(graphics, brush, path);

    if (stat == NotImplemented)
    {
        FIXME("Not implemented for brushtype %i\n", brush->bt);
        stat = Ok;
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillPie(GpGraphics *graphics, GpBrush *brush, REAL x,
    REAL y, REAL width, REAL height, REAL startAngle, REAL sweepAngle)
{
    GpStatus stat;
    GpPath *path;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f)\n",
            graphics, brush, x, y, width, height, startAngle, sweepAngle);

    if(!graphics || !brush)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    stat = GdipCreatePath(FillModeAlternate, &path);

    if (stat == Ok)
    {
        stat = GdipAddPathPie(path, x, y, width, height, startAngle, sweepAngle);

        if (stat == Ok)
            stat = GdipFillPath(graphics, brush, path);

        GdipDeletePath(path);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillPieI(GpGraphics *graphics, GpBrush *brush, INT x,
    INT y, INT width, INT height, REAL startAngle, REAL sweepAngle)
{
    TRACE("(%p, %p, %d, %d, %d, %d, %.2f, %.2f)\n",
            graphics, brush, x, y, width, height, startAngle, sweepAngle);

    return GdipFillPie(graphics,brush,(REAL)x,(REAL)y,(REAL)width,(REAL)height,startAngle,sweepAngle);
}

GpStatus WINGDIPAPI GdipFillPolygon(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPointF *points, INT count, GpFillMode fillMode)
{
    GpStatus stat;
    GpPath *path;

    TRACE("(%p, %p, %p, %d, %d)\n", graphics, brush, points, count, fillMode);

    if(!graphics || !brush || !points || !count)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    stat = GdipCreatePath(fillMode, &path);

    if (stat == Ok)
    {
        stat = GdipAddPathPolygon(path, points, count);

        if (stat == Ok)
            stat = GdipFillPath(graphics, brush, path);

        GdipDeletePath(path);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillPolygonI(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPoint *points, INT count, GpFillMode fillMode)
{
    GpStatus stat;
    GpPath *path;

    TRACE("(%p, %p, %p, %d, %d)\n", graphics, brush, points, count, fillMode);

    if(!graphics || !brush || !points || !count)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    stat = GdipCreatePath(fillMode, &path);

    if (stat == Ok)
    {
        stat = GdipAddPathPolygonI(path, points, count);

        if (stat == Ok)
            stat = GdipFillPath(graphics, brush, path);

        GdipDeletePath(path);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillPolygon2(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPointF *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, brush, points, count);

    return GdipFillPolygon(graphics, brush, points, count, FillModeAlternate);
}

GpStatus WINGDIPAPI GdipFillPolygon2I(GpGraphics *graphics, GpBrush *brush,
    GDIPCONST GpPoint *points, INT count)
{
    TRACE("(%p, %p, %p, %d)\n", graphics, brush, points, count);

    return GdipFillPolygonI(graphics, brush, points, count, FillModeAlternate);
}

GpStatus WINGDIPAPI GdipFillRectangle(GpGraphics *graphics, GpBrush *brush,
    REAL x, REAL y, REAL width, REAL height)
{
    GpStatus stat;
    GpPath *path;

    TRACE("(%p, %p, %.2f, %.2f, %.2f, %.2f)\n", graphics, brush, x, y, width, height);

    if(!graphics || !brush)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    stat = GdipCreatePath(FillModeAlternate, &path);

    if (stat == Ok)
    {
        stat = GdipAddPathRectangle(path, x, y, width, height);

        if (stat == Ok)
            stat = GdipFillPath(graphics, brush, path);

        GdipDeletePath(path);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFillRectangleI(GpGraphics *graphics, GpBrush *brush,
    INT x, INT y, INT width, INT height)
{
    TRACE("(%p, %p, %d, %d, %d, %d)\n", graphics, brush, x, y, width, height);

    return GdipFillRectangle(graphics, brush, x, y, width, height);
}

GpStatus WINGDIPAPI GdipFillRectangles(GpGraphics *graphics, GpBrush *brush, GDIPCONST GpRectF *rects,
    INT count)
{
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, brush, rects, count);

    if(!rects)
        return InvalidParameter;

    for(i = 0; i < count; i++){
        ret = GdipFillRectangle(graphics, brush, rects[i].X, rects[i].Y, rects[i].Width, rects[i].Height);
        if(ret != Ok)   return ret;
    }

    return Ok;
}

GpStatus WINGDIPAPI GdipFillRectanglesI(GpGraphics *graphics, GpBrush *brush, GDIPCONST GpRect *rects,
    INT count)
{
    GpRectF *rectsF;
    GpStatus ret;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, brush, rects, count);

    if(!rects || count <= 0)
        return InvalidParameter;

    rectsF = GdipAlloc(sizeof(GpRectF)*count);
    if(!rectsF)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        rectsF[i].X      = (REAL)rects[i].X;
        rectsF[i].Y      = (REAL)rects[i].Y;
        rectsF[i].X      = (REAL)rects[i].Width;
        rectsF[i].Height = (REAL)rects[i].Height;
    }

    ret = GdipFillRectangles(graphics,brush,rectsF,count);
    GdipFree(rectsF);

    return ret;
}

static GpStatus GDI32_GdipFillRegion(GpGraphics* graphics, GpBrush* brush,
    GpRegion* region)
{
    INT save_state;
    GpStatus status;
    HRGN hrgn;
    RECT rc;

    if(!graphics->hdc || !brush_can_fill_path(brush))
        return NotImplemented;

    status = GdipGetRegionHRgn(region, graphics, &hrgn);
    if(status != Ok)
        return status;

    save_state = SaveDC(graphics->hdc);
    EndPath(graphics->hdc);

    ExtSelectClipRgn(graphics->hdc, hrgn, RGN_AND);

    if (GetClipBox(graphics->hdc, &rc) != NULLREGION)
    {
        BeginPath(graphics->hdc);
        Rectangle(graphics->hdc, rc.left, rc.top, rc.right, rc.bottom);
        EndPath(graphics->hdc);

        brush_fill_path(graphics, brush);
    }

    RestoreDC(graphics->hdc, save_state);

    DeleteObject(hrgn);

    return Ok;
}

static GpStatus SOFTWARE_GdipFillRegion(GpGraphics *graphics, GpBrush *brush,
    GpRegion* region)
{
    GpStatus stat;
    GpRegion *temp_region;
    GpMatrix *world_to_device;
    GpRectF graphics_bounds;
    DWORD *pixel_data;
    HRGN hregion;
    RECT bound_rect;
    GpRect gp_bound_rect;

    if (!brush_can_fill_pixels(brush))
        return NotImplemented;

    stat = get_graphics_bounds(graphics, &graphics_bounds);

    if (stat == Ok)
        stat = GdipCloneRegion(region, &temp_region);

    if (stat == Ok)
    {
        stat = get_graphics_transform(graphics, CoordinateSpaceDevice,
            CoordinateSpaceWorld, &world_to_device);

        if (stat == Ok)
        {
            stat = GdipTransformRegion(temp_region, world_to_device);

            GdipDeleteMatrix(world_to_device);
        }

        if (stat == Ok)
            stat = GdipCombineRegionRect(temp_region, &graphics_bounds, CombineModeIntersect);

        if (stat == Ok)
            stat = GdipGetRegionHRgn(temp_region, NULL, &hregion);

        GdipDeleteRegion(temp_region);
    }

    if (stat == Ok && GetRgnBox(hregion, &bound_rect) == NULLREGION)
    {
        DeleteObject(hregion);
        return Ok;
    }

    if (stat == Ok)
    {
        gp_bound_rect.X = bound_rect.left;
        gp_bound_rect.Y = bound_rect.top;
        gp_bound_rect.Width = bound_rect.right - bound_rect.left;
        gp_bound_rect.Height = bound_rect.bottom - bound_rect.top;

        pixel_data = GdipAlloc(sizeof(*pixel_data) * gp_bound_rect.Width * gp_bound_rect.Height);
        if (!pixel_data)
            stat = OutOfMemory;

        if (stat == Ok)
        {
            stat = brush_fill_pixels(graphics, brush, pixel_data,
                &gp_bound_rect, gp_bound_rect.Width);

            if (stat == Ok)
                stat = alpha_blend_pixels_hrgn(graphics, gp_bound_rect.X,
                    gp_bound_rect.Y, (BYTE*)pixel_data, gp_bound_rect.Width,
                    gp_bound_rect.Height, gp_bound_rect.Width * 4, hregion);

            GdipFree(pixel_data);
        }

        DeleteObject(hregion);
    }

    return stat;
}

/*****************************************************************************
 * GdipFillRegion [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipFillRegion(GpGraphics* graphics, GpBrush* brush,
        GpRegion* region)
{
    GpStatus stat = NotImplemented;

    TRACE("(%p, %p, %p)\n", graphics, brush, region);

    if (!(graphics && brush && region))
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->image)
        stat = GDI32_GdipFillRegion(graphics, brush, region);

    if (stat == NotImplemented)
        stat = SOFTWARE_GdipFillRegion(graphics, brush, region);

    if (stat == NotImplemented)
    {
        FIXME("not implemented for brushtype %i\n", brush->bt);
        stat = Ok;
    }

    return stat;
}

GpStatus WINGDIPAPI GdipFlush(GpGraphics *graphics, GpFlushIntention intention)
{
    TRACE("(%p,%u)\n", graphics, intention);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    /* We have no internal operation queue, so there's no need to clear it. */

    if (graphics->hdc)
        GdiFlush();

    return Ok;
}

/*****************************************************************************
 * GdipGetClipBounds [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipGetClipBounds(GpGraphics *graphics, GpRectF *rect)
{
    TRACE("(%p, %p)\n", graphics, rect);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipGetRegionBounds(graphics->clip, graphics, rect);
}

/*****************************************************************************
 * GdipGetClipBoundsI [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipGetClipBoundsI(GpGraphics *graphics, GpRect *rect)
{
    TRACE("(%p, %p)\n", graphics, rect);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipGetRegionBoundsI(graphics->clip, graphics, rect);
}

/* FIXME: Compositing mode is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetCompositingMode(GpGraphics *graphics,
    CompositingMode *mode)
{
    TRACE("(%p, %p)\n", graphics, mode);

    if(!graphics || !mode)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *mode = graphics->compmode;

    return Ok;
}

/* FIXME: Compositing quality is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetCompositingQuality(GpGraphics *graphics,
    CompositingQuality *quality)
{
    TRACE("(%p, %p)\n", graphics, quality);

    if(!graphics || !quality)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *quality = graphics->compqual;

    return Ok;
}

/* FIXME: Interpolation mode is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetInterpolationMode(GpGraphics *graphics,
    InterpolationMode *mode)
{
    TRACE("(%p, %p)\n", graphics, mode);

    if(!graphics || !mode)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *mode = graphics->interpolation;

    return Ok;
}

/* FIXME: Need to handle color depths less than 24bpp */
GpStatus WINGDIPAPI GdipGetNearestColor(GpGraphics *graphics, ARGB* argb)
{
    FIXME("(%p, %p): Passing color unmodified\n", graphics, argb);

    if(!graphics || !argb)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return Ok;
}

GpStatus WINGDIPAPI GdipGetPageScale(GpGraphics *graphics, REAL *scale)
{
    TRACE("(%p, %p)\n", graphics, scale);

    if(!graphics || !scale)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *scale = graphics->scale;

    return Ok;
}

GpStatus WINGDIPAPI GdipGetPageUnit(GpGraphics *graphics, GpUnit *unit)
{
    TRACE("(%p, %p)\n", graphics, unit);

    if(!graphics || !unit)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *unit = graphics->unit;

    return Ok;
}

/* FIXME: Pixel offset mode is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetPixelOffsetMode(GpGraphics *graphics, PixelOffsetMode
    *mode)
{
    TRACE("(%p, %p)\n", graphics, mode);

    if(!graphics || !mode)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *mode = graphics->pixeloffset;

    return Ok;
}

/* FIXME: Smoothing mode is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetSmoothingMode(GpGraphics *graphics, SmoothingMode *mode)
{
    TRACE("(%p, %p)\n", graphics, mode);

    if(!graphics || !mode)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *mode = graphics->smoothing;

    return Ok;
}

GpStatus WINGDIPAPI GdipGetTextContrast(GpGraphics *graphics, UINT *contrast)
{
    TRACE("(%p, %p)\n", graphics, contrast);

    if(!graphics || !contrast)
        return InvalidParameter;

    *contrast = graphics->textcontrast;

    return Ok;
}

/* FIXME: Text rendering hint is not used anywhere except the getter/setter. */
GpStatus WINGDIPAPI GdipGetTextRenderingHint(GpGraphics *graphics,
    TextRenderingHint *hint)
{
    TRACE("(%p, %p)\n", graphics, hint);

    if(!graphics || !hint)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *hint = graphics->texthint;

    return Ok;
}

GpStatus WINGDIPAPI GdipGetVisibleClipBounds(GpGraphics *graphics, GpRectF *rect)
{
    GpRegion *clip_rgn;
    GpStatus stat;

    TRACE("(%p, %p)\n", graphics, rect);

    if(!graphics || !rect)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    /* intersect window and graphics clipping regions */
    if((stat = GdipCreateRegion(&clip_rgn)) != Ok)
        return stat;

    if((stat = get_visible_clip_region(graphics, clip_rgn)) != Ok)
        goto cleanup;

    /* get bounds of the region */
    stat = GdipGetRegionBounds(clip_rgn, graphics, rect);

cleanup:
    GdipDeleteRegion(clip_rgn);

    return stat;
}

GpStatus WINGDIPAPI GdipGetVisibleClipBoundsI(GpGraphics *graphics, GpRect *rect)
{
    GpRectF rectf;
    GpStatus stat;

    TRACE("(%p, %p)\n", graphics, rect);

    if(!graphics || !rect)
        return InvalidParameter;

    if((stat = GdipGetVisibleClipBounds(graphics, &rectf)) == Ok)
    {
        rect->X = gdip_round(rectf.X);
        rect->Y = gdip_round(rectf.Y);
        rect->Width  = gdip_round(rectf.Width);
        rect->Height = gdip_round(rectf.Height);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipGetWorldTransform(GpGraphics *graphics, GpMatrix *matrix)
{
    TRACE("(%p, %p)\n", graphics, matrix);

    if(!graphics || !matrix)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *matrix = *graphics->worldtrans;
    return Ok;
}

GpStatus WINGDIPAPI GdipGraphicsClear(GpGraphics *graphics, ARGB color)
{
    GpSolidFill *brush;
    GpStatus stat;
    GpRectF wnd_rect;

    TRACE("(%p, %x)\n", graphics, color);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if((stat = GdipCreateSolidFill(color, &brush)) != Ok)
        return stat;

    if((stat = get_graphics_bounds(graphics, &wnd_rect)) != Ok){
        GdipDeleteBrush((GpBrush*)brush);
        return stat;
    }

    GdipFillRectangle(graphics, (GpBrush*)brush, wnd_rect.X, wnd_rect.Y,
                                                 wnd_rect.Width, wnd_rect.Height);

    GdipDeleteBrush((GpBrush*)brush);

    return Ok;
}

GpStatus WINGDIPAPI GdipIsClipEmpty(GpGraphics *graphics, BOOL *res)
{
    TRACE("(%p, %p)\n", graphics, res);

    if(!graphics || !res)
        return InvalidParameter;

    return GdipIsEmptyRegion(graphics->clip, graphics, res);
}

GpStatus WINGDIPAPI GdipIsVisiblePoint(GpGraphics *graphics, REAL x, REAL y, BOOL *result)
{
    GpStatus stat;
    GpRegion* rgn;
    GpPointF pt;

    TRACE("(%p, %.2f, %.2f, %p)\n", graphics, x, y, result);

    if(!graphics || !result)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    pt.X = x;
    pt.Y = y;
    if((stat = GdipTransformPoints(graphics, CoordinateSpaceDevice,
                   CoordinateSpaceWorld, &pt, 1)) != Ok)
        return stat;

    if((stat = GdipCreateRegion(&rgn)) != Ok)
        return stat;

    if((stat = get_visible_clip_region(graphics, rgn)) != Ok)
        goto cleanup;

    stat = GdipIsVisibleRegionPoint(rgn, pt.X, pt.Y, graphics, result);

cleanup:
    GdipDeleteRegion(rgn);
    return stat;
}

GpStatus WINGDIPAPI GdipIsVisiblePointI(GpGraphics *graphics, INT x, INT y, BOOL *result)
{
    return GdipIsVisiblePoint(graphics, (REAL)x, (REAL)y, result);
}

GpStatus WINGDIPAPI GdipIsVisibleRect(GpGraphics *graphics, REAL x, REAL y, REAL width, REAL height, BOOL *result)
{
    GpStatus stat;
    GpRegion* rgn;
    GpPointF pts[2];

    TRACE("(%p %.2f %.2f %.2f %.2f %p)\n", graphics, x, y, width, height, result);

    if(!graphics || !result)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    pts[0].X = x;
    pts[0].Y = y;
    pts[1].X = x + width;
    pts[1].Y = y + height;

    if((stat = GdipTransformPoints(graphics, CoordinateSpaceDevice,
                    CoordinateSpaceWorld, pts, 2)) != Ok)
        return stat;

    pts[1].X -= pts[0].X;
    pts[1].Y -= pts[0].Y;

    if((stat = GdipCreateRegion(&rgn)) != Ok)
        return stat;

    if((stat = get_visible_clip_region(graphics, rgn)) != Ok)
        goto cleanup;

    stat = GdipIsVisibleRegionRect(rgn, pts[0].X, pts[0].Y, pts[1].X, pts[1].Y, graphics, result);

cleanup:
    GdipDeleteRegion(rgn);
    return stat;
}

GpStatus WINGDIPAPI GdipIsVisibleRectI(GpGraphics *graphics, INT x, INT y, INT width, INT height, BOOL *result)
{
    return GdipIsVisibleRect(graphics, (REAL)x, (REAL)y, (REAL)width, (REAL)height, result);
}

GpStatus gdip_format_string(HDC hdc,
    GDIPCONST WCHAR *string, INT length, GDIPCONST GpFont *font,
    GDIPCONST RectF *rect, GDIPCONST GpStringFormat *format,
    gdip_format_string_callback callback, void *user_data)
{
    WCHAR* stringdup;
    int sum = 0, height = 0, fit, fitcpy, i, j, lret, nwidth,
        nheight, lineend, lineno = 0;
    RectF bounds;
    StringAlignment halign;
    GpStatus stat = Ok;
    SIZE size;
    HotkeyPrefix hkprefix;
    INT *hotkeyprefix_offsets=NULL;
    INT hotkeyprefix_count=0;
    INT hotkeyprefix_pos=0, hotkeyprefix_end_pos=0;
    int seen_prefix=0;

    if(length == -1) length = lstrlenW(string);

    stringdup = GdipAlloc((length + 1) * sizeof(WCHAR));
    if(!stringdup) return OutOfMemory;

    nwidth = rect->Width;
    nheight = rect->Height;

    if (format)
        hkprefix = format->hkprefix;
    else
        hkprefix = HotkeyPrefixNone;

    if (hkprefix == HotkeyPrefixShow)
    {
        for (i=0; i<length; i++)
        {
            if (string[i] == '&')
                hotkeyprefix_count++;
        }
    }

    if (hotkeyprefix_count)
        hotkeyprefix_offsets = GdipAlloc(sizeof(INT) * hotkeyprefix_count);

    hotkeyprefix_count = 0;

    for(i = 0, j = 0; i < length; i++){
        /* FIXME: This makes the indexes passed to callback inaccurate. */
        if(!isprintW(string[i]) && (string[i] != '\n'))
            continue;

        /* FIXME: tabs should be handled using tabstops from stringformat */
        if (string[i] == '\t')
            continue;

        if (seen_prefix && hkprefix == HotkeyPrefixShow && string[i] != '&')
            hotkeyprefix_offsets[hotkeyprefix_count++] = j;
        else if (!seen_prefix && hkprefix != HotkeyPrefixNone && string[i] == '&')
        {
            seen_prefix = 1;
            continue;
        }

        seen_prefix = 0;

        stringdup[j] = string[i];
        j++;
    }

    length = j;

    if (format) halign = format->align;
    else halign = StringAlignmentNear;

    while(sum < length){
        GetTextExtentExPointW(hdc, stringdup + sum, length - sum,
                              nwidth, &fit, NULL, &size);
        fitcpy = fit;

        if(fit == 0)
            break;

        for(lret = 0; lret < fit; lret++)
            if(*(stringdup + sum + lret) == '\n')
                break;

        /* Line break code (may look strange, but it imitates windows). */
        if(lret < fit)
            lineend = fit = lret;    /* this is not an off-by-one error */
        else if(fit < (length - sum)){
            if(*(stringdup + sum + fit) == ' ')
                while(*(stringdup + sum + fit) == ' ')
                    fit++;
            else
                while(*(stringdup + sum + fit - 1) != ' '){
                    fit--;

                    if(*(stringdup + sum + fit) == '\t')
                        break;

                    if(fit == 0){
                        fit = fitcpy;
                        break;
                    }
                }
            lineend = fit;
            while(*(stringdup + sum + lineend - 1) == ' ' ||
                  *(stringdup + sum + lineend - 1) == '\t')
                lineend--;
        }
        else
            lineend = fit;

        GetTextExtentExPointW(hdc, stringdup + sum, lineend,
                              nwidth, &j, NULL, &size);

        bounds.Width = size.cx;

        if(height + size.cy > nheight)
            bounds.Height = nheight - (height + size.cy);
        else
            bounds.Height = size.cy;

        bounds.Y = rect->Y + height;

        switch (halign)
        {
        case StringAlignmentNear:
        default:
            bounds.X = rect->X;
            break;
        case StringAlignmentCenter:
            bounds.X = rect->X + (rect->Width/2) - (bounds.Width/2);
            break;
        case StringAlignmentFar:
            bounds.X = rect->X + rect->Width - bounds.Width;
            break;
        }

        for (hotkeyprefix_end_pos=hotkeyprefix_pos; hotkeyprefix_end_pos<hotkeyprefix_count; hotkeyprefix_end_pos++)
            if (hotkeyprefix_offsets[hotkeyprefix_end_pos] >= sum + lineend)
                break;

        stat = callback(hdc, stringdup, sum, lineend,
            font, rect, format, lineno, &bounds,
            &hotkeyprefix_offsets[hotkeyprefix_pos],
            hotkeyprefix_end_pos-hotkeyprefix_pos, user_data);

        if (stat != Ok)
            break;

        sum += fit + (lret < fitcpy ? 1 : 0);
        height += size.cy;
        lineno++;

        hotkeyprefix_pos = hotkeyprefix_end_pos;

        if(height > nheight)
            break;

        /* Stop if this was a linewrap (but not if it was a linebreak). */
        if ((lret == fitcpy) && format &&
            (format->attr & (StringFormatFlagsNoWrap | StringFormatFlagsLineLimit)))
            break;
    }

    GdipFree(stringdup);
    GdipFree(hotkeyprefix_offsets);

    return stat;
}

struct measure_ranges_args {
    GpRegion **regions;
    REAL rel_width, rel_height;
};

static GpStatus measure_ranges_callback(HDC hdc,
    GDIPCONST WCHAR *string, INT index, INT length, GDIPCONST GpFont *font,
    GDIPCONST RectF *rect, GDIPCONST GpStringFormat *format,
    INT lineno, const RectF *bounds, INT *underlined_indexes,
    INT underlined_index_count, void *user_data)
{
    int i;
    GpStatus stat = Ok;
    struct measure_ranges_args *args = user_data;

    for (i=0; i<format->range_count; i++)
    {
        INT range_start = max(index, format->character_ranges[i].First);
        INT range_end = min(index+length, format->character_ranges[i].First+format->character_ranges[i].Length);
        if (range_start < range_end)
        {
            GpRectF range_rect;
            SIZE range_size;

            range_rect.Y = bounds->Y / args->rel_height;
            range_rect.Height = bounds->Height / args->rel_height;

            GetTextExtentExPointW(hdc, string + index, range_start - index,
                                  INT_MAX, NULL, NULL, &range_size);
            range_rect.X = (bounds->X + range_size.cx) / args->rel_width;

            GetTextExtentExPointW(hdc, string + index, range_end - index,
                                  INT_MAX, NULL, NULL, &range_size);
            range_rect.Width = (bounds->X + range_size.cx) / args->rel_width - range_rect.X;

            stat = GdipCombineRegionRect(args->regions[i], &range_rect, CombineModeUnion);
            if (stat != Ok)
                break;
        }
    }

    return stat;
}

GpStatus WINGDIPAPI GdipMeasureCharacterRanges(GpGraphics* graphics,
        GDIPCONST WCHAR* string, INT length, GDIPCONST GpFont* font,
        GDIPCONST RectF* layoutRect, GDIPCONST GpStringFormat *stringFormat,
        INT regionCount, GpRegion** regions)
{
    GpStatus stat;
    int i;
    HFONT gdifont, oldfont;
    struct measure_ranges_args args;
    HDC hdc, temp_hdc=NULL;
    GpPointF pt[3];
    RectF scaled_rect;

    TRACE("(%p %s %d %p %s %p %d %p)\n", graphics, debugstr_w(string),
            length, font, debugstr_rectf(layoutRect), stringFormat, regionCount, regions);

    if (!(graphics && string && font && layoutRect && stringFormat && regions))
        return InvalidParameter;

    if (regionCount < stringFormat->range_count)
        return InvalidParameter;

    if(!graphics->hdc)
    {
        hdc = temp_hdc = CreateCompatibleDC(0);
        if (!temp_hdc) return OutOfMemory;
    }
    else
        hdc = graphics->hdc;

    if (stringFormat->attr)
        TRACE("may be ignoring some format flags: attr %x\n", stringFormat->attr);

    pt[0].X = 0.0;
    pt[0].Y = 0.0;
    pt[1].X = 1.0;
    pt[1].Y = 0.0;
    pt[2].X = 0.0;
    pt[2].Y = 1.0;
    GdipTransformPoints(graphics, CoordinateSpaceDevice, CoordinateSpaceWorld, pt, 3);
    args.rel_width = sqrt((pt[1].Y-pt[0].Y)*(pt[1].Y-pt[0].Y)+
                     (pt[1].X-pt[0].X)*(pt[1].X-pt[0].X));
    args.rel_height = sqrt((pt[2].Y-pt[0].Y)*(pt[2].Y-pt[0].Y)+
                      (pt[2].X-pt[0].X)*(pt[2].X-pt[0].X));

    scaled_rect.X = layoutRect->X * args.rel_width;
    scaled_rect.Y = layoutRect->Y * args.rel_height;
    scaled_rect.Width = layoutRect->Width * args.rel_width;
    scaled_rect.Height = layoutRect->Height * args.rel_height;

    get_font_hfont(graphics, font, &gdifont);
    oldfont = SelectObject(hdc, gdifont);

    for (i=0; i<stringFormat->range_count; i++)
    {
        stat = GdipSetEmpty(regions[i]);
        if (stat != Ok)
            return stat;
    }

    args.regions = regions;

    stat = gdip_format_string(hdc, string, length, font, &scaled_rect, stringFormat,
        measure_ranges_callback, &args);

    SelectObject(hdc, oldfont);
    DeleteObject(gdifont);

    if (temp_hdc)
        DeleteDC(temp_hdc);

    return stat;
}

struct measure_string_args {
    RectF *bounds;
    INT *codepointsfitted;
    INT *linesfilled;
    REAL rel_width, rel_height;
};

static GpStatus measure_string_callback(HDC hdc,
    GDIPCONST WCHAR *string, INT index, INT length, GDIPCONST GpFont *font,
    GDIPCONST RectF *rect, GDIPCONST GpStringFormat *format,
    INT lineno, const RectF *bounds, INT *underlined_indexes,
    INT underlined_index_count, void *user_data)
{
    struct measure_string_args *args = user_data;
    REAL new_width, new_height;

    new_width = bounds->Width / args->rel_width;
    new_height = (bounds->Height + bounds->Y) / args->rel_height - args->bounds->Y;

    if (new_width > args->bounds->Width)
        args->bounds->Width = new_width;

    if (new_height > args->bounds->Height)
        args->bounds->Height = new_height;

    if (args->codepointsfitted)
        *args->codepointsfitted = index + length;

    if (args->linesfilled)
        (*args->linesfilled)++;

    return Ok;
}

/* Find the smallest rectangle that bounds the text when it is printed in rect
 * according to the format options listed in format. If rect has 0 width and
 * height, then just find the smallest rectangle that bounds the text when it's
 * printed at location (rect->X, rect-Y). */
GpStatus WINGDIPAPI GdipMeasureString(GpGraphics *graphics,
    GDIPCONST WCHAR *string, INT length, GDIPCONST GpFont *font,
    GDIPCONST RectF *rect, GDIPCONST GpStringFormat *format, RectF *bounds,
    INT *codepointsfitted, INT *linesfilled)
{
    HFONT oldfont, gdifont;
    struct measure_string_args args;
    HDC temp_hdc=NULL, hdc;
    GpPointF pt[3];
    RectF scaled_rect;

    TRACE("(%p, %s, %i, %p, %s, %p, %p, %p, %p)\n", graphics,
        debugstr_wn(string, length), length, font, debugstr_rectf(rect), format,
        bounds, codepointsfitted, linesfilled);

    if(!graphics || !string || !font || !rect || !bounds)
        return InvalidParameter;

    if(!graphics->hdc)
    {
        hdc = temp_hdc = CreateCompatibleDC(0);
        if (!temp_hdc) return OutOfMemory;
    }
    else
        hdc = graphics->hdc;

    if(linesfilled) *linesfilled = 0;
    if(codepointsfitted) *codepointsfitted = 0;

    if(format)
        TRACE("may be ignoring some format flags: attr %x\n", format->attr);

    pt[0].X = 0.0;
    pt[0].Y = 0.0;
    pt[1].X = 1.0;
    pt[1].Y = 0.0;
    pt[2].X = 0.0;
    pt[2].Y = 1.0;
    GdipTransformPoints(graphics, CoordinateSpaceDevice, CoordinateSpaceWorld, pt, 3);
    args.rel_width = sqrt((pt[1].Y-pt[0].Y)*(pt[1].Y-pt[0].Y)+
                     (pt[1].X-pt[0].X)*(pt[1].X-pt[0].X));
    args.rel_height = sqrt((pt[2].Y-pt[0].Y)*(pt[2].Y-pt[0].Y)+
                      (pt[2].X-pt[0].X)*(pt[2].X-pt[0].X));

    get_font_hfont(graphics, font, &gdifont);
    oldfont = SelectObject(hdc, gdifont);

    scaled_rect.X = rect->X * args.rel_width;
    scaled_rect.Y = rect->Y * args.rel_height;
    scaled_rect.Width = rect->Width * args.rel_width;
    scaled_rect.Height = rect->Height * args.rel_height;

    if (scaled_rect.Width >= INT_MAX || scaled_rect.Width < 0.5) scaled_rect.Width = (REAL)(1 << 23);
    if (scaled_rect.Height >= INT_MAX || scaled_rect.Height < 0.5) scaled_rect.Height = (REAL)(1 << 23);

    bounds->X = rect->X;
    bounds->Y = rect->Y;
    bounds->Width = 0.0;
    bounds->Height = 0.0;

    args.bounds = bounds;
    args.codepointsfitted = codepointsfitted;
    args.linesfilled = linesfilled;

    gdip_format_string(hdc, string, length, font, &scaled_rect, format,
        measure_string_callback, &args);

    SelectObject(hdc, oldfont);
    DeleteObject(gdifont);

    if (temp_hdc)
        DeleteDC(temp_hdc);

    return Ok;
}

struct draw_string_args {
    GpGraphics *graphics;
    GDIPCONST GpBrush *brush;
    REAL x, y, rel_width, rel_height, ascent;
};

static GpStatus draw_string_callback(HDC hdc,
    GDIPCONST WCHAR *string, INT index, INT length, GDIPCONST GpFont *font,
    GDIPCONST RectF *rect, GDIPCONST GpStringFormat *format,
    INT lineno, const RectF *bounds, INT *underlined_indexes,
    INT underlined_index_count, void *user_data)
{
    struct draw_string_args *args = user_data;
    PointF position;
    GpStatus stat;

    position.X = args->x + bounds->X / args->rel_width;
    position.Y = args->y + bounds->Y / args->rel_height + args->ascent;

    stat = GdipDrawDriverString(args->graphics, &string[index], length, font,
        args->brush, &position,
        DriverStringOptionsCmapLookup|DriverStringOptionsRealizedAdvance, NULL);

    if (stat == Ok && underlined_index_count)
    {
        OUTLINETEXTMETRICW otm;
        REAL underline_y, underline_height;
        int i;

        GetOutlineTextMetricsW(hdc, sizeof(otm), &otm);

        underline_height = otm.otmsUnderscoreSize / args->rel_height;
        underline_y = position.Y - otm.otmsUnderscorePosition / args->rel_height - underline_height / 2;

        for (i=0; i<underlined_index_count; i++)
        {
            REAL start_x, end_x;
            SIZE text_size;
            INT ofs = underlined_indexes[i] - index;

            GetTextExtentExPointW(hdc, string + index, ofs, INT_MAX, NULL, NULL, &text_size);
            start_x = text_size.cx / args->rel_width;

            GetTextExtentExPointW(hdc, string + index, ofs+1, INT_MAX, NULL, NULL, &text_size);
            end_x = text_size.cx / args->rel_width;

            GdipFillRectangle(args->graphics, (GpBrush*)args->brush, position.X+start_x, underline_y, end_x-start_x, underline_height);
        }
    }

    return stat;
}

GpStatus WINGDIPAPI GdipDrawString(GpGraphics *graphics, GDIPCONST WCHAR *string,
    INT length, GDIPCONST GpFont *font, GDIPCONST RectF *rect,
    GDIPCONST GpStringFormat *format, GDIPCONST GpBrush *brush)
{
    HRGN rgn = NULL;
    HFONT gdifont;
    GpPointF pt[3], rectcpy[4];
    POINT corners[4];
    REAL rel_width, rel_height;
    INT save_state;
    REAL offsety = 0.0;
    struct draw_string_args args;
    RectF scaled_rect;
    HDC hdc, temp_hdc=NULL;
    TEXTMETRICW textmetric;

    TRACE("(%p, %s, %i, %p, %s, %p, %p)\n", graphics, debugstr_wn(string, length),
        length, font, debugstr_rectf(rect), format, brush);

    if(!graphics || !string || !font || !brush || !rect)
        return InvalidParameter;

    if(graphics->hdc)
    {
        hdc = graphics->hdc;
    }
    else
    {
        hdc = temp_hdc = CreateCompatibleDC(0);
    }

    if(format){
        TRACE("may be ignoring some format flags: attr %x\n", format->attr);

        /* Should be no need to explicitly test for StringAlignmentNear as
         * that is default behavior if no alignment is passed. */
        if(format->vertalign != StringAlignmentNear){
            RectF bounds, in_rect = *rect;
            in_rect.Height = 0.0; /* avoid height clipping */
            GdipMeasureString(graphics, string, length, font, &in_rect, format, &bounds, 0, 0);

            TRACE("bounds %s\n", debugstr_rectf(&bounds));

            if(format->vertalign == StringAlignmentCenter)
                offsety = (rect->Height - bounds.Height) / 2;
            else if(format->vertalign == StringAlignmentFar)
                offsety = (rect->Height - bounds.Height);
        }
        TRACE("vertical align %d, offsety %f\n", format->vertalign, offsety);
    }

    save_state = SaveDC(hdc);

    pt[0].X = 0.0;
    pt[0].Y = 0.0;
    pt[1].X = 1.0;
    pt[1].Y = 0.0;
    pt[2].X = 0.0;
    pt[2].Y = 1.0;
    GdipTransformPoints(graphics, CoordinateSpaceDevice, CoordinateSpaceWorld, pt, 3);
    rel_width = sqrt((pt[1].Y-pt[0].Y)*(pt[1].Y-pt[0].Y)+
                     (pt[1].X-pt[0].X)*(pt[1].X-pt[0].X));
    rel_height = sqrt((pt[2].Y-pt[0].Y)*(pt[2].Y-pt[0].Y)+
                      (pt[2].X-pt[0].X)*(pt[2].X-pt[0].X));

    rectcpy[3].X = rectcpy[0].X = rect->X;
    rectcpy[1].Y = rectcpy[0].Y = rect->Y;
    rectcpy[2].X = rectcpy[1].X = rect->X + rect->Width;
    rectcpy[3].Y = rectcpy[2].Y = rect->Y + rect->Height;
    transform_and_round_points(graphics, corners, rectcpy, 4);

    scaled_rect.X = 0.0;
    scaled_rect.Y = 0.0;
    scaled_rect.Width = rel_width * rect->Width;
    scaled_rect.Height = rel_height * rect->Height;

    if (scaled_rect.Width >= INT_MAX || scaled_rect.Width < 0.5) scaled_rect.Width = (REAL)(1 << 23);
    if (scaled_rect.Height >= INT_MAX || scaled_rect.Height < 0.5) scaled_rect.Height = (REAL)(1 << 23);

    if (gdip_round(scaled_rect.Width) != 0 && gdip_round(scaled_rect.Height) != 0)
    {
        /* FIXME: If only the width or only the height is 0, we should probably still clip */
        rgn = CreatePolygonRgn(corners, 4, ALTERNATE);
        SelectClipRgn(hdc, rgn);
    }

    get_font_hfont(graphics, font, &gdifont);
    SelectObject(hdc, gdifont);

    args.graphics = graphics;
    args.brush = brush;

    args.x = rect->X;
    args.y = rect->Y + offsety;

    args.rel_width = rel_width;
    args.rel_height = rel_height;

    GetTextMetricsW(hdc, &textmetric);
    args.ascent = textmetric.tmAscent / rel_height;

    gdip_format_string(hdc, string, length, font, &scaled_rect, format,
        draw_string_callback, &args);

    DeleteObject(rgn);
    DeleteObject(gdifont);

    RestoreDC(hdc, save_state);

    DeleteDC(temp_hdc);

    return Ok;
}

GpStatus WINGDIPAPI GdipResetClip(GpGraphics *graphics)
{
    TRACE("(%p)\n", graphics);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipSetInfinite(graphics->clip);
}

GpStatus WINGDIPAPI GdipResetWorldTransform(GpGraphics *graphics)
{
    TRACE("(%p)\n", graphics);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    graphics->worldtrans->matrix[0] = 1.0;
    graphics->worldtrans->matrix[1] = 0.0;
    graphics->worldtrans->matrix[2] = 0.0;
    graphics->worldtrans->matrix[3] = 1.0;
    graphics->worldtrans->matrix[4] = 0.0;
    graphics->worldtrans->matrix[5] = 0.0;

    return Ok;
}

GpStatus WINGDIPAPI GdipRestoreGraphics(GpGraphics *graphics, GraphicsState state)
{
    return GdipEndContainer(graphics, state);
}

GpStatus WINGDIPAPI GdipRotateWorldTransform(GpGraphics *graphics, REAL angle,
    GpMatrixOrder order)
{
    TRACE("(%p, %.2f, %d)\n", graphics, angle, order);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipRotateMatrix(graphics->worldtrans, angle, order);
}

GpStatus WINGDIPAPI GdipSaveGraphics(GpGraphics *graphics, GraphicsState *state)
{
    return GdipBeginContainer2(graphics, state);
}

GpStatus WINGDIPAPI GdipBeginContainer2(GpGraphics *graphics,
        GraphicsContainer *state)
{
    GraphicsContainerItem *container;
    GpStatus sts;

    TRACE("(%p, %p)\n", graphics, state);

    if(!graphics || !state)
        return InvalidParameter;

    sts = init_container(&container, graphics);
    if(sts != Ok)
        return sts;

    list_add_head(&graphics->containers, &container->entry);
    *state = graphics->contid = container->contid;

    return Ok;
}

GpStatus WINGDIPAPI GdipBeginContainer(GpGraphics *graphics, GDIPCONST GpRectF *dstrect, GDIPCONST GpRectF *srcrect, GpUnit unit, GraphicsContainer *state)
{
    FIXME("(%p, %p, %p, %d, %p): stub\n", graphics, dstrect, srcrect, unit, state);
    return NotImplemented;
}

GpStatus WINGDIPAPI GdipBeginContainerI(GpGraphics *graphics, GDIPCONST GpRect *dstrect, GDIPCONST GpRect *srcrect, GpUnit unit, GraphicsContainer *state)
{
    FIXME("(%p, %p, %p, %d, %p): stub\n", graphics, dstrect, srcrect, unit, state);
    return NotImplemented;
}

GpStatus WINGDIPAPI GdipComment(GpGraphics *graphics, UINT sizeData, GDIPCONST BYTE *data)
{
    FIXME("(%p, %d, %p): stub\n", graphics, sizeData, data);
    return NotImplemented;
}

GpStatus WINGDIPAPI GdipEndContainer(GpGraphics *graphics, GraphicsContainer state)
{
    GpStatus sts;
    GraphicsContainerItem *container, *container2;

    TRACE("(%p, %x)\n", graphics, state);

    if(!graphics)
        return InvalidParameter;

    LIST_FOR_EACH_ENTRY(container, &graphics->containers, GraphicsContainerItem, entry){
        if(container->contid == state)
            break;
    }

    /* did not find a matching container */
    if(&container->entry == &graphics->containers)
        return Ok;

    sts = restore_container(graphics, container);
    if(sts != Ok)
        return sts;

    /* remove all of the containers on top of the found container */
    LIST_FOR_EACH_ENTRY_SAFE(container, container2, &graphics->containers, GraphicsContainerItem, entry){
        if(container->contid == state)
            break;
        list_remove(&container->entry);
        delete_container(container);
    }

    list_remove(&container->entry);
    delete_container(container);

    return Ok;
}

GpStatus WINGDIPAPI GdipScaleWorldTransform(GpGraphics *graphics, REAL sx,
    REAL sy, GpMatrixOrder order)
{
    TRACE("(%p, %.2f, %.2f, %d)\n", graphics, sx, sy, order);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipScaleMatrix(graphics->worldtrans, sx, sy, order);
}

GpStatus WINGDIPAPI GdipSetClipGraphics(GpGraphics *graphics, GpGraphics *srcgraphics,
    CombineMode mode)
{
    TRACE("(%p, %p, %d)\n", graphics, srcgraphics, mode);

    if(!graphics || !srcgraphics)
        return InvalidParameter;

    return GdipCombineRegionRegion(graphics->clip, srcgraphics->clip, mode);
}

GpStatus WINGDIPAPI GdipSetCompositingMode(GpGraphics *graphics,
    CompositingMode mode)
{
    TRACE("(%p, %d)\n", graphics, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    graphics->compmode = mode;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetCompositingQuality(GpGraphics *graphics,
    CompositingQuality quality)
{
    TRACE("(%p, %d)\n", graphics, quality);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    graphics->compqual = quality;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetInterpolationMode(GpGraphics *graphics,
    InterpolationMode mode)
{
    TRACE("(%p, %d)\n", graphics, mode);

    if(!graphics || mode == InterpolationModeInvalid || mode > InterpolationModeHighQualityBicubic)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (mode == InterpolationModeDefault || mode == InterpolationModeLowQuality)
        mode = InterpolationModeBilinear;

    if (mode == InterpolationModeHighQuality)
        mode = InterpolationModeHighQualityBicubic;

    graphics->interpolation = mode;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetPageScale(GpGraphics *graphics, REAL scale)
{
    TRACE("(%p, %.2f)\n", graphics, scale);

    if(!graphics || (scale <= 0.0))
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    graphics->scale = scale;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetPageUnit(GpGraphics *graphics, GpUnit unit)
{
    TRACE("(%p, %d)\n", graphics, unit);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if(unit == UnitWorld)
        return InvalidParameter;

    graphics->unit = unit;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetPixelOffsetMode(GpGraphics *graphics, PixelOffsetMode
    mode)
{
    TRACE("(%p, %d)\n", graphics, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    graphics->pixeloffset = mode;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetRenderingOrigin(GpGraphics *graphics, INT x, INT y)
{
    static int calls;

    TRACE("(%p,%i,%i)\n", graphics, x, y);

    if (!(calls++))
        FIXME("value is unused in rendering\n");

    if (!graphics)
        return InvalidParameter;

    graphics->origin_x = x;
    graphics->origin_y = y;

    return Ok;
}

GpStatus WINGDIPAPI GdipGetRenderingOrigin(GpGraphics *graphics, INT *x, INT *y)
{
    TRACE("(%p,%p,%p)\n", graphics, x, y);

    if (!graphics || !x || !y)
        return InvalidParameter;

    *x = graphics->origin_x;
    *y = graphics->origin_y;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetSmoothingMode(GpGraphics *graphics, SmoothingMode mode)
{
    TRACE("(%p, %d)\n", graphics, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    graphics->smoothing = mode;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetTextContrast(GpGraphics *graphics, UINT contrast)
{
    TRACE("(%p, %d)\n", graphics, contrast);

    if(!graphics)
        return InvalidParameter;

    graphics->textcontrast = contrast;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetTextRenderingHint(GpGraphics *graphics,
    TextRenderingHint hint)
{
    TRACE("(%p, %d)\n", graphics, hint);

    if(!graphics || hint > TextRenderingHintClearTypeGridFit)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    graphics->texthint = hint;

    return Ok;
}

GpStatus WINGDIPAPI GdipSetWorldTransform(GpGraphics *graphics, GpMatrix *matrix)
{
    TRACE("(%p, %p)\n", graphics, matrix);

    if(!graphics || !matrix)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    TRACE("%f,%f,%f,%f,%f,%f\n",
          matrix->matrix[0], matrix->matrix[1], matrix->matrix[2],
          matrix->matrix[3], matrix->matrix[4], matrix->matrix[5]);

    GdipDeleteMatrix(graphics->worldtrans);
    return GdipCloneMatrix(matrix, &graphics->worldtrans);
}

GpStatus WINGDIPAPI GdipTranslateWorldTransform(GpGraphics *graphics, REAL dx,
    REAL dy, GpMatrixOrder order)
{
    TRACE("(%p, %.2f, %.2f, %d)\n", graphics, dx, dy, order);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipTranslateMatrix(graphics->worldtrans, dx, dy, order);
}

/*****************************************************************************
 * GdipSetClipHrgn [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipSetClipHrgn(GpGraphics *graphics, HRGN hrgn, CombineMode mode)
{
    GpRegion *region;
    GpStatus status;

    TRACE("(%p, %p, %d)\n", graphics, hrgn, mode);

    if(!graphics)
        return InvalidParameter;

    status = GdipCreateRegionHrgn(hrgn, &region);
    if(status != Ok)
        return status;

    status = GdipSetClipRegion(graphics, region, mode);

    GdipDeleteRegion(region);
    return status;
}

GpStatus WINGDIPAPI GdipSetClipPath(GpGraphics *graphics, GpPath *path, CombineMode mode)
{
    TRACE("(%p, %p, %d)\n", graphics, path, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipCombineRegionPath(graphics->clip, path, mode);
}

GpStatus WINGDIPAPI GdipSetClipRect(GpGraphics *graphics, REAL x, REAL y,
                                    REAL width, REAL height,
                                    CombineMode mode)
{
    GpRectF rect;

    TRACE("(%p, %.2f, %.2f, %.2f, %.2f, %d)\n", graphics, x, y, width, height, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    rect.X = x;
    rect.Y = y;
    rect.Width  = width;
    rect.Height = height;

    return GdipCombineRegionRect(graphics->clip, &rect, mode);
}

GpStatus WINGDIPAPI GdipSetClipRectI(GpGraphics *graphics, INT x, INT y,
                                     INT width, INT height,
                                     CombineMode mode)
{
    TRACE("(%p, %d, %d, %d, %d, %d)\n", graphics, x, y, width, height, mode);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipSetClipRect(graphics, (REAL)x, (REAL)y, (REAL)width, (REAL)height, mode);
}

GpStatus WINGDIPAPI GdipSetClipRegion(GpGraphics *graphics, GpRegion *region,
                                      CombineMode mode)
{
    TRACE("(%p, %p, %d)\n", graphics, region, mode);

    if(!graphics || !region)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipCombineRegionRegion(graphics->clip, region, mode);
}

GpStatus WINGDIPAPI GdipSetMetafileDownLevelRasterizationLimit(GpMetafile *metafile,
    UINT limitDpi)
{
    static int calls;

    TRACE("(%p,%u)\n", metafile, limitDpi);

    if(!(calls++))
        FIXME("not implemented\n");

    return NotImplemented;
}

GpStatus WINGDIPAPI GdipDrawPolygon(GpGraphics *graphics,GpPen *pen,GDIPCONST GpPointF *points,
    INT count)
{
    INT save_state;
    POINT *pti;

    TRACE("(%p, %p, %d)\n", graphics, points, count);

    if(!graphics || !pen || count<=0)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (!graphics->hdc)
    {
        FIXME("graphics object has no HDC\n");
        return Ok;
    }

    pti = GdipAlloc(sizeof(POINT) * count);

    save_state = prepare_dc(graphics, pen);
    SelectObject(graphics->hdc, GetStockObject(NULL_BRUSH));

    transform_and_round_points(graphics, pti, (GpPointF*)points, count);
    Polygon(graphics->hdc, pti, count);

    restore_dc(graphics, save_state);
    GdipFree(pti);

    return Ok;
}

GpStatus WINGDIPAPI GdipDrawPolygonI(GpGraphics *graphics,GpPen *pen,GDIPCONST GpPoint *points,
    INT count)
{
    GpStatus ret;
    GpPointF *ptf;
    INT i;

    TRACE("(%p, %p, %p, %d)\n", graphics, pen, points, count);

    if(count<=0)    return InvalidParameter;
    ptf = GdipAlloc(sizeof(GpPointF) * count);

    for(i = 0;i < count; i++){
        ptf[i].X = (REAL)points[i].X;
        ptf[i].Y = (REAL)points[i].Y;
    }

    ret = GdipDrawPolygon(graphics,pen,ptf,count);
    GdipFree(ptf);

    return ret;
}

GpStatus WINGDIPAPI GdipGetDpiX(GpGraphics *graphics, REAL* dpi)
{
    TRACE("(%p, %p)\n", graphics, dpi);

    if(!graphics || !dpi)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *dpi = graphics->xres;
    return Ok;
}

GpStatus WINGDIPAPI GdipGetDpiY(GpGraphics *graphics, REAL* dpi)
{
    TRACE("(%p, %p)\n", graphics, dpi);

    if(!graphics || !dpi)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    *dpi = graphics->yres;
    return Ok;
}

GpStatus WINGDIPAPI GdipMultiplyWorldTransform(GpGraphics *graphics, GDIPCONST GpMatrix *matrix,
    GpMatrixOrder order)
{
    GpMatrix m;
    GpStatus ret;

    TRACE("(%p, %p, %d)\n", graphics, matrix, order);

    if(!graphics || !matrix)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    m = *(graphics->worldtrans);

    ret = GdipMultiplyMatrix(&m, matrix, order);
    if(ret == Ok)
        *(graphics->worldtrans) = m;

    return ret;
}

/* Color used to fill bitmaps so we can tell which parts have been drawn over by gdi32. */
static const COLORREF DC_BACKGROUND_KEY = 0x0c0b0d;

GpStatus WINGDIPAPI GdipGetDC(GpGraphics *graphics, HDC *hdc)
{
    GpStatus stat=Ok;

    TRACE("(%p, %p)\n", graphics, hdc);

    if(!graphics || !hdc)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if (graphics->image && graphics->image->type == ImageTypeMetafile)
    {
        stat = METAFILE_GetDC((GpMetafile*)graphics->image, hdc);
    }
    else if (!graphics->hdc ||
        (graphics->image && graphics->image->type == ImageTypeBitmap && ((GpBitmap*)graphics->image)->format & PixelFormatAlpha))
    {
        /* Create a fake HDC and fill it with a constant color. */
        HDC temp_hdc;
        HBITMAP hbitmap;
        GpRectF bounds;
        BITMAPINFOHEADER bmih;
        int i;

        stat = get_graphics_bounds(graphics, &bounds);
        if (stat != Ok)
            return stat;

        graphics->temp_hbitmap_width = bounds.Width;
        graphics->temp_hbitmap_height = bounds.Height;

        bmih.biSize = sizeof(bmih);
        bmih.biWidth = graphics->temp_hbitmap_width;
        bmih.biHeight = -graphics->temp_hbitmap_height;
        bmih.biPlanes = 1;
        bmih.biBitCount = 32;
        bmih.biCompression = BI_RGB;
        bmih.biSizeImage = 0;
        bmih.biXPelsPerMeter = 0;
        bmih.biYPelsPerMeter = 0;
        bmih.biClrUsed = 0;
        bmih.biClrImportant = 0;

        hbitmap = CreateDIBSection(NULL, (BITMAPINFO*)&bmih, DIB_RGB_COLORS,
            (void**)&graphics->temp_bits, NULL, 0);
        if (!hbitmap)
            return GenericError;

        temp_hdc = CreateCompatibleDC(0);
        if (!temp_hdc)
        {
            DeleteObject(hbitmap);
            return GenericError;
        }

        for (i=0; i<(graphics->temp_hbitmap_width * graphics->temp_hbitmap_height); i++)
            ((DWORD*)graphics->temp_bits)[i] = DC_BACKGROUND_KEY;

        SelectObject(temp_hdc, hbitmap);

        graphics->temp_hbitmap = hbitmap;
        *hdc = graphics->temp_hdc = temp_hdc;
    }
    else
    {
        *hdc = graphics->hdc;
    }

    if (stat == Ok)
        graphics->busy = TRUE;

    return stat;
}

GpStatus WINGDIPAPI GdipReleaseDC(GpGraphics *graphics, HDC hdc)
{
    GpStatus stat=Ok;

    TRACE("(%p, %p)\n", graphics, hdc);

    if(!graphics || !hdc || !graphics->busy)
        return InvalidParameter;

    if (graphics->image && graphics->image->type == ImageTypeMetafile)
    {
        stat = METAFILE_ReleaseDC((GpMetafile*)graphics->image, hdc);
    }
    else if (graphics->temp_hdc == hdc)
    {
        DWORD* pos;
        int i;

        /* Find the pixels that have changed, and mark them as opaque. */
        pos = (DWORD*)graphics->temp_bits;
        for (i=0; i<(graphics->temp_hbitmap_width * graphics->temp_hbitmap_height); i++)
        {
            if (*pos != DC_BACKGROUND_KEY)
            {
                *pos |= 0xff000000;
            }
            pos++;
        }

        /* Write the changed pixels to the real target. */
        alpha_blend_pixels(graphics, 0, 0, graphics->temp_bits,
            graphics->temp_hbitmap_width, graphics->temp_hbitmap_height,
            graphics->temp_hbitmap_width * 4);

        /* Clean up. */
        DeleteDC(graphics->temp_hdc);
        DeleteObject(graphics->temp_hbitmap);
        graphics->temp_hdc = NULL;
        graphics->temp_hbitmap = NULL;
    }
    else if (hdc != graphics->hdc)
    {
        stat = InvalidParameter;
    }

    if (stat == Ok)
        graphics->busy = FALSE;

    return stat;
}

GpStatus WINGDIPAPI GdipGetClip(GpGraphics *graphics, GpRegion *region)
{
    GpRegion *clip;
    GpStatus status;

    TRACE("(%p, %p)\n", graphics, region);

    if(!graphics || !region)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    if((status = GdipCloneRegion(graphics->clip, &clip)) != Ok)
        return status;

    /* free everything except root node and header */
    delete_element(&region->node);
    memcpy(region, clip, sizeof(GpRegion));
    GdipFree(clip);

    return Ok;
}

static GpStatus get_graphics_transform(GpGraphics *graphics, GpCoordinateSpace dst_space,
        GpCoordinateSpace src_space, GpMatrix **matrix)
{
    GpStatus stat = GdipCreateMatrix(matrix);
    REAL scale_x, scale_y;

    if (dst_space != src_space && stat == Ok)
    {
        scale_x = units_to_pixels(1.0, graphics->unit, graphics->xres);
        scale_y = units_to_pixels(1.0, graphics->unit, graphics->yres);

        if(graphics->unit != UnitDisplay)
        {
            scale_x *= graphics->scale;
            scale_y *= graphics->scale;
        }

        /* transform from src_space to CoordinateSpacePage */
        switch (src_space)
        {
        case CoordinateSpaceWorld:
            GdipMultiplyMatrix(*matrix, graphics->worldtrans, MatrixOrderAppend);
            break;
        case CoordinateSpacePage:
            break;
        case CoordinateSpaceDevice:
            GdipScaleMatrix(*matrix, 1.0/scale_x, 1.0/scale_y, MatrixOrderAppend);
            break;
        }

        /* transform from CoordinateSpacePage to dst_space */
        switch (dst_space)
        {
        case CoordinateSpaceWorld:
            {
                GpMatrix *inverted_transform;
                stat = GdipCloneMatrix(graphics->worldtrans, &inverted_transform);
                if (stat == Ok)
                {
                    stat = GdipInvertMatrix(inverted_transform);
                    if (stat == Ok)
                        GdipMultiplyMatrix(*matrix, inverted_transform, MatrixOrderAppend);
                    GdipDeleteMatrix(inverted_transform);
                }
                break;
            }
        case CoordinateSpacePage:
            break;
        case CoordinateSpaceDevice:
            GdipScaleMatrix(*matrix, scale_x, scale_y, MatrixOrderAppend);
            break;
        }
    }
    return stat;
}

GpStatus WINGDIPAPI GdipTransformPoints(GpGraphics *graphics, GpCoordinateSpace dst_space,
                                        GpCoordinateSpace src_space, GpPointF *points, INT count)
{
    GpMatrix *matrix;
    GpStatus stat;

    if(!graphics || !points || count <= 0)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    TRACE("(%p, %d, %d, %p, %d)\n", graphics, dst_space, src_space, points, count);

    if (src_space == dst_space) return Ok;

    stat = get_graphics_transform(graphics, dst_space, src_space, &matrix);

    if (stat == Ok)
    {
        stat = GdipTransformMatrixPoints(matrix, points, count);

        GdipDeleteMatrix(matrix);
    }

    return stat;
}

GpStatus WINGDIPAPI GdipTransformPointsI(GpGraphics *graphics, GpCoordinateSpace dst_space,
                                         GpCoordinateSpace src_space, GpPoint *points, INT count)
{
    GpPointF *pointsF;
    GpStatus ret;
    INT i;

    TRACE("(%p, %d, %d, %p, %d)\n", graphics, dst_space, src_space, points, count);

    if(count <= 0)
        return InvalidParameter;

    pointsF = GdipAlloc(sizeof(GpPointF) * count);
    if(!pointsF)
        return OutOfMemory;

    for(i = 0; i < count; i++){
        pointsF[i].X = (REAL)points[i].X;
        pointsF[i].Y = (REAL)points[i].Y;
    }

    ret = GdipTransformPoints(graphics, dst_space, src_space, pointsF, count);

    if(ret == Ok)
        for(i = 0; i < count; i++){
            points[i].X = gdip_round(pointsF[i].X);
            points[i].Y = gdip_round(pointsF[i].Y);
        }
    GdipFree(pointsF);

    return ret;
}

HPALETTE WINGDIPAPI GdipCreateHalftonePalette(void)
{
    static int calls;

    TRACE("\n");

    if (!calls++)
      FIXME("stub\n");

    return NULL;
}

/*****************************************************************************
 * GdipTranslateClip [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipTranslateClip(GpGraphics *graphics, REAL dx, REAL dy)
{
    TRACE("(%p, %.2f, %.2f)\n", graphics, dx, dy);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipTranslateRegion(graphics->clip, dx, dy);
}

/*****************************************************************************
 * GdipTranslateClipI [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipTranslateClipI(GpGraphics *graphics, INT dx, INT dy)
{
    TRACE("(%p, %d, %d)\n", graphics, dx, dy);

    if(!graphics)
        return InvalidParameter;

    if(graphics->busy)
        return ObjectBusy;

    return GdipTranslateRegion(graphics->clip, (REAL)dx, (REAL)dy);
}


/*****************************************************************************
 * GdipMeasureDriverString [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipMeasureDriverString(GpGraphics *graphics, GDIPCONST UINT16 *text, INT length,
                                            GDIPCONST GpFont *font, GDIPCONST PointF *positions,
                                            INT flags, GDIPCONST GpMatrix *matrix, RectF *boundingBox)
{
    static const INT unsupported_flags = ~(DriverStringOptionsCmapLookup|DriverStringOptionsRealizedAdvance);
    HFONT hfont;
    HDC hdc;
    REAL min_x, min_y, max_x, max_y, x, y;
    int i;
    TEXTMETRICW textmetric;
    const WORD *glyph_indices;
    WORD *dynamic_glyph_indices=NULL;
    REAL rel_width, rel_height, ascent, descent;
    GpPointF pt[3];

    TRACE("(%p %p %d %p %p %d %p %p)\n", graphics, text, length, font, positions, flags, matrix, boundingBox);

    if (!graphics || !text || !font || !positions || !boundingBox)
        return InvalidParameter;

    if (length == -1)
        length = strlenW(text);

    if (length == 0)
    {
        boundingBox->X = 0.0;
        boundingBox->Y = 0.0;
        boundingBox->Width = 0.0;
        boundingBox->Height = 0.0;
    }

    if (flags & unsupported_flags)
        FIXME("Ignoring flags %x\n", flags & unsupported_flags);

    if (matrix)
        FIXME("Ignoring matrix\n");

    get_font_hfont(graphics, font, &hfont);

    hdc = CreateCompatibleDC(0);
    SelectObject(hdc, hfont);

    GetTextMetricsW(hdc, &textmetric);

    pt[0].X = 0.0;
    pt[0].Y = 0.0;
    pt[1].X = 1.0;
    pt[1].Y = 0.0;
    pt[2].X = 0.0;
    pt[2].Y = 1.0;
    GdipTransformPoints(graphics, CoordinateSpaceDevice, CoordinateSpaceWorld, pt, 3);
    rel_width = sqrt((pt[1].Y-pt[0].Y)*(pt[1].Y-pt[0].Y)+
                     (pt[1].X-pt[0].X)*(pt[1].X-pt[0].X));
    rel_height = sqrt((pt[2].Y-pt[0].Y)*(pt[2].Y-pt[0].Y)+
                      (pt[2].X-pt[0].X)*(pt[2].X-pt[0].X));

    if (flags & DriverStringOptionsCmapLookup)
    {
        glyph_indices = dynamic_glyph_indices = GdipAlloc(sizeof(WORD) * length);
        if (!glyph_indices)
        {
            DeleteDC(hdc);
            DeleteObject(hfont);
            return OutOfMemory;
        }

        GetGlyphIndicesW(hdc, text, length, dynamic_glyph_indices, 0);
    }
    else
        glyph_indices = text;

    min_x = max_x = x = positions[0].X;
    min_y = max_y = y = positions[0].Y;

    ascent = textmetric.tmAscent / rel_height;
    descent = textmetric.tmDescent / rel_height;

    for (i=0; i<length; i++)
    {
        int char_width;
        ABC abc;

        if (!(flags & DriverStringOptionsRealizedAdvance))
        {
            x = positions[i].X;
            y = positions[i].Y;
        }

        GetCharABCWidthsW(hdc, glyph_indices[i], glyph_indices[i], &abc);
        char_width = abc.abcA + abc.abcB + abc.abcB;

        if (min_y > y - ascent) min_y = y - ascent;
        if (max_y < y + descent) max_y = y + descent;
        if (min_x > x) min_x = x;

        x += char_width / rel_width;

        if (max_x < x) max_x = x;
    }

    GdipFree(dynamic_glyph_indices);
    DeleteDC(hdc);
    DeleteObject(hfont);

    boundingBox->X = min_x;
    boundingBox->Y = min_y;
    boundingBox->Width = max_x - min_x;
    boundingBox->Height = max_y - min_y;

    return Ok;
}

static GpStatus GDI32_GdipDrawDriverString(GpGraphics *graphics, GDIPCONST UINT16 *text, INT length,
                                     GDIPCONST GpFont *font, GDIPCONST GpBrush *brush,
                                     GDIPCONST PointF *positions, INT flags,
                                     GDIPCONST GpMatrix *matrix )
{
    static const INT unsupported_flags = ~(DriverStringOptionsRealizedAdvance|DriverStringOptionsCmapLookup);
    INT save_state;
    GpPointF pt;
    HFONT hfont;
    UINT eto_flags=0;

    if (flags & unsupported_flags)
        FIXME("Ignoring flags %x\n", flags & unsupported_flags);

    if (matrix)
        FIXME("Ignoring matrix\n");

    if (!(flags & DriverStringOptionsCmapLookup))
        eto_flags |= ETO_GLYPH_INDEX;

    save_state = SaveDC(graphics->hdc);
    SetBkMode(graphics->hdc, TRANSPARENT);
    SetTextColor(graphics->hdc, get_gdi_brush_color(brush));

    pt = positions[0];
    GdipTransformPoints(graphics, CoordinateSpaceDevice, CoordinateSpaceWorld, &pt, 1);

    get_font_hfont(graphics, font, &hfont);
    SelectObject(graphics->hdc, hfont);

    SetTextAlign(graphics->hdc, TA_BASELINE|TA_LEFT);

    ExtTextOutW(graphics->hdc, gdip_round(pt.X), gdip_round(pt.Y), eto_flags, NULL, text, length, NULL);

    RestoreDC(graphics->hdc, save_state);

    DeleteObject(hfont);

    return Ok;
}

static GpStatus SOFTWARE_GdipDrawDriverString(GpGraphics *graphics, GDIPCONST UINT16 *text, INT length,
                                         GDIPCONST GpFont *font, GDIPCONST GpBrush *brush,
                                         GDIPCONST PointF *positions, INT flags,
                                         GDIPCONST GpMatrix *matrix )
{
    static const INT unsupported_flags = ~(DriverStringOptionsCmapLookup|DriverStringOptionsRealizedAdvance);
    GpStatus stat;
    PointF *real_positions, real_position;
    POINT *pti;
    HFONT hfont;
    HDC hdc;
    int min_x=INT_MAX, min_y=INT_MAX, max_x=INT_MIN, max_y=INT_MIN, i, x, y;
    DWORD max_glyphsize=0;
    GLYPHMETRICS glyphmetrics;
    static const MAT2 identity = {{0,1}, {0,0}, {0,0}, {0,1}};
    BYTE *glyph_mask;
    BYTE *text_mask;
    int text_mask_stride;
    BYTE *pixel_data;
    int pixel_data_stride;
    GpRect pixel_area;
    UINT ggo_flags = GGO_GRAY8_BITMAP;

    if (length <= 0)
        return Ok;

    if (!(flags & DriverStringOptionsCmapLookup))
        ggo_flags |= GGO_GLYPH_INDEX;

    if (flags & unsupported_flags)
        FIXME("Ignoring flags %x\n", flags & unsupported_flags);

    if (matrix)
        FIXME("Ignoring matrix\n");

    pti = GdipAlloc(sizeof(POINT) * length);
    if (!pti)
        return OutOfMemory;

    if (flags & DriverStringOptionsRealizedAdvance)
    {
        real_position = positions[0];

        transform_and_round_points(graphics, pti, &real_position, 1);
    }
    else
    {
        real_positions = GdipAlloc(sizeof(PointF) * length);
        if (!real_positions)
        {
            GdipFree(pti);
            return OutOfMemory;
        }

        memcpy(real_positions, positions, sizeof(PointF) * length);

        transform_and_round_points(graphics, pti, real_positions, length);

        GdipFree(real_positions);
    }

    get_font_hfont(graphics, font, &hfont);

    hdc = CreateCompatibleDC(0);
    SelectObject(hdc, hfont);

    /* Get the boundaries of the text to be drawn */
    for (i=0; i<length; i++)
    {
        DWORD glyphsize;
        int left, top, right, bottom;

        glyphsize = GetGlyphOutlineW(hdc, text[i], ggo_flags,
            &glyphmetrics, 0, NULL, &identity);

        if (glyphsize == GDI_ERROR)
        {
            ERR("GetGlyphOutlineW failed\n");
            GdipFree(pti);
            DeleteDC(hdc);
            DeleteObject(hfont);
            return GenericError;
        }

        if (glyphsize > max_glyphsize)
            max_glyphsize = glyphsize;

        left = pti[i].x + glyphmetrics.gmptGlyphOrigin.x;
        top = pti[i].y - glyphmetrics.gmptGlyphOrigin.y;
        right = pti[i].x + glyphmetrics.gmptGlyphOrigin.x + glyphmetrics.gmBlackBoxX;
        bottom = pti[i].y - glyphmetrics.gmptGlyphOrigin.y + glyphmetrics.gmBlackBoxY;

        if (left < min_x) min_x = left;
        if (top < min_y) min_y = top;
        if (right > max_x) max_x = right;
        if (bottom > max_y) max_y = bottom;

        if (i+1 < length && (flags & DriverStringOptionsRealizedAdvance) == DriverStringOptionsRealizedAdvance)
        {
            pti[i+1].x = pti[i].x + glyphmetrics.gmCellIncX;
            pti[i+1].y = pti[i].y + glyphmetrics.gmCellIncY;
        }
    }

    glyph_mask = GdipAlloc(max_glyphsize);
    text_mask = GdipAlloc((max_x - min_x) * (max_y - min_y));
    text_mask_stride = max_x - min_x;

    if (!(glyph_mask && text_mask))
    {
        GdipFree(glyph_mask);
        GdipFree(text_mask);
        GdipFree(pti);
        DeleteDC(hdc);
        DeleteObject(hfont);
        return OutOfMemory;
    }

    /* Generate a mask for the text */
    for (i=0; i<length; i++)
    {
        int left, top, stride;

        GetGlyphOutlineW(hdc, text[i], ggo_flags,
            &glyphmetrics, max_glyphsize, glyph_mask, &identity);

        left = pti[i].x + glyphmetrics.gmptGlyphOrigin.x;
        top = pti[i].y - glyphmetrics.gmptGlyphOrigin.y;
        stride = (glyphmetrics.gmBlackBoxX + 3) & (~3);

        for (y=0; y<glyphmetrics.gmBlackBoxY; y++)
        {
            BYTE *glyph_val = glyph_mask + y * stride;
            BYTE *text_val = text_mask + (left - min_x) + (top - min_y + y) * text_mask_stride;
            for (x=0; x<glyphmetrics.gmBlackBoxX; x++)
            {
                *text_val = min(64, *text_val + *glyph_val);
                glyph_val++;
                text_val++;
            }
        }
    }

    GdipFree(pti);
    DeleteDC(hdc);
    DeleteObject(hfont);
    GdipFree(glyph_mask);

    /* get the brush data */
    pixel_data = GdipAlloc(4 * (max_x - min_x) * (max_y - min_y));
    if (!pixel_data)
    {
        GdipFree(text_mask);
        return OutOfMemory;
    }

    pixel_area.X = min_x;
    pixel_area.Y = min_y;
    pixel_area.Width = max_x - min_x;
    pixel_area.Height = max_y - min_y;
    pixel_data_stride = pixel_area.Width * 4;

    stat = brush_fill_pixels(graphics, (GpBrush*)brush, (DWORD*)pixel_data, &pixel_area, pixel_area.Width);
    if (stat != Ok)
    {
        GdipFree(text_mask);
        GdipFree(pixel_data);
        return stat;
    }

    /* multiply the brush data by the mask */
    for (y=0; y<pixel_area.Height; y++)
    {
        BYTE *text_val = text_mask + text_mask_stride * y;
        BYTE *pixel_val = pixel_data + pixel_data_stride * y + 3;
        for (x=0; x<pixel_area.Width; x++)
        {
            *pixel_val = (*pixel_val) * (*text_val) / 64;
            text_val++;
            pixel_val+=4;
        }
    }

    GdipFree(text_mask);

    /* draw the result */
    stat = alpha_blend_pixels(graphics, min_x, min_y, pixel_data, pixel_area.Width,
        pixel_area.Height, pixel_data_stride);

    GdipFree(pixel_data);

    return stat;
}

/*****************************************************************************
 * GdipDrawDriverString [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipDrawDriverString(GpGraphics *graphics, GDIPCONST UINT16 *text, INT length,
                                         GDIPCONST GpFont *font, GDIPCONST GpBrush *brush,
                                         GDIPCONST PointF *positions, INT flags,
                                         GDIPCONST GpMatrix *matrix )
{
    GpStatus stat=NotImplemented;

    TRACE("(%p %s %p %p %p %d %p)\n", graphics, debugstr_wn(text, length), font, brush, positions, flags, matrix);

    if (!graphics || !text || !font || !brush || !positions)
        return InvalidParameter;

    if (length == -1)
        length = strlenW(text);

    if (graphics->hdc &&
        ((flags & DriverStringOptionsRealizedAdvance) || length <= 1) &&
        brush->bt == BrushTypeSolidColor &&
        (((GpSolidFill*)brush)->color & 0xff000000) == 0xff000000)
        stat = GDI32_GdipDrawDriverString(graphics, text, length, font, brush,
            positions, flags, matrix);

    if (stat == NotImplemented)
        stat = SOFTWARE_GdipDrawDriverString(graphics, text, length, font, brush,
            positions, flags, matrix);

    return stat;
}

GpStatus WINGDIPAPI GdipRecordMetafileStream(IStream *stream, HDC hdc, EmfType type, GDIPCONST GpRect *frameRect,
                                        MetafileFrameUnit frameUnit, GDIPCONST WCHAR *desc, GpMetafile **metafile)
{
    FIXME("(%p %p %d %p %d %p %p): stub\n", stream, hdc, type, frameRect, frameUnit, desc, metafile);
    return NotImplemented;
}

/*****************************************************************************
 * GdipIsVisibleClipEmpty [GDIPLUS.@]
 */
GpStatus WINGDIPAPI GdipIsVisibleClipEmpty(GpGraphics *graphics, BOOL *res)
{
    GpStatus stat;
    GpRegion* rgn;

    TRACE("(%p, %p)\n", graphics, res);

    if((stat = GdipCreateRegion(&rgn)) != Ok)
        return stat;

    if((stat = get_visible_clip_region(graphics, rgn)) != Ok)
        goto cleanup;

    stat = GdipIsEmptyRegion(rgn, graphics, res);

cleanup:
    GdipDeleteRegion(rgn);
    return stat;
}

GpStatus WINGDIPAPI GdipResetPageTransform(GpGraphics *graphics)
{
    static int calls;

    TRACE("(%p) stub\n", graphics);

    if(!(calls++))
        FIXME("not implemented\n");

    return NotImplemented;
}
