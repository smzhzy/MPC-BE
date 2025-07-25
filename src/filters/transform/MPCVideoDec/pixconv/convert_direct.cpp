/*
 *      Copyright (C) 2010-2021 Hendrik Leppkes
 *      http://www.1f0.de
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *
 *  Adaptation for MPC-BE (C) 2013-2024 v0lt & Alexandr Vodiannikov aka "Aleksoid1978" (Aleksoid1978@mail.ru)
 */

#include "stdafx.h"
#include "FormatConverter.h"
#include "pixconv_internal.h"
#include "DSUtil/pixconv/pixconv_sse2_templates.h"

#pragma warning(push)
#pragma warning(disable: 4005)
extern "C" {
    #include <ExtLib/ffmpeg/libavutil/pixfmt.h>
}
#pragma warning(pop)

//
// from LAVFilters/decoder/LAVVideo/pixconv/convert_direct.cpp
//

// This function is only designed for NV12-like pixel formats, like NV12, P010, P016, ...
HRESULT CFormatConverter::plane_copy_direct_nv12_sse4(CONV_FUNC_PARAMS)
{
    const ptrdiff_t inStride = srcStride[0];
    const ptrdiff_t outStride = dstStride[0];
    const ptrdiff_t chromaHeight = (height >> 1);

    const ptrdiff_t byteWidth =
        (m_out_pixfmt == PixFmt_P010 || m_out_pixfmt == PixFmt_P016) ? width << 1 : width;
    const ptrdiff_t stride = std::min(FFALIGN(byteWidth, 64),std::min(inStride, outStride));

    __m128i xmm0, xmm1, xmm2, xmm3;

    _mm_sfence();

    ptrdiff_t line, i;

    for (line = 0; line < height; line++)
    {
        const uint8_t *y = (src[0] + line * inStride);
        uint8_t *dy = (dst[0] + line * outStride);
        for (i = 0; i < (stride - 63); i += 64)
        {
            PIXCONV_STREAM_LOAD(xmm0, y + i + 0);
            PIXCONV_STREAM_LOAD(xmm1, y + i + 16);
            PIXCONV_STREAM_LOAD(xmm2, y + i + 32);
            PIXCONV_STREAM_LOAD(xmm3, y + i + 48);

            _ReadWriteBarrier();

            PIXCONV_PUT_STREAM(dy + i + 0, xmm0);
            PIXCONV_PUT_STREAM(dy + i + 16, xmm1);
            PIXCONV_PUT_STREAM(dy + i + 32, xmm2);
            PIXCONV_PUT_STREAM(dy + i + 48, xmm3);
        }

        for (; i < byteWidth; i += 16)
        {
            PIXCONV_LOAD_ALIGNED(xmm0, y + i);
            PIXCONV_PUT_STREAM(dy + i, xmm0);
        }
    }

    for (line = 0; line < chromaHeight; line++)
    {
        const uint8_t *uv = (src[1] + line * inStride);
        uint8_t *duv = (dst[1] + line * outStride);
        for (i = 0; i < (stride - 63); i += 64)
        {
            PIXCONV_STREAM_LOAD(xmm0, uv + i + 0);
            PIXCONV_STREAM_LOAD(xmm1, uv + i + 16);
            PIXCONV_STREAM_LOAD(xmm2, uv + i + 32);
            PIXCONV_STREAM_LOAD(xmm3, uv + i + 48);

            _ReadWriteBarrier();

            PIXCONV_PUT_STREAM(duv + i + 0, xmm0);
            PIXCONV_PUT_STREAM(duv + i + 16, xmm1);
            PIXCONV_PUT_STREAM(duv + i + 32, xmm2);
            PIXCONV_PUT_STREAM(duv + i + 48, xmm3);
        }

        for (; i < byteWidth; i += 16)
        {
            PIXCONV_LOAD_ALIGNED(xmm0, uv + i);
            PIXCONV_PUT_STREAM(duv + i, xmm0);
        }
    }

    return S_OK;
}

HRESULT CFormatConverter::plane_copy_direct_sse4(CONV_FUNC_PARAMS)
{
    const SW_OUT_FMT& desc = s_sw_formats[m_out_pixfmt];

    const int widthBytes = width * desc.codedbytes;
    const int planes = std::max(desc.planes, 1);

    ptrdiff_t line, plane;

    for (plane = 0; plane < planes; plane++)
    {
        const int planeWidth = widthBytes / desc.planeWidth[plane];
        const int planeHeight = height / desc.planeHeight[plane];
        const ptrdiff_t srcPlaneStride = srcStride[plane];
        const ptrdiff_t dstPlaneStride = dstStride[plane];
        const uint8_t *const srcBuf = src[plane];
        uint8_t *const dstBuf = dst[plane];

        for (line = 0; line < planeHeight; ++line)
        {
            const uint8_t *const srcLinePtr = srcBuf + line * srcPlaneStride;
            uint8_t *const dstLinePtr = dstBuf + line * dstPlaneStride;
            __m128i r1, r2, r3, r4;
            ptrdiff_t i;
            for (i = 0; i < (planeWidth - 63); i += 64)
            {
                PIXCONV_STREAM_LOAD(r1, srcLinePtr + i + 0)
                PIXCONV_STREAM_LOAD(r2, srcLinePtr + i + 16);
                PIXCONV_STREAM_LOAD(r3, srcLinePtr + i + 32);
                PIXCONV_STREAM_LOAD(r4, srcLinePtr + i + 48);

                _ReadWriteBarrier();

                PIXCONV_PUT_STREAM(dstLinePtr + i + 0, r1);
                PIXCONV_PUT_STREAM(dstLinePtr + i + 16, r2);
                PIXCONV_PUT_STREAM(dstLinePtr + i + 32, r3);
                PIXCONV_PUT_STREAM(dstLinePtr + i + 48, r4);
            }
            for (; i < planeWidth; i += 16)
            {
                PIXCONV_STREAM_LOAD(r1, srcLinePtr + i);
                PIXCONV_PUT_STREAM(dstLinePtr + i, r1);
            }
        }
    }

    return S_OK;
}

HRESULT CFormatConverter::convert_nv12_yv12_direct_sse4(CONV_FUNC_PARAMS)
{
    const ptrdiff_t inStride = srcStride[0];
    const ptrdiff_t outStride = dstStride[0];
    const ptrdiff_t outChromaStride = dstStride[1];
    const ptrdiff_t chromaHeight = (height >> 1);

    const ptrdiff_t stride = std::min<ptrdiff_t>(FFALIGN(width, 64), std::min(inStride, outStride));

    __m128i xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm7;
    xmm7 = _mm_set1_epi16(0x00FF);

    _mm_sfence();

    ptrdiff_t line, i;

    for (line = 0; line < height; line++)
    {
        const uint8_t *y = (src[0] + line * inStride);
        uint8_t *dy = (dst[0] + line * outStride);
        for (i = 0; i < (stride - 63); i += 64)
        {
            PIXCONV_STREAM_LOAD(xmm0, y + i + 0);
            PIXCONV_STREAM_LOAD(xmm1, y + i + 16);
            PIXCONV_STREAM_LOAD(xmm2, y + i + 32);
            PIXCONV_STREAM_LOAD(xmm3, y + i + 48);

            _ReadWriteBarrier();

            PIXCONV_PUT_STREAM(dy + i + 0, xmm0);
            PIXCONV_PUT_STREAM(dy + i + 16, xmm1);
            PIXCONV_PUT_STREAM(dy + i + 32, xmm2);
            PIXCONV_PUT_STREAM(dy + i + 48, xmm3);
        }

        for (; i < width; i += 16)
        {
            PIXCONV_LOAD_ALIGNED(xmm0, y + i);
            PIXCONV_PUT_STREAM(dy + i, xmm0);
        }
    }

    for (line = 0; line < chromaHeight; line++)
    {
        const uint8_t *uv = (src[1] + line * inStride);
        uint8_t *dv = (dst[1] + line * outChromaStride);
        uint8_t *du = (dst[2] + line * outChromaStride);
        for (i = 0; i < (stride - 63); i += 64)
        {
            PIXCONV_STREAM_LOAD(xmm0, uv + i + 0);
            PIXCONV_STREAM_LOAD(xmm1, uv + i + 16);
            PIXCONV_STREAM_LOAD(xmm2, uv + i + 32);
            PIXCONV_STREAM_LOAD(xmm3, uv + i + 48);

            _ReadWriteBarrier();

            // process first pair
            xmm4 = _mm_srli_epi16(xmm0, 8);
            xmm5 = _mm_srli_epi16(xmm1, 8);
            xmm0 = _mm_and_si128(xmm0, xmm7);
            xmm1 = _mm_and_si128(xmm1, xmm7);
            xmm0 = _mm_packus_epi16(xmm0, xmm1);
            xmm4 = _mm_packus_epi16(xmm4, xmm5);

            PIXCONV_PUT_STREAM(du + (i >> 1) + 0, xmm0);
            PIXCONV_PUT_STREAM(dv + (i >> 1) + 0, xmm4);

            // and second pair
            xmm4 = _mm_srli_epi16(xmm2, 8);
            xmm5 = _mm_srli_epi16(xmm3, 8);
            xmm2 = _mm_and_si128(xmm2, xmm7);
            xmm3 = _mm_and_si128(xmm3, xmm7);
            xmm2 = _mm_packus_epi16(xmm2, xmm3);
            xmm4 = _mm_packus_epi16(xmm4, xmm5);

            PIXCONV_PUT_STREAM(du + (i >> 1) + 16, xmm2);
            PIXCONV_PUT_STREAM(dv + (i >> 1) + 16, xmm4);
        }

        for (; i < width; i += 32)
        {
            PIXCONV_LOAD_ALIGNED(xmm0, uv + i + 0);
            PIXCONV_LOAD_ALIGNED(xmm1, uv + i + 16);

            xmm4 = _mm_srli_epi16(xmm0, 8);
            xmm5 = _mm_srli_epi16(xmm1, 8);
            xmm0 = _mm_and_si128(xmm0, xmm7);
            xmm1 = _mm_and_si128(xmm1, xmm7);
            xmm0 = _mm_packus_epi16(xmm0, xmm1);
            xmm4 = _mm_packus_epi16(xmm4, xmm5);

            PIXCONV_PUT_STREAM(du + (i >> 1), xmm0);
            PIXCONV_PUT_STREAM(dv + (i >> 1), xmm4);
        }
    }

    return S_OK;
}

HRESULT CFormatConverter::convert_p010_nv12_direct_sse4(CONV_FUNC_PARAMS)
{
    const ptrdiff_t inStride = srcStride[0];
    const ptrdiff_t outStride = dstStride[0];
    const ptrdiff_t chromaHeight = (height >> 1);

    const ptrdiff_t byteWidth = width << 1;
    const ptrdiff_t stride = std::min(FFALIGN(byteWidth, 64), std::min(inStride, outStride << 1));

    __m128i xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7;

    _mm_sfence();

    ptrdiff_t line, i;

    for (line = 0; line < height; line++)
    {
        // Load dithering coefficients for this line
        {
            PIXCONV_LOAD_DITHER_COEFFS(xmm7, line, 8, dithers);
            xmm4 = xmm5 = xmm6 = xmm7;
        }

        const uint8_t *y = (src[0] + line * inStride);
        uint8_t *dy = (dst[0] + line * outStride);

        for (i = 0; i < (stride - 63); i += 64)
        {
            PIXCONV_STREAM_LOAD(xmm0, y + i + 0);
            PIXCONV_STREAM_LOAD(xmm1, y + i + 16);
            PIXCONV_STREAM_LOAD(xmm2, y + i + 32);
            PIXCONV_STREAM_LOAD(xmm3, y + i + 48);

            _ReadWriteBarrier();

            // apply dithering coeffs
            xmm0 = _mm_adds_epu16(xmm0, xmm4);
            xmm1 = _mm_adds_epu16(xmm1, xmm5);
            xmm2 = _mm_adds_epu16(xmm2, xmm6);
            xmm3 = _mm_adds_epu16(xmm3, xmm7);

            // shift and pack to 8-bit
            xmm0 = _mm_packus_epi16(_mm_srli_epi16(xmm0, 8), _mm_srli_epi16(xmm1, 8));
            xmm2 = _mm_packus_epi16(_mm_srli_epi16(xmm2, 8), _mm_srli_epi16(xmm3, 8));

            PIXCONV_PUT_STREAM(dy + (i >> 1) + 0, xmm0);
            PIXCONV_PUT_STREAM(dy + (i >> 1) + 16, xmm2);
        }

        for (; i < byteWidth; i += 32)
        {
            PIXCONV_LOAD_ALIGNED(xmm0, y + i + 0);
            PIXCONV_LOAD_ALIGNED(xmm1, y + i + 16);

            // apply dithering coeffs
            xmm0 = _mm_adds_epu16(xmm0, xmm4);
            xmm1 = _mm_adds_epu16(xmm1, xmm5);

            // shift and pack to 8-bit
            xmm0 = _mm_packus_epi16(_mm_srli_epi16(xmm0, 8), _mm_srli_epi16(xmm1, 8));

            PIXCONV_PUT_STREAM(dy + (i >> 1), xmm0);
        }
    }

    for (line = 0; line < chromaHeight; line++)
    {
        // Load dithering coefficients for this line
        {
            PIXCONV_LOAD_DITHER_COEFFS(xmm7, line, 8, dithers);
            xmm4 = xmm5 = xmm6 = xmm7;
        }

        const uint8_t *uv = (src[1] + line * inStride);
        uint8_t *duv = (dst[1] + line * outStride);

        for (i = 0; i < (stride - 63); i += 64)
        {
            PIXCONV_STREAM_LOAD(xmm0, uv + i + 0);
            PIXCONV_STREAM_LOAD(xmm1, uv + i + 16);
            PIXCONV_STREAM_LOAD(xmm2, uv + i + 32);
            PIXCONV_STREAM_LOAD(xmm3, uv + i + 48);

            _ReadWriteBarrier();

            // apply dithering coeffs
            xmm0 = _mm_adds_epu16(xmm0, xmm4);
            xmm1 = _mm_adds_epu16(xmm1, xmm5);
            xmm2 = _mm_adds_epu16(xmm2, xmm6);
            xmm3 = _mm_adds_epu16(xmm3, xmm7);

            // shift and pack to 8-bit
            xmm0 = _mm_packus_epi16(_mm_srli_epi16(xmm0, 8), _mm_srli_epi16(xmm1, 8));
            xmm2 = _mm_packus_epi16(_mm_srli_epi16(xmm2, 8), _mm_srli_epi16(xmm3, 8));

            PIXCONV_PUT_STREAM(duv + (i >> 1) + 0, xmm0);
            PIXCONV_PUT_STREAM(duv + (i >> 1) + 16, xmm2);
        }

        for (; i < byteWidth; i += 32)
        {
            PIXCONV_LOAD_ALIGNED(xmm0, uv + i + 0);
            PIXCONV_LOAD_ALIGNED(xmm1, uv + i + 16);

            // apply dithering coeffs
            xmm0 = _mm_adds_epu16(xmm0, xmm4);
            xmm1 = _mm_adds_epu16(xmm1, xmm5);

            // shift and pack to 8-bit
            xmm0 = _mm_packus_epi16(_mm_srli_epi16(xmm0, 8), _mm_srli_epi16(xmm1, 8));

            PIXCONV_PUT_STREAM(duv + (i >> 1), xmm0);
        }
    }

    return S_OK;
}

HRESULT CFormatConverter::convert_y210_p210_direct_sse4(CONV_FUNC_PARAMS)
{
    const ptrdiff_t inStride = srcStride[0];
    const ptrdiff_t outStride = dstStride[0];

    const ptrdiff_t byteWidth = width << 2;
    const ptrdiff_t stride = std::min(FFALIGN(byteWidth, 64), std::min(inStride, outStride << 1));

    ptrdiff_t line, i;
    __m128i xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7;
    xmm7 = _mm_set1_epi32(0x0000FFFF);

    for (line = 0; line < height; line++)
    {
        const uint8_t *srcLine = src[0] + line * inStride;
        uint8_t *dstY = dst[0] + line * dstStride[0];
        uint8_t *dstUV = dst[1] + line * dstStride[1];
        for (i = 0; i < (stride - 63); i += 64)
        {
            PIXCONV_STREAM_LOAD(xmm0, srcLine + i + 0); // Y0 U Y1 V
            PIXCONV_STREAM_LOAD(xmm1, srcLine + i + 16);
            PIXCONV_STREAM_LOAD(xmm2, srcLine + i + 32);
            PIXCONV_STREAM_LOAD(xmm3, srcLine + i + 48);

            // extract Y
            xmm4 = _mm_and_si128(xmm0, xmm7);
            xmm5 = _mm_and_si128(xmm1, xmm7);
            xmm6 = _mm_packus_epi32(xmm4, xmm5);

            xmm4 = _mm_and_si128(xmm2, xmm7);
            xmm5 = _mm_and_si128(xmm3, xmm7);
            xmm4 = _mm_packus_epi32(xmm4, xmm5);

            PIXCONV_PUT_STREAM(dstY + (i >> 1) + 0, xmm6);
            PIXCONV_PUT_STREAM(dstY + (i >> 1) + 16, xmm4);

            // extract UV
            xmm4 = _mm_srli_epi32(xmm0, 16);
            xmm5 = _mm_srli_epi32(xmm1, 16);
            xmm6 = _mm_packus_epi32(xmm4, xmm5);

            xmm4 = _mm_srli_epi32(xmm2, 16);
            xmm5 = _mm_srli_epi32(xmm3, 16);
            xmm4 = _mm_packus_epi32(xmm4, xmm5);

            PIXCONV_PUT_STREAM(dstUV + (i >> 1) + 0, xmm6);
            PIXCONV_PUT_STREAM(dstUV + (i >> 1) + 16, xmm4);
        }
    }

    return S_OK;
}

HRESULT CFormatConverter::convert_yuy2_yv16_direct_sse4(CONV_FUNC_PARAMS)
{
    const ptrdiff_t inStride = srcStride[0];
    const ptrdiff_t outStride = dstStride[0];

    const ptrdiff_t byteWidth = width << 1;
    const ptrdiff_t stride = std::min(FFALIGN(byteWidth, 64), std::min(inStride, outStride << 1));

    ptrdiff_t line, i;
    __m128i xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7;
    xmm7 = _mm_set1_epi16(0x00FF);

    for (line = 0; line < height; line++)
    {
        const uint8_t *srcLine = src[0] + line * inStride;
        uint8_t *dstY = dst[0] + line * dstStride[0];
        uint8_t *dstV = dst[1] + line * dstStride[1];
        uint8_t *dstU = dst[2] + line * dstStride[2];
        for (i = 0; i < (stride - 63); i += 64)
        {
            PIXCONV_STREAM_LOAD(xmm0, srcLine + i + 0); // Y0 U Y1 V
            PIXCONV_STREAM_LOAD(xmm1, srcLine + i + 16);
            PIXCONV_STREAM_LOAD(xmm2, srcLine + i + 32);
            PIXCONV_STREAM_LOAD(xmm3, srcLine + i + 48);

            _ReadWriteBarrier();

            // extract Y
            xmm4 = _mm_and_si128(xmm0, xmm7);
            xmm5 = _mm_and_si128(xmm1, xmm7);
            xmm6 = _mm_packus_epi16(xmm4, xmm5);

            xmm4 = _mm_and_si128(xmm2, xmm7);
            xmm5 = _mm_and_si128(xmm3, xmm7);
            xmm4 = _mm_packus_epi16(xmm4, xmm5);

            PIXCONV_PUT_STREAM(dstY + (i >> 1) + 0, xmm6);
            PIXCONV_PUT_STREAM(dstY + (i >> 1) + 16, xmm4);

            // extract UV
            xmm4 = _mm_srli_epi16(xmm0, 8);
            xmm5 = _mm_srli_epi16(xmm1, 8);
            xmm0 = _mm_packus_epi16(xmm4, xmm5);

            xmm4 = _mm_srli_epi16(xmm2, 8);
            xmm5 = _mm_srli_epi16(xmm3, 8);
            xmm1 = _mm_packus_epi16(xmm4, xmm5);

            // split into U/V
            xmm4 = _mm_srli_epi16(xmm0, 8);
            xmm5 = _mm_srli_epi16(xmm1, 8);
            xmm0 = _mm_and_si128(xmm0, xmm7);
            xmm1 = _mm_and_si128(xmm1, xmm7);
            xmm0 = _mm_packus_epi16(xmm0, xmm1);
            xmm4 = _mm_packus_epi16(xmm4, xmm5);

            PIXCONV_PUT_STREAM(dstU + (i >> 2), xmm0);
            PIXCONV_PUT_STREAM(dstV + (i >> 2), xmm4);
        }
    }

    return S_OK;
}
