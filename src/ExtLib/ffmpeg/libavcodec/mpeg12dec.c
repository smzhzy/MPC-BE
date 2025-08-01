/*
 * MPEG-1/2 decoder
 * Copyright (c) 2000, 2001 Fabrice Bellard
 * Copyright (c) 2002-2013 Michael Niedermayer <michaelni@gmx.at>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * MPEG-1/2 decoder
 */

#include "config_components.h"

#define UNCHECKED_BITSTREAM_READER 1
#include <inttypes.h>
#include <stdatomic.h>

#include "libavutil/attributes.h"
#include "libavutil/emms.h"
#include "libavutil/imgutils.h"
#include "libavutil/internal.h"
#include "libavutil/mem_internal.h"
#include "libavutil/reverse.h"
#include "libavutil/stereo3d.h"
#include "libavutil/timecode.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "error_resilience.h"
#include "get_bits.h"
#include "hwaccel_internal.h"
#include "hwconfig.h"
#include "idctdsp.h"
#include "mpeg_er.h"
#include "mpeg12.h"
#include "mpeg12codecs.h"
#include "mpeg12data.h"
#include "mpeg12dec.h"
#include "mpegutils.h"
#include "mpegvideo.h"
#include "mpegvideodata.h"
#include "mpegvideodec.h"
#include "profiles.h"
#include "startcode.h"

#define A53_MAX_CC_COUNT 2000

enum Mpeg2ClosedCaptionsFormat {
    CC_FORMAT_AUTO,
    CC_FORMAT_A53_PART4,
    CC_FORMAT_SCTE20,
    CC_FORMAT_DVD,
    CC_FORMAT_DISH
};

typedef struct Mpeg12SliceContext {
    MPVContext c;
    GetBitContext gb;

    DECLARE_ALIGNED_32(int16_t, block)[12][64];
} Mpeg12SliceContext;

typedef struct Mpeg1Context {
    Mpeg12SliceContext slice;
    AVPanScan pan_scan;         /* some temporary storage for the panscan */
    enum AVStereo3DType stereo3d_type;
    int has_stereo3d;
    AVBufferRef *a53_buf_ref;
    enum Mpeg2ClosedCaptionsFormat cc_format;
    uint8_t afd;
    int has_afd;
    int slice_count;
    unsigned aspect_ratio_info;
    int save_progressive_seq, save_chroma_format;
    AVRational frame_rate_ext;  /* MPEG-2 specific framerate modificator */
    unsigned frame_rate_index;
    int sync;                   /* Did we reach a sync point like a GOP/SEQ/KEYFrame? */
    int closed_gop;
    int tmpgexs;
    int first_slice;
    int extradata_decoded;
    int vbv_delay;
    int64_t bit_rate;
    int64_t timecode_frame_start;  /*< GOP timecode frame start number, in non drop frame format */
} Mpeg1Context;

// ==> Start patch MPC
int decode_chunks(AVCodecContext* avctx, AVFrame* picture,
                  int* got_output, const uint8_t* buf, int buf_size);
// ==> End patch MPC

/* as H.263, but only 17 codes */
static int mpeg_decode_motion(Mpeg12SliceContext *const s, int fcode, int pred)
{
    int code, sign, val, shift;

    code = get_vlc2(&s->gb, ff_mv_vlc, MV_VLC_BITS, 2);
    if (code == 0)
        return pred;
    if (code < 0)
        return 0xffff;

    sign  = get_bits1(&s->gb);
    shift = fcode - 1;
    val   = code;
    if (shift) {
        val  = (val - 1) << shift;
        val |= get_bits(&s->gb, shift);
        val++;
    }
    if (sign)
        val = -val;
    val += pred;

    /* modulo decoding */
    return sign_extend(val, 5 + shift);
}

#define MAX_INDEX (64 - 1)
#define check_scantable_index(ctx, x)                                         \
    do {                                                                      \
        if ((x) > MAX_INDEX) {                                                \
            av_log(ctx->c.avctx, AV_LOG_ERROR, "ac-tex damaged at %d %d\n",   \
                   ctx->c.mb_x, ctx->c.mb_y);                                 \
            return AVERROR_INVALIDDATA;                                       \
        }                                                                     \
    } while (0)

static inline int mpeg1_decode_block_inter(Mpeg12SliceContext *const s,
                                           int16_t *block, int n)
{
    int level, i, j, run;
    const uint8_t *const scantable = s->c.intra_scantable.permutated;
    const uint16_t *quant_matrix = s->c.inter_matrix;
    const int qscale             = s->c.qscale;

    {
        OPEN_READER(re, &s->gb);
        i = -1;
        // special case for first coefficient, no need to add second VLC table
        UPDATE_CACHE(re, &s->gb);
        if (((int32_t) GET_CACHE(re, &s->gb)) < 0) {
            level = (3 * qscale * quant_matrix[0]) >> 5;
            level = (level - 1) | 1;
            if (GET_CACHE(re, &s->gb) & 0x40000000)
                level = -level;
            block[0] = level;
            i++;
            SKIP_BITS(re, &s->gb, 2);
            if (((int32_t) GET_CACHE(re, &s->gb)) <= (int32_t) 0xBFFFFFFF)
                goto end;
        }
        /* now quantify & encode AC coefficients */
        for (;;) {
            GET_RL_VLC(level, run, re, &s->gb, ff_mpeg1_rl_vlc,
                       TEX_VLC_BITS, 2, 0);

            if (level != 0) {
                i += run;
                if (i > MAX_INDEX)
                    break;
                j = scantable[i];
                level = ((level * 2 + 1) * qscale * quant_matrix[j]) >> 5;
                level = (level - 1) | 1;
                level = (level ^ SHOW_SBITS(re, &s->gb, 1)) -
                        SHOW_SBITS(re, &s->gb, 1);
                SKIP_BITS(re, &s->gb, 1);
            } else {
                /* escape */
                run = SHOW_UBITS(re, &s->gb, 6) + 1;
                LAST_SKIP_BITS(re, &s->gb, 6);
                UPDATE_CACHE(re, &s->gb);
                level = SHOW_SBITS(re, &s->gb, 8);
                SKIP_BITS(re, &s->gb, 8);
                if (level == -128) {
                    level = SHOW_UBITS(re, &s->gb, 8) - 256;
                    SKIP_BITS(re, &s->gb, 8);
                } else if (level == 0) {
                    level = SHOW_UBITS(re, &s->gb, 8);
                    SKIP_BITS(re, &s->gb, 8);
                }
                i += run;
                if (i > MAX_INDEX)
                    break;
                j = scantable[i];
                if (level < 0) {
                    level = -level;
                    level = ((level * 2 + 1) * qscale * quant_matrix[j]) >> 5;
                    level = (level - 1) | 1;
                    level = -level;
                } else {
                    level = ((level * 2 + 1) * qscale * quant_matrix[j]) >> 5;
                    level = (level - 1) | 1;
                }
            }

            block[j] = level;
            if (((int32_t) GET_CACHE(re, &s->gb)) <= (int32_t) 0xBFFFFFFF)
                break;
            UPDATE_CACHE(re, &s->gb);
        }
end:
        LAST_SKIP_BITS(re, &s->gb, 2);
        CLOSE_READER(re, &s->gb);
    }

    check_scantable_index(s, i);

    s->c.block_last_index[n] = i;
    return 0;
}

static inline int mpeg2_decode_block_non_intra(Mpeg12SliceContext *const s,
                                               int16_t *block, int n)
{
    int level, i, j, run;
    const uint8_t *const scantable = s->c.intra_scantable.permutated;
    const uint16_t *quant_matrix;
    const int qscale = s->c.qscale;
    int mismatch;

    mismatch = 1;

    {
        OPEN_READER(re, &s->gb);
        i = -1;
        if (n < 4)
            quant_matrix = s->c.inter_matrix;
        else
            quant_matrix = s->c.chroma_inter_matrix;

        // Special case for first coefficient, no need to add second VLC table.
        UPDATE_CACHE(re, &s->gb);
        if (((int32_t) GET_CACHE(re, &s->gb)) < 0) {
            level = (3 * qscale * quant_matrix[0]) >> 5;
            if (GET_CACHE(re, &s->gb) & 0x40000000)
                level = -level;
            block[0]  = level;
            mismatch ^= level;
            i++;
            SKIP_BITS(re, &s->gb, 2);
            if (((int32_t) GET_CACHE(re, &s->gb)) <= (int32_t) 0xBFFFFFFF)
                goto end;
        }

        /* now quantify & encode AC coefficients */
        for (;;) {
            GET_RL_VLC(level, run, re, &s->gb, ff_mpeg1_rl_vlc,
                       TEX_VLC_BITS, 2, 0);

            if (level != 0) {
                i += run;
                if (i > MAX_INDEX)
                    break;
                j = scantable[i];
                level = ((level * 2 + 1) * qscale * quant_matrix[j]) >> 5;
                level = (level ^ SHOW_SBITS(re, &s->gb, 1)) -
                        SHOW_SBITS(re, &s->gb, 1);
                SKIP_BITS(re, &s->gb, 1);
            } else {
                /* escape */
                run = SHOW_UBITS(re, &s->gb, 6) + 1;
                LAST_SKIP_BITS(re, &s->gb, 6);
                UPDATE_CACHE(re, &s->gb);
                level = SHOW_SBITS(re, &s->gb, 12);
                SKIP_BITS(re, &s->gb, 12);

                i += run;
                if (i > MAX_INDEX)
                    break;
                j = scantable[i];
                if (level < 0) {
                    level = ((-level * 2 + 1) * qscale * quant_matrix[j]) >> 5;
                    level = -level;
                } else {
                    level = ((level * 2 + 1) * qscale * quant_matrix[j]) >> 5;
                }
            }

            mismatch ^= level;
            block[j]  = level;
            if (((int32_t) GET_CACHE(re, &s->gb)) <= (int32_t) 0xBFFFFFFF)
                break;
            UPDATE_CACHE(re, &s->gb);
        }
end:
        LAST_SKIP_BITS(re, &s->gb, 2);
        CLOSE_READER(re, &s->gb);
    }
    block[63] ^= (mismatch & 1);

    check_scantable_index(s, i);

    s->c.block_last_index[n] = i;
    return 0;
}

static inline int mpeg2_decode_block_intra(Mpeg12SliceContext *const s,
                                           int16_t *block, int n)
{
    int level, dc, diff, i, j, run;
    int component;
    const RL_VLC_ELEM *rl_vlc;
    const uint8_t *const scantable = s->c.intra_scantable.permutated;
    const uint16_t *quant_matrix;
    const int qscale = s->c.qscale;
    int mismatch;

    /* DC coefficient */
    if (n < 4) {
        quant_matrix = s->c.intra_matrix;
        component    = 0;
    } else {
        quant_matrix = s->c.chroma_intra_matrix;
        component    = (n & 1) + 1;
    }
    diff = decode_dc(&s->gb, component);
    dc  = s->c.last_dc[component];
    dc += diff;
    s->c.last_dc[component] = dc;
    block[0] = dc * (1 << (3 - s->c.intra_dc_precision));
    ff_tlog(s->c.avctx, "dc=%d\n", block[0]);
    mismatch = block[0] ^ 1;
    i = 0;
    if (s->c.intra_vlc_format)
        rl_vlc = ff_mpeg2_rl_vlc;
    else
        rl_vlc = ff_mpeg1_rl_vlc;

    {
        OPEN_READER(re, &s->gb);
        /* now quantify & encode AC coefficients */
        for (;;) {
            UPDATE_CACHE(re, &s->gb);
            GET_RL_VLC(level, run, re, &s->gb, rl_vlc,
                       TEX_VLC_BITS, 2, 0);

            if (level == 127) {
                break;
            } else if (level != 0) {
                i += run;
                if (i > MAX_INDEX)
                    break;
                j = scantable[i];
                level = (level * qscale * quant_matrix[j]) >> 4;
                level = (level ^ SHOW_SBITS(re, &s->gb, 1)) -
                        SHOW_SBITS(re, &s->gb, 1);
                LAST_SKIP_BITS(re, &s->gb, 1);
            } else {
                /* escape */
                run = SHOW_UBITS(re, &s->gb, 6) + 1;
                SKIP_BITS(re, &s->gb, 6);
                level = SHOW_SBITS(re, &s->gb, 12);
                LAST_SKIP_BITS(re, &s->gb, 12);
                i += run;
                if (i > MAX_INDEX)
                    break;
                j = scantable[i];
                if (level < 0) {
                    level = (-level * qscale * quant_matrix[j]) >> 4;
                    level = -level;
                } else {
                    level = (level * qscale * quant_matrix[j]) >> 4;
                }
            }

            mismatch ^= level;
            block[j]  = level;
        }
        CLOSE_READER(re, &s->gb);
    }
    block[63] ^= mismatch & 1;

    check_scantable_index(s, i);

    return 0;
}

static inline int get_dmv(Mpeg12SliceContext *const s)
{
    if (get_bits1(&s->gb))
        return 1 - (get_bits1(&s->gb) << 1);
    else
        return 0;
}

/* motion type (for MPEG-2) */
#define MT_FIELD 1
#define MT_FRAME 2
#define MT_16X8  2
#define MT_DMV   3

static int mpeg_decode_mb(Mpeg12SliceContext *const s, int *mb_skip_run)
{
    int i, j, k, cbp, val, mb_type, motion_type;
    const int mb_block_count = 4 + (1 << s->c.chroma_format);
    int ret;

    ff_tlog(s->c.avctx, "decode_mb: x=%d y=%d\n", s->c.mb_x, s->c.mb_y);

    av_assert2(s->c.mb_skipped == 0);

    if ((*mb_skip_run)-- != 0) {
        if (s->c.pict_type == AV_PICTURE_TYPE_P) {
            s->c.mb_skipped = 1;
            s->c.cur_pic.mb_type[s->c.mb_x + s->c.mb_y * s->c.mb_stride] =
                MB_TYPE_SKIP | MB_TYPE_FORWARD_MV | MB_TYPE_16x16;
        } else {
            int mb_type;

            if (s->c.mb_x)
                mb_type = s->c.cur_pic.mb_type[s->c.mb_x + s->c.mb_y * s->c.mb_stride - 1];
            else
                // FIXME not sure if this is allowed in MPEG at all
                mb_type = s->c.cur_pic.mb_type[s->c.mb_width + (s->c.mb_y - 1) * s->c.mb_stride - 1];
            if (IS_INTRA(mb_type)) {
                av_log(s->c.avctx, AV_LOG_ERROR, "skip with previntra\n");
                return AVERROR_INVALIDDATA;
            }
            s->c.cur_pic.mb_type[s->c.mb_x + s->c.mb_y * s->c.mb_stride] =
                mb_type | MB_TYPE_SKIP;

            if ((s->c.mv[0][0][0] | s->c.mv[0][0][1] | s->c.mv[1][0][0] | s->c.mv[1][0][1]) == 0)
                s->c.mb_skipped = 1;
        }

        return 0;
    }

    switch (s->c.pict_type) {
    default:
    case AV_PICTURE_TYPE_I:
        if (get_bits1(&s->gb) == 0) {
            if (get_bits1(&s->gb) == 0) {
                av_log(s->c.avctx, AV_LOG_ERROR,
                       "Invalid mb type in I-frame at %d %d\n",
                       s->c.mb_x, s->c.mb_y);
                return AVERROR_INVALIDDATA;
            }
            mb_type = MB_TYPE_QUANT | MB_TYPE_INTRA;
        } else {
            mb_type = MB_TYPE_INTRA;
        }
        break;
    case AV_PICTURE_TYPE_P:
        mb_type = get_vlc2(&s->gb, ff_mb_ptype_vlc, MB_PTYPE_VLC_BITS, 1);
        if (mb_type < 0) {
            av_log(s->c.avctx, AV_LOG_ERROR,
                   "Invalid mb type in P-frame at %d %d\n", s->c.mb_x, s->c.mb_y);
            return AVERROR_INVALIDDATA;
        }
        break;
    case AV_PICTURE_TYPE_B:
        mb_type = get_vlc2(&s->gb, ff_mb_btype_vlc, MB_BTYPE_VLC_BITS, 1);
        if (mb_type < 0) {
            av_log(s->c.avctx, AV_LOG_ERROR,
                   "Invalid mb type in B-frame at %d %d\n", s->c.mb_x, s->c.mb_y);
            return AVERROR_INVALIDDATA;
        }
        break;
    }
    ff_tlog(s->c.avctx, "mb_type=%x\n", mb_type);
//    motion_type = 0; /* avoid warning */
    if (IS_INTRA(mb_type)) {
        s->c.bdsp.clear_blocks(s->block[0]);

        if (!s->c.chroma_y_shift)
            s->c.bdsp.clear_blocks(s->block[6]);

        /* compute DCT type */
        // FIXME: add an interlaced_dct coded var?
        if (s->c.picture_structure == PICT_FRAME &&
            !s->c.frame_pred_frame_dct)
            s->c.interlaced_dct = get_bits1(&s->gb);

        if (IS_QUANT(mb_type))
            s->c.qscale = mpeg_get_qscale(&s->gb, s->c.q_scale_type);

        if (s->c.concealment_motion_vectors) {
            /* just parse them */
            if (s->c.picture_structure != PICT_FRAME)
                skip_bits1(&s->gb);  /* field select */

            s->c.mv[0][0][0]      =
            s->c.last_mv[0][0][0] =
            s->c.last_mv[0][1][0] = mpeg_decode_motion(s, s->c.mpeg_f_code[0][0],
                                                       s->c.last_mv[0][0][0]);
            s->c.mv[0][0][1]      =
            s->c.last_mv[0][0][1] =
            s->c.last_mv[0][1][1] = mpeg_decode_motion(s, s->c.mpeg_f_code[0][1],
                                                       s->c.last_mv[0][0][1]);

            check_marker(s->c.avctx, &s->gb, "after concealment_motion_vectors");
        } else {
            /* reset mv prediction */
            memset(s->c.last_mv, 0, sizeof(s->c.last_mv));
        }
        s->c.mb_intra = 1;

        if (s->c.codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            for (i = 0; i < mb_block_count; i++)
                if ((ret = mpeg2_decode_block_intra(s, s->block[i], i)) < 0)
                    return ret;
        } else {
            for (i = 0; i < 6; i++) {
                ret = ff_mpeg1_decode_block_intra(&s->gb,
                                                  s->c.intra_matrix,
                                                  s->c.intra_scantable.permutated,
                                                  s->c.last_dc, s->block[i],
                                                  i, s->c.qscale);
                if (ret < 0) {
                    av_log(s->c.avctx, AV_LOG_ERROR, "ac-tex damaged at %d %d\n",
                           s->c.mb_x, s->c.mb_y);
                    return ret;
                }
            }
        }
    } else {
        if (mb_type & MB_TYPE_ZERO_MV) {
            av_assert2(mb_type & MB_TYPE_CBP);

            s->c.mv_dir = MV_DIR_FORWARD;
            if (s->c.picture_structure == PICT_FRAME) {
                if (s->c.picture_structure == PICT_FRAME
                    && !s->c.frame_pred_frame_dct)
                    s->c.interlaced_dct = get_bits1(&s->gb);
                s->c.mv_type = MV_TYPE_16X16;
            } else {
                s->c.mv_type            = MV_TYPE_FIELD;
                mb_type              |= MB_TYPE_INTERLACED;
                s->c.field_select[0][0] = s->c.picture_structure - 1;
            }

            if (IS_QUANT(mb_type))
                s->c.qscale = mpeg_get_qscale(&s->gb, s->c.q_scale_type);

            s->c.last_mv[0][0][0] = 0;
            s->c.last_mv[0][0][1] = 0;
            s->c.last_mv[0][1][0] = 0;
            s->c.last_mv[0][1][1] = 0;
            s->c.mv[0][0][0]      = 0;
            s->c.mv[0][0][1]      = 0;
        } else {
            av_assert2(mb_type & MB_TYPE_BIDIR_MV);
            // FIXME decide if MBs in field pictures are MB_TYPE_INTERLACED
            /* get additional motion vector type */
            if (s->c.picture_structure == PICT_FRAME && s->c.frame_pred_frame_dct) {
                motion_type = MT_FRAME;
            } else {
                motion_type = get_bits(&s->gb, 2);
                if (s->c.picture_structure == PICT_FRAME && HAS_CBP(mb_type))
                    s->c.interlaced_dct = get_bits1(&s->gb);
            }

            if (IS_QUANT(mb_type))
                s->c.qscale = mpeg_get_qscale(&s->gb, s->c.q_scale_type);

            /* motion vectors */
            s->c.mv_dir = MB_TYPE_MV_2_MV_DIR(mb_type);
            ff_tlog(s->c.avctx, "motion_type=%d\n", motion_type);
            switch (motion_type) {
            case MT_FRAME: /* or MT_16X8 */
                if (s->c.picture_structure == PICT_FRAME) {
                    mb_type   |= MB_TYPE_16x16;
                    s->c.mv_type = MV_TYPE_16X16;
                    for (i = 0; i < 2; i++) {
                        if (HAS_MV(mb_type, i)) {
                            /* MT_FRAME */
                            s->c.mv[i][0][0]      =
                            s->c.last_mv[i][0][0] =
                            s->c.last_mv[i][1][0] =
                                mpeg_decode_motion(s, s->c.mpeg_f_code[i][0],
                                                   s->c.last_mv[i][0][0]);
                            s->c.mv[i][0][1]      =
                            s->c.last_mv[i][0][1] =
                            s->c.last_mv[i][1][1] =
                                mpeg_decode_motion(s, s->c.mpeg_f_code[i][1],
                                                   s->c.last_mv[i][0][1]);
                            /* full_pel: only for MPEG-1 */
                            if (s->c.full_pel[i]) {
                                s->c.mv[i][0][0] *= 2;
                                s->c.mv[i][0][1] *= 2;
                            }
                        }
                    }
                } else {
                    mb_type   |= MB_TYPE_16x8 | MB_TYPE_INTERLACED;
                    s->c.mv_type = MV_TYPE_16X8;
                    for (i = 0; i < 2; i++) {
                        if (HAS_MV(mb_type, i)) {
                            /* MT_16X8 */
                            for (j = 0; j < 2; j++) {
                                s->c.field_select[i][j] = get_bits1(&s->gb);
                                for (k = 0; k < 2; k++) {
                                    val = mpeg_decode_motion(s, s->c.mpeg_f_code[i][k],
                                                             s->c.last_mv[i][j][k]);
                                    s->c.last_mv[i][j][k] = val;
                                    s->c.mv[i][j][k]      = val;
                                }
                            }
                        }
                    }
                }
                break;
            case MT_FIELD:
                s->c.mv_type = MV_TYPE_FIELD;
                if (s->c.picture_structure == PICT_FRAME) {
                    mb_type |= MB_TYPE_16x8 | MB_TYPE_INTERLACED;
                    for (i = 0; i < 2; i++) {
                        if (HAS_MV(mb_type, i)) {
                            for (j = 0; j < 2; j++) {
                                s->c.field_select[i][j] = get_bits1(&s->gb);
                                val = mpeg_decode_motion(s, s->c.mpeg_f_code[i][0],
                                                         s->c.last_mv[i][j][0]);
                                s->c.last_mv[i][j][0] = val;
                                s->c.mv[i][j][0]      = val;
                                ff_tlog(s->c.avctx, "fmx=%d\n", val);
                                val = mpeg_decode_motion(s, s->c.mpeg_f_code[i][1],
                                                         s->c.last_mv[i][j][1] >> 1);
                                s->c.last_mv[i][j][1] = 2 * val;
                                s->c.mv[i][j][1]      = val;
                                ff_tlog(s->c.avctx, "fmy=%d\n", val);
                            }
                        }
                    }
                } else {
                    av_assert0(!s->c.progressive_sequence);
                    mb_type |= MB_TYPE_16x16 | MB_TYPE_INTERLACED;
                    for (i = 0; i < 2; i++) {
                        if (HAS_MV(mb_type, i)) {
                            s->c.field_select[i][0] = get_bits1(&s->gb);
                            for (k = 0; k < 2; k++) {
                                val = mpeg_decode_motion(s, s->c.mpeg_f_code[i][k],
                                                         s->c.last_mv[i][0][k]);
                                s->c.last_mv[i][0][k] = val;
                                s->c.last_mv[i][1][k] = val;
                                s->c.mv[i][0][k]      = val;
                            }
                        }
                    }
                }
                break;
            case MT_DMV:
                if (s->c.progressive_sequence){
                    av_log(s->c.avctx, AV_LOG_ERROR, "MT_DMV in progressive_sequence\n");
                    return AVERROR_INVALIDDATA;
                }
                s->c.mv_type = MV_TYPE_DMV;
                for (i = 0; i < 2; i++) {
                    if (HAS_MV(mb_type, i)) {
                        int dmx, dmy, mx, my, m;
                        const int my_shift = s->c.picture_structure == PICT_FRAME;

                        mx = mpeg_decode_motion(s, s->c.mpeg_f_code[i][0],
                                                s->c.last_mv[i][0][0]);
                        s->c.last_mv[i][0][0] = mx;
                        s->c.last_mv[i][1][0] = mx;
                        dmx = get_dmv(s);
                        my  = mpeg_decode_motion(s, s->c.mpeg_f_code[i][1],
                                                 s->c.last_mv[i][0][1] >> my_shift);
                        dmy = get_dmv(s);


                        s->c.last_mv[i][0][1] = my * (1 << my_shift);
                        s->c.last_mv[i][1][1] = my * (1 << my_shift);

                        s->c.mv[i][0][0] = mx;
                        s->c.mv[i][0][1] = my;
                        s->c.mv[i][1][0] = mx; // not used
                        s->c.mv[i][1][1] = my; // not used

                        if (s->c.picture_structure == PICT_FRAME) {
                            mb_type |= MB_TYPE_16x16 | MB_TYPE_INTERLACED;

                            // m = 1 + 2 * s->c.top_field_first;
                            m = s->c.top_field_first ? 1 : 3;

                            /* top -> top pred */
                            s->c.mv[i][2][0] = ((mx * m + (mx > 0)) >> 1) + dmx;
                            s->c.mv[i][2][1] = ((my * m + (my > 0)) >> 1) + dmy - 1;
                            m = 4 - m;
                            s->c.mv[i][3][0] = ((mx * m + (mx > 0)) >> 1) + dmx;
                            s->c.mv[i][3][1] = ((my * m + (my > 0)) >> 1) + dmy + 1;
                        } else {
                            mb_type |= MB_TYPE_16x16;

                            s->c.mv[i][2][0] = ((mx + (mx > 0)) >> 1) + dmx;
                            s->c.mv[i][2][1] = ((my + (my > 0)) >> 1) + dmy;
                            if (s->c.picture_structure == PICT_TOP_FIELD)
                                s->c.mv[i][2][1]--;
                            else
                                s->c.mv[i][2][1]++;
                        }
                    }
                }
                break;
            default:
                av_log(s->c.avctx, AV_LOG_ERROR,
                       "00 motion_type at %d %d\n", s->c.mb_x, s->c.mb_y);
                return AVERROR_INVALIDDATA;
            }
        }

        s->c.mb_intra = 0;
        s->c.last_dc[0] = s->c.last_dc[1] = s->c.last_dc[2] = 128 << s->c.intra_dc_precision;
        if (HAS_CBP(mb_type)) {
            s->c.bdsp.clear_blocks(s->block[0]);

            cbp = get_vlc2(&s->gb, ff_mb_pat_vlc, MB_PAT_VLC_BITS, 1);
            if (mb_block_count > 6) {
                cbp *= 1 << mb_block_count - 6;
                cbp |= get_bits(&s->gb, mb_block_count - 6);
                s->c.bdsp.clear_blocks(s->block[6]);
            }
            if (cbp <= 0) {
                av_log(s->c.avctx, AV_LOG_ERROR,
                       "invalid cbp %d at %d %d\n", cbp, s->c.mb_x, s->c.mb_y);
                return AVERROR_INVALIDDATA;
            }

            if (s->c.codec_id == AV_CODEC_ID_MPEG2VIDEO) {
                cbp <<= 12 - mb_block_count;

                for (i = 0; i < mb_block_count; i++) {
                    if (cbp & (1 << 11)) {
                        if ((ret = mpeg2_decode_block_non_intra(s, s->block[i], i)) < 0)
                            return ret;
                    } else {
                        s->c.block_last_index[i] = -1;
                    }
                    cbp += cbp;
                }
            } else {
                for (i = 0; i < 6; i++) {
                    if (cbp & 32) {
                        if ((ret = mpeg1_decode_block_inter(s, s->block[i], i)) < 0)
                            return ret;
                    } else {
                        s->c.block_last_index[i] = -1;
                    }
                    cbp += cbp;
                }
            }
        } else {
            for (i = 0; i < 12; i++)
                s->c.block_last_index[i] = -1;
        }
    }

    s->c.cur_pic.mb_type[s->c.mb_x + s->c.mb_y * s->c.mb_stride] = mb_type;

    return 0;
}

static av_cold int mpeg_decode_init(AVCodecContext *avctx)
{
    Mpeg1Context *s    = avctx->priv_data;
    MPVContext *const s2 = &s->slice.c;
    int ret;

    s2->slice_ctx_size = sizeof(s->slice);
    s2->out_format = FMT_MPEG1;

    // ==> Start patch MPC
    /*
    if (   avctx->codec_tag != AV_RL32("VCR2")
        && avctx->codec_tag != AV_RL32("BW10"))
        avctx->coded_width = avctx->coded_height = 0; // do not trust dimensions from input
    */
    // ==> End patch MPC
    ret = ff_mpv_decode_init(s2, avctx);
    if (ret < 0)
        return ret;

    ff_mpeg12_init_vlcs();

    s2->chroma_format  = CHROMA_420;
    avctx->color_range             = AVCOL_RANGE_MPEG;
    // ==> Start patch MPC
    if (avctx->extradata_size > 0 && avctx->extradata) {
        int ret, got_output;
        ret = decode_chunks(avctx, NULL, &got_output,
                            avctx->extradata, avctx->extradata_size);
        s->extradata_decoded = 1;
        if (ret < 0 && (avctx->err_recognition & AV_EF_EXPLODE)) {
            return ret;
        }

        Mpeg1Context* s1 = avctx->priv_data;
        MPVContext* c = &s1->slice.c;

        if (c->chroma_format < 2)
            avctx->pix_fmt = AV_PIX_FMT_YUV420P;
        else if (c->chroma_format == 2)
            avctx->pix_fmt = AV_PIX_FMT_YUV422P;
        else
            avctx->pix_fmt = AV_PIX_FMT_YUV444P;
    }
    // ==> End patch MPC
    return 0;
}

static const enum AVPixelFormat mpeg1_hwaccel_pixfmt_list_420[] = {
#if CONFIG_MPEG1_NVDEC_HWACCEL
    AV_PIX_FMT_CUDA,
#endif
#if CONFIG_MPEG1_VDPAU_HWACCEL
    AV_PIX_FMT_VDPAU,
#endif
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NONE
};

static const enum AVPixelFormat mpeg2_hwaccel_pixfmt_list_420[] = {
#if CONFIG_MPEG2_NVDEC_HWACCEL
    AV_PIX_FMT_CUDA,
#endif
#if CONFIG_MPEG2_VDPAU_HWACCEL
    AV_PIX_FMT_VDPAU,
#endif
#if CONFIG_MPEG2_DXVA2_HWACCEL
    AV_PIX_FMT_DXVA2_VLD,
#endif
#if CONFIG_MPEG2_D3D11VA_HWACCEL
    AV_PIX_FMT_D3D11VA_VLD,
    AV_PIX_FMT_D3D11,
#endif
#if CONFIG_MPEG2_D3D12VA_HWACCEL
    AV_PIX_FMT_D3D12,
#endif
#if CONFIG_MPEG2_VAAPI_HWACCEL
    AV_PIX_FMT_VAAPI,
#endif
#if CONFIG_MPEG2_VIDEOTOOLBOX_HWACCEL
    AV_PIX_FMT_VIDEOTOOLBOX,
#endif
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NONE
};

static const enum AVPixelFormat mpeg12_pixfmt_list_422[] = {
    AV_PIX_FMT_YUV422P,
    AV_PIX_FMT_NONE
};

static const enum AVPixelFormat mpeg12_pixfmt_list_444[] = {
    AV_PIX_FMT_YUV444P,
    AV_PIX_FMT_NONE
};

static enum AVPixelFormat mpeg_get_pixelformat(AVCodecContext *avctx)
{
    Mpeg1Context *s1  = avctx->priv_data;
    MPVContext *const s = &s1->slice.c;
    const enum AVPixelFormat *pix_fmts;

    if (CONFIG_GRAY && (avctx->flags & AV_CODEC_FLAG_GRAY))
        return AV_PIX_FMT_GRAY8;

    if (s->chroma_format < CHROMA_422)
        pix_fmts = avctx->codec_id == AV_CODEC_ID_MPEG1VIDEO ?
                                mpeg1_hwaccel_pixfmt_list_420 :
                                mpeg2_hwaccel_pixfmt_list_420;
    else if (s->chroma_format == CHROMA_422)
        pix_fmts = mpeg12_pixfmt_list_422;
    else
        pix_fmts = mpeg12_pixfmt_list_444;

    return ff_get_format(avctx, pix_fmts);
}

/* Call this function when we know all parameters.
 * It may be called in different places for MPEG-1 and MPEG-2. */
static int mpeg_decode_postinit(AVCodecContext *avctx)
{
    Mpeg1Context *s1  = avctx->priv_data;
    MPVContext *const s = &s1->slice.c;
    int ret;

    if (avctx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
        // MPEG-1 aspect
        AVRational aspect_inv = av_d2q(ff_mpeg1_aspect[s1->aspect_ratio_info], 255);
        avctx->sample_aspect_ratio = (AVRational) { aspect_inv.den, aspect_inv.num };
    } else { // MPEG-2
        // MPEG-2 aspect
        if (s1->aspect_ratio_info > 1) {
            AVRational dar =
                av_mul_q(av_div_q(ff_mpeg2_aspect[s1->aspect_ratio_info],
                                  (AVRational) { s1->pan_scan.width,
                                                 s1->pan_scan.height }),
                         (AVRational) { s->width, s->height });

            /* We ignore the spec here and guess a bit as reality does not
             * match the spec, see for example res_change_ffmpeg_aspect.ts
             * and sequence-display-aspect.mpg.
             * issue1613, 621, 562 */
            if ((s1->pan_scan.width == 0) || (s1->pan_scan.height == 0) ||
                (av_cmp_q(dar, (AVRational) { 4, 3 }) &&
                 av_cmp_q(dar, (AVRational) { 16, 9 }))) {
                s->avctx->sample_aspect_ratio =
                    av_div_q(ff_mpeg2_aspect[s1->aspect_ratio_info],
                             (AVRational) { s->width, s->height });
            } else {
                s->avctx->sample_aspect_ratio =
                    av_div_q(ff_mpeg2_aspect[s1->aspect_ratio_info],
                             (AVRational) { s1->pan_scan.width, s1->pan_scan.height });
// issue1613 4/3 16/9 -> 16/9
// res_change_ffmpeg_aspect.ts 4/3 225/44 ->4/3
// widescreen-issue562.mpg 4/3 16/9 -> 16/9
//                s->avctx->sample_aspect_ratio = av_mul_q(s->avctx->sample_aspect_ratio, (AVRational) {s->width, s->height});
                ff_dlog(avctx, "aspect A %d/%d\n",
                        ff_mpeg2_aspect[s1->aspect_ratio_info].num,
                        ff_mpeg2_aspect[s1->aspect_ratio_info].den);
                ff_dlog(avctx, "aspect B %d/%d\n", s->avctx->sample_aspect_ratio.num,
                        s->avctx->sample_aspect_ratio.den);
            }
        } else {
            s->avctx->sample_aspect_ratio =
                ff_mpeg2_aspect[s1->aspect_ratio_info];
        }
    } // MPEG-2

    if (av_image_check_sar(s->width, s->height,
                           avctx->sample_aspect_ratio) < 0) {
        av_log(avctx, AV_LOG_WARNING, "ignoring invalid SAR: %u/%u\n",
                avctx->sample_aspect_ratio.num,
                avctx->sample_aspect_ratio.den);
        avctx->sample_aspect_ratio = (AVRational){ 0, 1 };
    }

    if (!s->context_initialized                             ||
        avctx->coded_width       != s->width                ||
        avctx->coded_height      != s->height               ||
        s1->save_chroma_format   != s->chroma_format        ||
        (s1->save_progressive_seq != s->progressive_sequence && FFALIGN(s->height, 16) != FFALIGN(s->height, 32)) ||
        0) {
        if (s->context_initialized)
            ff_mpv_common_end(s);

        ret = ff_set_dimensions(avctx, s->width, s->height);
        if (ret < 0)
            return ret;

        if (avctx->codec_id == AV_CODEC_ID_MPEG2VIDEO && s1->bit_rate &&
            (s1->bit_rate != 0x3FFFF*400)) {
            avctx->rc_max_rate = s1->bit_rate;
        } else if (avctx->codec_id == AV_CODEC_ID_MPEG1VIDEO && s1->bit_rate &&
                   (s1->bit_rate != 0x3FFFF*400 || s1->vbv_delay != 0xFFFF)) {
            avctx->bit_rate = s1->bit_rate;
        }
        s1->save_progressive_seq = s->progressive_sequence;
        s1->save_chroma_format   = s->chroma_format;

        /* low_delay may be forced, in this case we will have B-frames
         * that behave like P-frames. */
        avctx->has_b_frames = !s->low_delay;

        if (avctx->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            // MPEG-1 fps
            avctx->framerate = ff_mpeg12_frame_rate_tab[s1->frame_rate_index];
            avctx->chroma_sample_location = AVCHROMA_LOC_CENTER;
        } else { // MPEG-2
            // MPEG-2 fps
            av_reduce(&s->avctx->framerate.num,
                      &s->avctx->framerate.den,
                      ff_mpeg12_frame_rate_tab[s1->frame_rate_index].num * s1->frame_rate_ext.num,
                      ff_mpeg12_frame_rate_tab[s1->frame_rate_index].den * s1->frame_rate_ext.den,
                      1 << 30);

            switch (s->chroma_format) {
            case CHROMA_420: avctx->chroma_sample_location = AVCHROMA_LOC_LEFT; break;
            case CHROMA_422:
            case CHROMA_444: avctx->chroma_sample_location = AVCHROMA_LOC_TOPLEFT; break;
            default: av_assert0(0);
            }
        } // MPEG-2

        avctx->pix_fmt = mpeg_get_pixelformat(avctx);

        if ((ret = ff_mpv_common_init(s)) < 0)
            return ret;
        if (!s->avctx->lowres)
            for (int i = 0; i < s->slice_context_count; i++)
                ff_mpv_framesize_disable(&s->thread_context[i]->sc);
    }
    return 0;
}

static int mpeg1_decode_picture(AVCodecContext *avctx, const uint8_t *buf,
                                int buf_size)
{
    Mpeg1Context *s1  = avctx->priv_data;
    MPVContext *const s = &s1->slice.c;
    GetBitContext gb0, *const gb = &gb0;
    int ref, f_code, vbv_delay, ret;

    ret = init_get_bits8(gb, buf, buf_size);
    if (ret < 0)
        return ret;

    ref = get_bits(gb, 10); /* temporal ref */
    s->pict_type = get_bits(gb, 3);
    if (s->pict_type == 0 || s->pict_type > 3)
        return AVERROR_INVALIDDATA;

    vbv_delay = get_bits(gb, 16);
    s1->vbv_delay = vbv_delay;
    if (s->pict_type == AV_PICTURE_TYPE_P ||
        s->pict_type == AV_PICTURE_TYPE_B) {
        s->full_pel[0] = get_bits1(gb);
        f_code = get_bits(gb, 3);
        if (f_code == 0 && (avctx->err_recognition & (AV_EF_BITSTREAM|AV_EF_COMPLIANT)))
            return AVERROR_INVALIDDATA;
        f_code += !f_code;
        s->mpeg_f_code[0][0] = f_code;
        s->mpeg_f_code[0][1] = f_code;
    }
    if (s->pict_type == AV_PICTURE_TYPE_B) {
        s->full_pel[1] = get_bits1(gb);
        f_code = get_bits(gb, 3);
        if (f_code == 0 && (avctx->err_recognition & (AV_EF_BITSTREAM|AV_EF_COMPLIANT)))
            return AVERROR_INVALIDDATA;
        f_code += !f_code;
        s->mpeg_f_code[1][0] = f_code;
        s->mpeg_f_code[1][1] = f_code;
    }

    if (avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(avctx, AV_LOG_DEBUG,
               "vbv_delay %d, ref %d type:%d\n", vbv_delay, ref, s->pict_type);

    return 0;
}

static void mpeg_decode_sequence_extension(Mpeg1Context *const s1,
                                           GetBitContext *const gb)
{
    MPVContext *const s = &s1->slice.c;
    int horiz_size_ext, vert_size_ext;
    int bit_rate_ext;

    skip_bits(gb, 1); /* profile and level esc*/
    s->avctx->profile       = get_bits(gb, 3);
    s->avctx->level         = get_bits(gb, 4);
    s->progressive_sequence = get_bits1(gb);   /* progressive_sequence */
    s->chroma_format        = get_bits(gb, 2); /* chroma_format 1=420, 2=422, 3=444 */

    if (!s->chroma_format) {
        s->chroma_format = CHROMA_420;
        av_log(s->avctx, AV_LOG_WARNING, "Chroma format invalid\n");
    }

    horiz_size_ext          = get_bits(gb, 2);
    vert_size_ext           = get_bits(gb, 2);
    s->width  |= (horiz_size_ext << 12);
    s->height |= (vert_size_ext  << 12);
    bit_rate_ext = get_bits(gb, 12);  /* XXX: handle it */
    s1->bit_rate += (bit_rate_ext << 18) * 400LL;
    check_marker(s->avctx, gb, "after bit rate extension");
    s->avctx->rc_buffer_size += get_bits(gb, 8) * 1024 * 16 << 10;

    s->low_delay = get_bits1(gb);
    if (s->avctx->flags & AV_CODEC_FLAG_LOW_DELAY)
        s->low_delay = 1;

    s1->frame_rate_ext.num = get_bits(gb, 2) + 1;
    s1->frame_rate_ext.den = get_bits(gb, 5) + 1;

    ff_dlog(s->avctx, "sequence extension\n");
    s->codec_id = s->avctx->codec_id = AV_CODEC_ID_MPEG2VIDEO;

    if (s->avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(s->avctx, AV_LOG_DEBUG,
               "profile: %d, level: %d ps: %d cf:%d vbv buffer: %d, bitrate:%"PRId64"\n",
               s->avctx->profile, s->avctx->level, s->progressive_sequence, s->chroma_format,
               s->avctx->rc_buffer_size, s1->bit_rate);
}

static void mpeg_decode_sequence_display_extension(Mpeg1Context *const s1,
                                                   GetBitContext *const gb)
{
    MPVContext *const s = &s1->slice.c;
    int color_description, w, h;

    skip_bits(gb, 3); /* video format */
    color_description = get_bits1(gb);
    if (color_description) {
        s->avctx->color_primaries = get_bits(gb, 8);
        s->avctx->color_trc       = get_bits(gb, 8);
        s->avctx->colorspace      = get_bits(gb, 8);
    }
    w = get_bits(gb, 14);
    skip_bits(gb, 1); // marker
    h = get_bits(gb, 14);
    // remaining 3 bits are zero padding

    s1->pan_scan.width  = 16 * w;
    s1->pan_scan.height = 16 * h;

    if (s->avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(s->avctx, AV_LOG_DEBUG, "sde w:%d, h:%d\n", w, h);
}

static void mpeg_decode_picture_display_extension(Mpeg1Context *const s1,
                                                  GetBitContext *const gb)
{
    MPVContext *const s = &s1->slice.c;
    int i, nofco;

    nofco = 1;
    if (s->progressive_sequence) {
        if (s->repeat_first_field) {
            nofco++;
            if (s->top_field_first)
                nofco++;
        }
    } else {
        if (s->picture_structure == PICT_FRAME) {
            nofco++;
            if (s->repeat_first_field)
                nofco++;
        }
    }
    for (i = 0; i < nofco; i++) {
        s1->pan_scan.position[i][0] = get_sbits(gb, 16);
        skip_bits(gb, 1); // marker
        s1->pan_scan.position[i][1] = get_sbits(gb, 16);
        skip_bits(gb, 1); // marker
    }

    if (s->avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(s->avctx, AV_LOG_DEBUG,
               "pde (%"PRId16",%"PRId16") (%"PRId16",%"PRId16") (%"PRId16",%"PRId16")\n",
               s1->pan_scan.position[0][0], s1->pan_scan.position[0][1],
               s1->pan_scan.position[1][0], s1->pan_scan.position[1][1],
               s1->pan_scan.position[2][0], s1->pan_scan.position[2][1]);
}

static int load_matrix(MPVContext *const s, GetBitContext *const gb,
                       uint16_t matrix0[64], uint16_t matrix1[64], int intra)
{
    int i;

    for (i = 0; i < 64; i++) {
        int j = s->idsp.idct_permutation[ff_zigzag_direct[i]];
        int v = get_bits(gb, 8);
        if (v == 0) {
            av_log(s->avctx, AV_LOG_ERROR, "matrix damaged\n");
            return AVERROR_INVALIDDATA;
        }
        if (intra && i == 0 && v != 8) {
            av_log(s->avctx, AV_LOG_DEBUG, "intra matrix specifies invalid DC quantizer %d, ignoring\n", v);
            v = 8; // needed by pink.mpg / issue1046
        }
        matrix0[j] = v;
        if (matrix1)
            matrix1[j] = v;
    }
    return 0;
}

static void mpeg_decode_quant_matrix_extension(MPVContext *const s,
                                               GetBitContext *const gb)
{
    ff_dlog(s->avctx, "matrix extension\n");

    if (get_bits1(gb))
        load_matrix(s, gb, s->chroma_intra_matrix, s->intra_matrix, 1);
    if (get_bits1(gb))
        load_matrix(s, gb, s->chroma_inter_matrix, s->inter_matrix, 0);
    if (get_bits1(gb))
        load_matrix(s, gb, s->chroma_intra_matrix, NULL, 1);
    if (get_bits1(gb))
        load_matrix(s, gb, s->chroma_inter_matrix, NULL, 0);
}

static int mpeg_decode_picture_coding_extension(Mpeg1Context *const s1,
                                                GetBitContext *const gb)
{
    MPVContext *const s = &s1->slice.c;

    s->full_pel[0]       = s->full_pel[1] = 0;
    s->mpeg_f_code[0][0] = get_bits(gb, 4);
    s->mpeg_f_code[0][1] = get_bits(gb, 4);
    s->mpeg_f_code[1][0] = get_bits(gb, 4);
    s->mpeg_f_code[1][1] = get_bits(gb, 4);
    s->mpeg_f_code[0][0] += !s->mpeg_f_code[0][0];
    s->mpeg_f_code[0][1] += !s->mpeg_f_code[0][1];
    s->mpeg_f_code[1][0] += !s->mpeg_f_code[1][0];
    s->mpeg_f_code[1][1] += !s->mpeg_f_code[1][1];
    if (!s->pict_type && s->context_initialized) {
        av_log(s->avctx, AV_LOG_ERROR, "Missing picture start code\n");
        if (s->avctx->err_recognition & AV_EF_EXPLODE)
            return AVERROR_INVALIDDATA;
        av_log(s->avctx, AV_LOG_WARNING, "Guessing pict_type from mpeg_f_code\n");
        if (s->mpeg_f_code[1][0] == 15 && s->mpeg_f_code[1][1] == 15) {
            if (s->mpeg_f_code[0][0] == 15 && s->mpeg_f_code[0][1] == 15)
                s->pict_type = AV_PICTURE_TYPE_I;
            else
                s->pict_type = AV_PICTURE_TYPE_P;
        } else
            s->pict_type = AV_PICTURE_TYPE_B;
    }

    s->intra_dc_precision         = get_bits(gb, 2);
    s->picture_structure          = get_bits(gb, 2);
    s->top_field_first            = get_bits1(gb);
    s->frame_pred_frame_dct       = get_bits1(gb);
    s->concealment_motion_vectors = get_bits1(gb);
    s->q_scale_type               = get_bits1(gb);
    s->intra_vlc_format           = get_bits1(gb);
    s->alternate_scan             = get_bits1(gb);
    s->repeat_first_field         = get_bits1(gb);
    s->chroma_420_type            = get_bits1(gb);
    s->progressive_frame          = get_bits1(gb);

    // We only initialize intra_scantable.permutated, as this is all we use.
    ff_permute_scantable(s->intra_scantable.permutated,
                         s->alternate_scan ? ff_alternate_vertical_scan : ff_zigzag_direct,
                         s->idsp.idct_permutation);

    /* composite display not parsed */
    ff_dlog(s->avctx, "intra_dc_precision=%d\n", s->intra_dc_precision);
    ff_dlog(s->avctx, "picture_structure=%d\n", s->picture_structure);
    ff_dlog(s->avctx, "top field first=%d\n", s->top_field_first);
    ff_dlog(s->avctx, "repeat first field=%d\n", s->repeat_first_field);
    ff_dlog(s->avctx, "conceal=%d\n", s->concealment_motion_vectors);
    ff_dlog(s->avctx, "intra_vlc_format=%d\n", s->intra_vlc_format);
    ff_dlog(s->avctx, "alternate_scan=%d\n", s->alternate_scan);
    ff_dlog(s->avctx, "frame_pred_frame_dct=%d\n", s->frame_pred_frame_dct);
    ff_dlog(s->avctx, "progressive_frame=%d\n", s->progressive_frame);

    return 0;
}

static int mpeg_field_start(Mpeg1Context *s1, const uint8_t *buf, int buf_size)
{
    MPVContext *const s = &s1->slice.c;
    AVCodecContext *avctx = s->avctx;
    int second_field = 0;
    int ret;

    if (!(avctx->flags2 & AV_CODEC_FLAG2_CHUNKS)) {
        if (s->mb_width * s->mb_height * 11LL / (33 * 2 * 8) > buf_size)
            return AVERROR_INVALIDDATA;
    }

    /* start frame decoding */
    if (s->first_field || s->picture_structure == PICT_FRAME) {
        AVFrameSideData *pan_scan;

        if ((ret = ff_mpv_frame_start(s, avctx)) < 0)
            return ret;

        if (s->picture_structure != PICT_FRAME) {
            s->cur_pic.ptr->f->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST *
                                        (s->picture_structure == PICT_TOP_FIELD);

            for (int i = 0; i < 3; i++) {
                if (s->picture_structure == PICT_BOTTOM_FIELD) {
                    s->cur_pic.data[i] = FF_PTR_ADD(s->cur_pic.data[i],
                                                    s->cur_pic.linesize[i]);
                }
                s->cur_pic.linesize[i]  *= 2;
            }
        }

        ff_mpeg_er_frame_start(s);

        /* first check if we must repeat the frame */
        s->cur_pic.ptr->f->repeat_pict = 0;
        if (s->repeat_first_field) {
            if (s->progressive_sequence) {
                if (s->top_field_first)
                    s->cur_pic.ptr->f->repeat_pict = 4;
                else
                    s->cur_pic.ptr->f->repeat_pict = 2;
            } else if (s->progressive_frame) {
                s->cur_pic.ptr->f->repeat_pict = 1;
            }
        }

        ret = ff_frame_new_side_data(s->avctx, s->cur_pic.ptr->f,
                                     AV_FRAME_DATA_PANSCAN, sizeof(s1->pan_scan),
                                     &pan_scan);
        if (ret < 0)
            return ret;
        if (pan_scan)
            memcpy(pan_scan->data, &s1->pan_scan, sizeof(s1->pan_scan));

        if (s1->a53_buf_ref) {
            ret = ff_frame_new_side_data_from_buf(
                s->avctx, s->cur_pic.ptr->f, AV_FRAME_DATA_A53_CC,
                &s1->a53_buf_ref);
            if (ret < 0)
                return ret;
        }

        if (s1->has_stereo3d) {
            AVStereo3D *stereo = av_stereo3d_create_side_data(s->cur_pic.ptr->f);
            if (!stereo)
                return AVERROR(ENOMEM);

            stereo->type = s1->stereo3d_type;
            s1->has_stereo3d = 0;
        }

        if (s1->has_afd) {
            AVFrameSideData *sd;
            ret = ff_frame_new_side_data(s->avctx, s->cur_pic.ptr->f,
                                         AV_FRAME_DATA_AFD, 1, &sd);
            if (ret < 0)
                return ret;
            if (sd)
                *sd->data = s1->afd;
            s1->has_afd = 0;
        }
    } else { // second field
        second_field = 1;
        if (!s->cur_pic.ptr) {
            av_log(s->avctx, AV_LOG_ERROR, "first field missing\n");
            return AVERROR_INVALIDDATA;
        }

        if (s->avctx->hwaccel) {
            if ((ret = FF_HW_SIMPLE_CALL(s->avctx, end_frame)) < 0) {
                av_log(avctx, AV_LOG_ERROR,
                       "hardware accelerator failed to decode first field\n");
                return ret;
            }
        }
        ret = ff_mpv_alloc_dummy_frames(s);
        if (ret < 0)
            return ret;

        for (int i = 0; i < 3; i++) {
            s->cur_pic.data[i] = s->cur_pic.ptr->f->data[i];
            if (s->picture_structure == PICT_BOTTOM_FIELD)
                s->cur_pic.data[i] +=
                    s->cur_pic.ptr->f->linesize[i];
        }
    }

    if (avctx->hwaccel) {
        if ((ret = FF_HW_CALL(avctx, start_frame, NULL, buf, buf_size)) < 0)
            return ret;
    } else if (s->codec_tag == MKTAG('V', 'C', 'R', '2')) {
        // Exchange UV
        FFSWAP(uint8_t*,  s->cur_pic.data[1],     s->cur_pic.data[2]);
        FFSWAP(ptrdiff_t, s->cur_pic.linesize[1], s->cur_pic.linesize[2]);
        if (!second_field) {
            FFSWAP(uint8_t*,  s->next_pic.data[1],     s->next_pic.data[2]);
            FFSWAP(ptrdiff_t, s->next_pic.linesize[1], s->next_pic.linesize[2]);
            FFSWAP(uint8_t*,  s->last_pic.data[1],     s->last_pic.data[2]);
            FFSWAP(ptrdiff_t, s->last_pic.linesize[1], s->last_pic.linesize[2]);
        }
    }

    return 0;
}

#define DECODE_SLICE_ERROR -1
#define DECODE_SLICE_OK     0

/**
 * Decode a slice.
 * Mpeg12SliceContext.c.mb_y must be set to the MB row from the startcode.
 * @return DECODE_SLICE_ERROR if the slice is damaged,
 *         DECODE_SLICE_OK if this slice is OK
 */
static int mpeg_decode_slice(Mpeg12SliceContext *const s, int mb_y,
                             const uint8_t **buf, int buf_size)
{
    AVCodecContext *avctx = s->c.avctx;
    const int lowres      = s->c.avctx->lowres;
    const int field_pic   = s->c.picture_structure != PICT_FRAME;
    int ret;

    s->c.resync_mb_x =
    s->c.resync_mb_y = -1;

    av_assert0(mb_y < s->c.mb_height);

    ret = init_get_bits8(&s->gb, *buf, buf_size);
    if (ret < 0)
        return ret;

    if (s->c.codec_id != AV_CODEC_ID_MPEG1VIDEO && s->c.mb_height > 2800/16)
        skip_bits(&s->gb, 3);

    ff_mpeg1_clean_buffers(&s->c);
    s->c.interlaced_dct = 0;

    s->c.qscale = mpeg_get_qscale(&s->gb, s->c.q_scale_type);

    if (s->c.qscale == 0) {
        av_log(s->c.avctx, AV_LOG_ERROR, "qscale == 0\n");
        return AVERROR_INVALIDDATA;
    }

    /* extra slice info */
    if (skip_1stop_8data_bits(&s->gb) < 0)
        return AVERROR_INVALIDDATA;

    s->c.mb_x = 0;

    if (mb_y == 0 && s->c.codec_tag == AV_RL32("SLIF")) {
        skip_bits1(&s->gb);
    } else {
        while (get_bits_left(&s->gb) > 0) {
            int code = get_vlc2(&s->gb, ff_mbincr_vlc,
                                MBINCR_VLC_BITS, 2);
            if (code < 0) {
                av_log(s->c.avctx, AV_LOG_ERROR, "first mb_incr damaged\n");
                return AVERROR_INVALIDDATA;
            }
            if (code >= 33) {
                if (code == 33)
                    s->c.mb_x += 33;
                /* otherwise, stuffing, nothing to do */
            } else {
                s->c.mb_x += code;
                break;
            }
        }
    }

    if (s->c.mb_x >= (unsigned) s->c.mb_width) {
        av_log(s->c.avctx, AV_LOG_ERROR, "initial skip overflow\n");
        return AVERROR_INVALIDDATA;
    }

    if (avctx->hwaccel) {
        const uint8_t *buf_end, *buf_start = *buf - 4; /* include start_code */
        int start_code = -1;
        buf_end = avpriv_find_start_code(buf_start + 2, *buf + buf_size, &start_code);
        if (buf_end < *buf + buf_size)
            buf_end -= 4;
        s->c.mb_y = mb_y;
        if (FF_HW_CALL(avctx, decode_slice, buf_start, buf_end - buf_start) < 0)
            return DECODE_SLICE_ERROR;
        *buf = buf_end;
        return DECODE_SLICE_OK;
    }

    s->c.resync_mb_x = s->c.mb_x;
    s->c.resync_mb_y = s->c.mb_y = mb_y;
    ff_init_block_index(&s->c);

    if (s->c.mb_y == 0 && s->c.mb_x == 0 && (s->c.first_field || s->c.picture_structure == PICT_FRAME)) {
        if (s->c.avctx->debug & FF_DEBUG_PICT_INFO) {
            av_log(s->c.avctx, AV_LOG_DEBUG,
                   "qp:%d fc:%2d%2d%2d%2d %c %s %s %s %s dc:%d pstruct:%d fdct:%d cmv:%d qtype:%d ivlc:%d rff:%d %s\n",
                   s->c.qscale,
                   s->c.mpeg_f_code[0][0], s->c.mpeg_f_code[0][1],
                   s->c.mpeg_f_code[1][0], s->c.mpeg_f_code[1][1],
                   s->c.pict_type  == AV_PICTURE_TYPE_I ? 'I' :
                   (s->c.pict_type == AV_PICTURE_TYPE_P ? 'P' :
                   (s->c.pict_type == AV_PICTURE_TYPE_B ? 'B' : 'S')),
                   s->c.progressive_sequence ? "ps"  : "",
                   s->c.progressive_frame    ? "pf"  : "",
                   s->c.alternate_scan       ? "alt" : "",
                   s->c.top_field_first      ? "top" : "",
                   s->c.intra_dc_precision, s->c.picture_structure,
                   s->c.frame_pred_frame_dct, s->c.concealment_motion_vectors,
                   s->c.q_scale_type, s->c.intra_vlc_format,
                   s->c.repeat_first_field, s->c.chroma_420_type ? "420" : "");
        }
    }

    for (int mb_skip_run = 0;;) {
        ret = mpeg_decode_mb(s, &mb_skip_run);
        if (ret < 0)
            return ret;

        // Note motion_val is normally NULL unless we want to extract the MVs.
        if (s->c.cur_pic.motion_val[0]) {
            const int wrap = s->c.b8_stride;
            int xy         = s->c.mb_x * 2 + s->c.mb_y * 2 * wrap;
            int b8_xy      = 4 * (s->c.mb_x + s->c.mb_y * s->c.mb_stride);
            int motion_x, motion_y, dir, i;

            for (i = 0; i < 2; i++) {
                for (dir = 0; dir < 2; dir++) {
                    if (s->c.mb_intra ||
                        (dir == 1 && s->c.pict_type != AV_PICTURE_TYPE_B)) {
                        motion_x = motion_y = 0;
                    } else if (s->c.mv_type == MV_TYPE_16X16 ||
                               (s->c.mv_type == MV_TYPE_FIELD && field_pic)) {
                        motion_x = s->c.mv[dir][0][0];
                        motion_y = s->c.mv[dir][0][1];
                    } else { /* if ((s->c.mv_type == MV_TYPE_FIELD) || (s->c.mv_type == MV_TYPE_16X8)) */
                        motion_x = s->c.mv[dir][i][0];
                        motion_y = s->c.mv[dir][i][1];
                    }

                    s->c.cur_pic.motion_val[dir][xy][0]     = motion_x;
                    s->c.cur_pic.motion_val[dir][xy][1]     = motion_y;
                    s->c.cur_pic.motion_val[dir][xy + 1][0] = motion_x;
                    s->c.cur_pic.motion_val[dir][xy + 1][1] = motion_y;
                    s->c.cur_pic.ref_index [dir][b8_xy]     =
                    s->c.cur_pic.ref_index [dir][b8_xy + 1] = s->c.field_select[dir][i];
                    av_assert2(s->c.field_select[dir][i] == 0 ||
                               s->c.field_select[dir][i] == 1);
                }
                xy    += wrap;
                b8_xy += 2;
            }
        }

        s->c.dest[0] += 16 >> lowres;
        s->c.dest[1] +=(16 >> lowres) >> s->c.chroma_x_shift;
        s->c.dest[2] +=(16 >> lowres) >> s->c.chroma_x_shift;

        ff_mpv_reconstruct_mb(&s->c, s->block);

        if (++s->c.mb_x >= s->c.mb_width) {
            const int mb_size = 16 >> s->c.avctx->lowres;
            int left;

            ff_mpeg_draw_horiz_band(&s->c, mb_size * (s->c.mb_y >> field_pic), mb_size);

            s->c.mb_x  = 0;
            s->c.mb_y += 1 << field_pic;

            if (s->c.mb_y >= s->c.mb_height) {
                int left   = get_bits_left(&s->gb);
                int is_d10 = s->c.chroma_format == CHROMA_422 &&
                             s->c.pict_type == AV_PICTURE_TYPE_I &&
                             avctx->profile == 0 && avctx->level == 5 &&
                             s->c.intra_dc_precision == 2 &&
                             s->c.q_scale_type == 1 && s->c.alternate_scan == 0 &&
                             s->c.progressive_frame == 0
                             /* vbv_delay == 0xBBB || 0xE10 */;

                if (left >= 32 && !is_d10) {
                    GetBitContext gb = s->gb;
                    align_get_bits(&gb);
                    if (show_bits(&gb, 24) == 0x060E2B) {
                        av_log(avctx, AV_LOG_DEBUG, "Invalid MXF data found in video stream\n");
                        is_d10 = 1;
                    }
                    if (left > 32 && show_bits_long(&gb, 32) == 0x201) {
                        av_log(avctx, AV_LOG_DEBUG, "skipping m704 alpha (unsupported)\n");
                        goto eos;
                    }
                }

                if (left < 0 ||
                    (left && show_bits(&s->gb, FFMIN(left, 23)) && !is_d10) ||
                    ((avctx->err_recognition & (AV_EF_BITSTREAM | AV_EF_AGGRESSIVE)) && left > 8)) {
                    av_log(avctx, AV_LOG_ERROR, "end mismatch left=%d %0X at %d %d\n",
                           left, left>0 ? show_bits(&s->gb, FFMIN(left, 23)) : 0, s->c.mb_x, s->c.mb_y);
                    return AVERROR_INVALIDDATA;
                } else
                    goto eos;
            }
            // There are some files out there which are missing the last slice
            // in cases where the slice is completely outside the visible
            // area, we detect this here instead of running into the end expecting
            // more data
            left = get_bits_left(&s->gb);
            if (s->c.mb_y >= ((s->c.height + 15) >> 4) &&
                !s->c.progressive_sequence &&
                left <= 25 &&
                left >= 0 &&
                mb_skip_run == -1 &&
                (!left || show_bits(&s->gb, left) == 0))
                goto eos;

            ff_init_block_index(&s->c);
        }

        /* skip mb handling */
        if (mb_skip_run == -1) {
            /* read increment again */
            mb_skip_run = 0;
            for (;;) {
                int code = get_vlc2(&s->gb, ff_mbincr_vlc,
                                    MBINCR_VLC_BITS, 2);
                if (code < 0) {
                    av_log(s->c.avctx, AV_LOG_ERROR, "mb incr damaged\n");
                    return AVERROR_INVALIDDATA;
                }
                if (code >= 33) {
                    if (code == 33) {
                        mb_skip_run += 33;
                    } else if (code == 35) {
                        if (mb_skip_run != 0 || show_bits(&s->gb, 15) != 0) {
                            av_log(s->c.avctx, AV_LOG_ERROR, "slice mismatch\n");
                            return AVERROR_INVALIDDATA;
                        }
                        goto eos; /* end of slice */
                    }
                    /* otherwise, stuffing, nothing to do */
                } else {
                    mb_skip_run += code;
                    break;
                }
            }
            if (mb_skip_run) {
                int i;
                if (s->c.pict_type == AV_PICTURE_TYPE_I) {
                    av_log(s->c.avctx, AV_LOG_ERROR,
                           "skipped MB in I-frame at %d %d\n", s->c.mb_x, s->c.mb_y);
                    return AVERROR_INVALIDDATA;
                }

                /* skip mb */
                s->c.mb_intra = 0;
                for (i = 0; i < 12; i++)
                    s->c.block_last_index[i] = -1;
                s->c.last_dc[0] = s->c.last_dc[1] = s->c.last_dc[2] = 128 << s->c.intra_dc_precision;
                if (s->c.picture_structure == PICT_FRAME)
                    s->c.mv_type = MV_TYPE_16X16;
                else
                    s->c.mv_type = MV_TYPE_FIELD;
                if (s->c.pict_type == AV_PICTURE_TYPE_P) {
                    /* if P type, zero motion vector is implied */
                    s->c.mv_dir             = MV_DIR_FORWARD;
                    s->c.mv[0][0][0]        = s->c.mv[0][0][1]      = 0;
                    s->c.last_mv[0][0][0]   = s->c.last_mv[0][0][1] = 0;
                    s->c.last_mv[0][1][0]   = s->c.last_mv[0][1][1] = 0;
                    s->c.field_select[0][0] = (s->c.picture_structure - 1) & 1;
                } else {
                    /* if B type, reuse previous vectors and directions */
                    s->c.mv[0][0][0] = s->c.last_mv[0][0][0];
                    s->c.mv[0][0][1] = s->c.last_mv[0][0][1];
                    s->c.mv[1][0][0] = s->c.last_mv[1][0][0];
                    s->c.mv[1][0][1] = s->c.last_mv[1][0][1];
                    s->c.field_select[0][0] = (s->c.picture_structure - 1) & 1;
                    s->c.field_select[1][0] = (s->c.picture_structure - 1) & 1;
                }
            }
        }
    }
eos: // end of slice
    if (get_bits_left(&s->gb) < 0) {
        av_log(s->c.avctx, AV_LOG_ERROR, "overread %d\n", -get_bits_left(&s->gb));
        return AVERROR_INVALIDDATA;
    }
    *buf += (get_bits_count(&s->gb) - 1) / 8;
    ff_dlog(s->c.avctx, "Slice start:%d %d  end:%d %d\n", s->c.resync_mb_x, s->c.resync_mb_y, s->c.mb_x, s->c.mb_y);
    return 0;
}

static int slice_decode_thread(AVCodecContext *c, void *arg)
{
    Mpeg12SliceContext *const s = *(void **) arg;
    const uint8_t *buf  = s->gb.buffer;
    const uint8_t *end  = buf + get_bits_bytesize(&s->gb, 0);
    int mb_y            = s->c.start_mb_y;
    const int field_pic = s->c.picture_structure != PICT_FRAME;

    s->c.er.error_count = (3 * (s->c.end_mb_y - s->c.start_mb_y) * s->c.mb_width) >> field_pic;

    for (;;) {
        uint32_t start_code;
        int ret;

        ret = mpeg_decode_slice(s, mb_y, &buf, end - buf);
        emms_c();
        ff_dlog(c, "ret:%d resync:%d/%d mb:%d/%d ts:%d/%d ec:%d\n",
                ret, s->c.resync_mb_x, s->c.resync_mb_y, s->c.mb_x, s->c.mb_y,
                s->c.start_mb_y, s->c.end_mb_y, s->c.er.error_count);
        if (ret < 0) {
            if (c->err_recognition & AV_EF_EXPLODE)
                return ret;
            if (s->c.resync_mb_x >= 0 && s->c.resync_mb_y >= 0)
                ff_er_add_slice(&s->c.er, s->c.resync_mb_x, s->c.resync_mb_y,
                                s->c.mb_x, s->c.mb_y,
                                ER_AC_ERROR | ER_DC_ERROR | ER_MV_ERROR);
        } else {
            ff_er_add_slice(&s->c.er, s->c.resync_mb_x, s->c.resync_mb_y,
                            s->c.mb_x - 1, s->c.mb_y,
                            ER_AC_END | ER_DC_END | ER_MV_END);
        }

        if (s->c.mb_y == s->c.end_mb_y)
            return 0;

        start_code = -1;
        buf        = avpriv_find_start_code(buf, end, &start_code);
        if (start_code < SLICE_MIN_START_CODE || start_code > SLICE_MAX_START_CODE)
            return AVERROR_INVALIDDATA;
        mb_y       = start_code - SLICE_MIN_START_CODE;
        if (s->c.codec_id != AV_CODEC_ID_MPEG1VIDEO && s->c.mb_height > 2800/16)
            mb_y += (*buf&0xE0)<<2;
        mb_y <<= field_pic;
        if (s->c.picture_structure == PICT_BOTTOM_FIELD)
            mb_y++;
        if (mb_y >= s->c.end_mb_y)
            return AVERROR_INVALIDDATA;
    }
}

/**
 * Handle slice ends.
 * @return 1 if it seems to be the last slice
 */
static int slice_end(AVCodecContext *avctx, AVFrame *pict, int *got_output)
{
    Mpeg1Context *s1  = avctx->priv_data;
    MPVContext *const s = &s1->slice.c;

    if (!s->context_initialized || !s->cur_pic.ptr)
        return 0;

    if (s->avctx->hwaccel) {
        int ret = FF_HW_SIMPLE_CALL(s->avctx, end_frame);
        if (ret < 0) {
            av_log(avctx, AV_LOG_ERROR,
                   "hardware accelerator failed to decode picture\n");
            return ret;
        }
    }

    /* end of slice reached */
    if (/* s->mb_y << field_pic == s->mb_height && */ !s->first_field && !s1->first_slice) {
        /* end of image */

        ff_er_frame_end(&s->er, NULL);

        ff_mpv_frame_end(s);

        if (s->pict_type == AV_PICTURE_TYPE_B || s->low_delay) {
            int ret = av_frame_ref(pict, s->cur_pic.ptr->f);
            if (ret < 0)
                return ret;
            ff_print_debug_info(s, s->cur_pic.ptr, pict);
            ff_mpv_export_qp_table(s, pict, s->cur_pic.ptr, FF_MPV_QSCALE_TYPE_MPEG2);
            *got_output = 1;
        } else {
            /* latency of 1 frame for I- and P-frames */
            if (s->last_pic.ptr && !s->last_pic.ptr->dummy) {
                int ret = av_frame_ref(pict, s->last_pic.ptr->f);
                if (ret < 0)
                    return ret;
                ff_print_debug_info(s, s->last_pic.ptr, pict);
                ff_mpv_export_qp_table(s, pict, s->last_pic.ptr, FF_MPV_QSCALE_TYPE_MPEG2);
                *got_output = 1;
            }
        }

        return 1;
    } else {
        return 0;
    }
}

static int mpeg1_decode_sequence(AVCodecContext *avctx,
                                 const uint8_t *buf, int buf_size)
{
    Mpeg1Context *s1  = avctx->priv_data;
    MPVContext *const s = &s1->slice.c;
    GetBitContext gb0, *const gb = &gb0;
    int width, height;
    int i, v, j;

    int ret = init_get_bits8(gb, buf, buf_size);
    if (ret < 0)
        return ret;

    width  = get_bits(gb, 12);
    height = get_bits(gb, 12);
    if (width == 0 || height == 0) {
        av_log(avctx, AV_LOG_WARNING,
               "Invalid horizontal or vertical size value.\n");
        if (avctx->err_recognition & (AV_EF_BITSTREAM | AV_EF_COMPLIANT))
            return AVERROR_INVALIDDATA;
    }
    s1->aspect_ratio_info = get_bits(gb, 4);
    if (s1->aspect_ratio_info == 0) {
        av_log(avctx, AV_LOG_ERROR, "aspect ratio has forbidden 0 value\n");
        if (avctx->err_recognition & (AV_EF_BITSTREAM | AV_EF_COMPLIANT))
            return AVERROR_INVALIDDATA;
    }
    s1->frame_rate_index = get_bits(gb, 4);
    if (s1->frame_rate_index == 0 || s1->frame_rate_index > 13) {
        av_log(avctx, AV_LOG_WARNING,
               "frame_rate_index %d is invalid\n", s1->frame_rate_index);
        s1->frame_rate_index = 1;
    }
    s1->bit_rate = get_bits(gb, 18) * 400;
    if (check_marker(s->avctx, gb, "in sequence header") == 0) {
        return AVERROR_INVALIDDATA;
    }

    s->avctx->rc_buffer_size = get_bits(gb, 10) * 1024 * 16;
    skip_bits(gb, 1);

    /* get matrix */
    if (get_bits1(gb)) {
        load_matrix(s, gb, s->chroma_intra_matrix, s->intra_matrix, 1);
    } else {
        for (i = 0; i < 64; i++) {
            j = s->idsp.idct_permutation[i];
            v = ff_mpeg1_default_intra_matrix[i];
            s->intra_matrix[j]        = v;
            s->chroma_intra_matrix[j] = v;
        }
    }
    if (get_bits1(gb)) {
        load_matrix(s, gb, s->chroma_inter_matrix, s->inter_matrix, 0);
    } else {
        for (i = 0; i < 64; i++) {
            int j = s->idsp.idct_permutation[i];
            v = ff_mpeg1_default_non_intra_matrix[i];
            s->inter_matrix[j]        = v;
            s->chroma_inter_matrix[j] = v;
        }
    }

    if (show_bits(gb, 23) != 0) {
        av_log(s->avctx, AV_LOG_ERROR, "sequence header damaged\n");
        return AVERROR_INVALIDDATA;
    }

    s->width  = width;
    s->height = height;

    /* We set MPEG-2 parameters so that it emulates MPEG-1. */
    s->progressive_sequence = 1;
    s->progressive_frame    = 1;
    s->picture_structure    = PICT_FRAME;
    s->first_field          = 0;
    s->frame_pred_frame_dct = 1;
    s->chroma_format        = CHROMA_420;
    s->codec_id             =
    s->avctx->codec_id      = AV_CODEC_ID_MPEG1VIDEO;
    if (s->avctx->flags & AV_CODEC_FLAG_LOW_DELAY)
        s->low_delay = 1;

    if (s->avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(s->avctx, AV_LOG_DEBUG, "vbv buffer: %d, bitrate:%"PRId64", aspect_ratio_info: %d \n",
               s->avctx->rc_buffer_size, s1->bit_rate, s1->aspect_ratio_info);

    return 0;
}

static int vcr2_init_sequence(AVCodecContext *avctx)
{
    Mpeg1Context *s1  = avctx->priv_data;
    MPVContext *const s = &s1->slice.c;
    int i, v, ret;

    /* start new MPEG-1 context decoding */
    if (s->context_initialized)
        ff_mpv_common_end(s);

    s->width            = avctx->coded_width;
    s->height           = avctx->coded_height;
    avctx->has_b_frames = 0; // true?
    s->low_delay        = 1;

    avctx->pix_fmt = mpeg_get_pixelformat(avctx);

    if ((ret = ff_mpv_common_init(s)) < 0)
        return ret;
    if (!s->avctx->lowres)
        for (int i = 0; i < s->slice_context_count; i++)
            ff_mpv_framesize_disable(&s->thread_context[i]->sc);

    for (i = 0; i < 64; i++) {
        int j = s->idsp.idct_permutation[i];
        v = ff_mpeg1_default_intra_matrix[i];
        s->intra_matrix[j]        = v;
        s->chroma_intra_matrix[j] = v;

        v = ff_mpeg1_default_non_intra_matrix[i];
        s->inter_matrix[j]        = v;
        s->chroma_inter_matrix[j] = v;
    }

    s->progressive_sequence  = 1;
    s->progressive_frame     = 1;
    s->picture_structure     = PICT_FRAME;
    s->first_field           = 0;
    s->frame_pred_frame_dct  = 1;
    s->chroma_format         = CHROMA_420;
    if (s->codec_tag == AV_RL32("BW10")) {
        s->codec_id              = s->avctx->codec_id = AV_CODEC_ID_MPEG1VIDEO;
    } else {
        s->codec_id              = s->avctx->codec_id = AV_CODEC_ID_MPEG2VIDEO;
    }
    s1->save_progressive_seq = s->progressive_sequence;
    s1->save_chroma_format   = s->chroma_format;
    return 0;
}

static void mpeg_set_cc_format(AVCodecContext *avctx, enum Mpeg2ClosedCaptionsFormat format,
                               const char *label)
{
    Mpeg1Context *s1 = avctx->priv_data;

    av_assert2(format != CC_FORMAT_AUTO);

    if (!s1->cc_format) {
        s1->cc_format = format;

        av_log(avctx, AV_LOG_DEBUG, "CC: first seen substream is %s format\n", label);
    }

#if FF_API_CODEC_PROPS
FF_DISABLE_DEPRECATION_WARNINGS
    avctx->properties |= FF_CODEC_PROPERTY_CLOSED_CAPTIONS;
FF_ENABLE_DEPRECATION_WARNINGS
#endif
}

static int mpeg_decode_a53_cc(AVCodecContext *avctx,
                              const uint8_t *p, int buf_size)
{
    Mpeg1Context *s1 = avctx->priv_data;

    if ((!s1->cc_format || s1->cc_format == CC_FORMAT_A53_PART4) &&
        buf_size >= 6 &&
        p[0] == 'G' && p[1] == 'A' && p[2] == '9' && p[3] == '4' &&
        p[4] == 3 && (p[5] & 0x40)) {
        /* extract A53 Part 4 CC data */
        int cc_count = p[5] & 0x1f;
        if (cc_count > 0 && buf_size >= 7 + cc_count * 3) {
            int old_size = s1->a53_buf_ref ? s1->a53_buf_ref->size : 0;
            const uint64_t new_size = (old_size + cc_count
                                            * UINT64_C(3));
            int ret;

            if (new_size > 3*A53_MAX_CC_COUNT)
                return AVERROR(EINVAL);

            ret = av_buffer_realloc(&s1->a53_buf_ref, new_size);
            if (ret >= 0)
                memcpy(s1->a53_buf_ref->data + old_size, p + 7, cc_count * UINT64_C(3));

            mpeg_set_cc_format(avctx, CC_FORMAT_A53_PART4, "A/53 Part 4");
        }
        return 1;
    } else if ((!s1->cc_format || s1->cc_format == CC_FORMAT_SCTE20) &&
               buf_size >= 2 &&
               p[0] == 0x03 && (p[1]&0x7f) == 0x01) {
        /* extract SCTE-20 CC data */
        GetBitContext gb;
        int cc_count = 0;
        int i, ret;

        ret = init_get_bits8(&gb, p + 2, buf_size - 2);
        if (ret < 0)
            return ret;
        cc_count = get_bits(&gb, 5);
        if (cc_count > 0) {
            int old_size = s1->a53_buf_ref ? s1->a53_buf_ref->size : 0;
            const uint64_t new_size = (old_size + cc_count
                                            * UINT64_C(3));
            if (new_size > 3*A53_MAX_CC_COUNT)
                return AVERROR(EINVAL);

            ret = av_buffer_realloc(&s1->a53_buf_ref, new_size);
            if (ret >= 0) {
                uint8_t field, cc1, cc2;
                uint8_t *cap = s1->a53_buf_ref->data + old_size;

                memset(cap, 0, cc_count * 3);
                for (i = 0; i < cc_count && get_bits_left(&gb) >= 26; i++) {
                    skip_bits(&gb, 2); // priority
                    field = get_bits(&gb, 2);
                    skip_bits(&gb, 5); // line_offset
                    cc1 = get_bits(&gb, 8);
                    cc2 = get_bits(&gb, 8);
                    skip_bits(&gb, 1); // marker

                    if (!field) { // forbidden
                        cap[0] = cap[1] = cap[2] = 0x00;
                    } else {
                        field = (field == 2 ? 1 : 0);
                        if (!s1->slice.c.top_field_first) field = !field;
                        cap[0] = 0x04 | field;
                        cap[1] = ff_reverse[cc1];
                        cap[2] = ff_reverse[cc2];
                    }
                    cap += 3;
                }
            }

            mpeg_set_cc_format(avctx, CC_FORMAT_SCTE20, "SCTE-20");
        }
        return 1;
    } else if ((!s1->cc_format || s1->cc_format == CC_FORMAT_DVD) &&
               buf_size >= 11 &&
               p[0] == 'C' && p[1] == 'C' && p[2] == 0x01 && p[3] == 0xf8) {
        /* extract DVD CC data
         *
         * uint32_t   user_data_start_code        0x000001B2    (big endian)
         * uint16_t   user_identifier             0x4343 "CC"
         * uint8_t    user_data_type_code         0x01
         * uint8_t    caption_block_size          0xF8
         * uint8_t
         *   bit 7    caption_odd_field_first     1=odd field (CC1/CC2) first  0=even field (CC3/CC4) first
         *   bit 6    caption_filler              0
         *   bit 5:1  caption_block_count         number of caption blocks (pairs of caption words = frames). Most DVDs use 15 per start of GOP.
         *   bit 0    caption_extra_field_added   1=one additional caption word
         *
         * struct caption_field_block {
         *   uint8_t
         *     bit 7:1 caption_filler             0x7F (all 1s)
         *     bit 0   caption_field_odd          1=odd field (this is CC1/CC2)  0=even field (this is CC3/CC4)
         *   uint8_t   caption_first_byte
         *   uint8_t   caption_second_byte
         * } caption_block[(caption_block_count * 2) + caption_extra_field_added];
         *
         * Some DVDs encode caption data for both fields with caption_field_odd=1. The only way to decode the fields
         * correctly is to start on the field indicated by caption_odd_field_first and count between odd/even fields.
         * Don't assume that the first caption word is the odd field. There do exist MPEG files in the wild that start
         * on the even field. There also exist DVDs in the wild that encode an odd field count and the
         * caption_extra_field_added/caption_odd_field_first bits change per packet to allow that. */
        int cc_count = 0;
        int i, ret;
        // There is a caption count field in the data, but it is often
        // incorrect.  So count the number of captions present.
        for (i = 5; i + 6 <= buf_size && ((p[i] & 0xfe) == 0xfe); i += 6)
            cc_count++;
        // Transform the DVD format into A53 Part 4 format
        if (cc_count > 0) {
            int old_size = s1->a53_buf_ref ? s1->a53_buf_ref->size : 0;
            const uint64_t new_size = (old_size + cc_count
                                            * UINT64_C(6));
            if (new_size > 3*A53_MAX_CC_COUNT)
                return AVERROR(EINVAL);

            ret = av_buffer_realloc(&s1->a53_buf_ref, new_size);
            if (ret >= 0) {
                uint8_t field1 = !!(p[4] & 0x80);
                uint8_t *cap = s1->a53_buf_ref->data + old_size;
                p += 5;
                for (i = 0; i < cc_count; i++) {
                    cap[0] = (p[0] == 0xff && field1) ? 0xfc : 0xfd;
                    cap[1] = p[1];
                    cap[2] = p[2];
                    cap[3] = (p[3] == 0xff && !field1) ? 0xfc : 0xfd;
                    cap[4] = p[4];
                    cap[5] = p[5];
                    cap += 6;
                    p += 6;
                }
            }

            mpeg_set_cc_format(avctx, CC_FORMAT_DVD, "DVD");
        }
        return 1;
    } else if ((!s1->cc_format || s1->cc_format == CC_FORMAT_DISH) &&
               buf_size >= 12 &&
               p[0] == 0x05 && p[1] == 0x02) {
        /* extract Dish Network CC data */
        const uint8_t cc_header = 0xf8 | 0x04 /* valid */ | 0x00 /* line 21 field 1 */;
        uint8_t cc_data[4] = {0};
        int cc_count = 0;
        uint8_t cc_type = p[7];
        p += 8;
        buf_size -= 8;

        if (cc_type == 0x05 && buf_size >= 7) {
            cc_type = p[6];
            p += 7;
            buf_size -= 7;
        }

        if (cc_type == 0x02 && buf_size >= 4) { /* 2-byte caption, can be repeated */
            cc_count = 1;
            cc_data[0] = p[1];
            cc_data[1] = p[2];
            cc_type = p[3];

            /* Only repeat characters when the next type flag
             * is 0x04 and the characters are repeatable (i.e., less than
             * 32 with the parity stripped).
             */
            if (cc_type == 0x04 && (cc_data[0] & 0x7f) < 32) {
                cc_count = 2;
                cc_data[2] = cc_data[0];
                cc_data[3] = cc_data[1];
            }
        } else if (cc_type == 0x04 && buf_size >= 5) { /* 4-byte caption, not repeated */
            cc_count = 2;
            cc_data[0] = p[1];
            cc_data[1] = p[2];
            cc_data[2] = p[3];
            cc_data[3] = p[4];
        }

        if (cc_count > 0) {
            int ret;
            int old_size = s1->a53_buf_ref ? s1->a53_buf_ref->size : 0;
            const uint64_t new_size = (old_size + cc_count * UINT64_C(3));
            if (new_size > 3 * A53_MAX_CC_COUNT)
                return AVERROR(EINVAL);

            ret = av_buffer_realloc(&s1->a53_buf_ref, new_size);
            if (ret >= 0) {
                uint8_t *cap = s1->a53_buf_ref->data + old_size;
                cap[0] = cc_header;
                cap[1] = cc_data[0];
                cap[2] = cc_data[1];
                if (cc_count == 2) {
                    cap[3] = cc_header;
                    cap[4] = cc_data[2];
                    cap[5] = cc_data[3];
                }
            }

            mpeg_set_cc_format(avctx, CC_FORMAT_DISH, "Dish Network");
        }
        return 1;
    }
    return 0;
}

static void mpeg_decode_user_data(AVCodecContext *avctx,
                                  const uint8_t *p, int buf_size)
{
    const uint8_t *buf_end = p + buf_size;
    Mpeg1Context *s1 = avctx->priv_data;

#if 0
    int i;
    for(i=0; !(!p[i-2] && !p[i-1] && p[i]==1) && i<buf_size; i++){
        av_log(avctx, AV_LOG_ERROR, "%c", p[i]);
    }
    av_log(avctx, AV_LOG_ERROR, "\n");
#endif

    if (buf_size > 29){
        int i;
        for(i=0; i<20; i++)
            if (!memcmp(p+i, "\0TMPGEXS\0", 9)){
                s1->tmpgexs = 1;
            }
    }
    /* we parse the DTG active format information */
    if (buf_end - p >= 5 &&
        p[0] == 'D' && p[1] == 'T' && p[2] == 'G' && p[3] == '1') {
        int flags = p[4];
        p += 5;
        if (flags & 0x80) {
            /* skip event id */
            p += 2;
        }
        if (flags & 0x40) {
            if (buf_end - p < 1)
                return;
            s1->has_afd = 1;
            s1->afd     = p[0] & 0x0f;
        }
    } else if (buf_end - p >= 6 &&
               p[0] == 'J' && p[1] == 'P' && p[2] == '3' && p[3] == 'D' &&
               p[4] == 0x03) { // S3D_video_format_length
        // the 0x7F mask ignores the reserved_bit value
        const uint8_t S3D_video_format_type = p[5] & 0x7F;

        if (S3D_video_format_type == 0x03 ||
            S3D_video_format_type == 0x04 ||
            S3D_video_format_type == 0x08 ||
            S3D_video_format_type == 0x23) {

            s1->has_stereo3d = 1;

            switch (S3D_video_format_type) {
            case 0x03:
                s1->stereo3d_type = AV_STEREO3D_SIDEBYSIDE;
                break;
            case 0x04:
                s1->stereo3d_type = AV_STEREO3D_TOPBOTTOM;
                break;
            case 0x08:
                s1->stereo3d_type = AV_STEREO3D_2D;
                break;
            case 0x23:
                s1->stereo3d_type = AV_STEREO3D_SIDEBYSIDE_QUINCUNX;
                break;
            }
        }
    } else if (mpeg_decode_a53_cc(avctx, p, buf_size)) {
        return;
    }
}

static int mpeg_decode_gop(AVCodecContext *avctx,
                            const uint8_t *buf, int buf_size)
{
    Mpeg1Context *s1  = avctx->priv_data;
    MPVContext *const s = &s1->slice.c;
    GetBitContext gb0, *const gb = &gb0;
    int broken_link;
    int64_t tc;

    int ret = init_get_bits8(gb, buf, buf_size);
    if (ret < 0)
        return ret;

    tc = s1->timecode_frame_start = get_bits(gb, 25);

    s1->closed_gop = get_bits1(gb);
    /* broken_link indicates that after editing the
     * reference frames of the first B-Frames after GOP I-Frame
     * are missing (open gop) */
    broken_link = get_bits1(gb);

    if (s->avctx->debug & FF_DEBUG_PICT_INFO) {
        char tcbuf[AV_TIMECODE_STR_SIZE];
        av_timecode_make_mpeg_tc_string(tcbuf, tc);
        av_log(s->avctx, AV_LOG_DEBUG,
               "GOP (%s) closed_gop=%d broken_link=%d\n",
               tcbuf, s1->closed_gop, broken_link);
    }

    return 0;
}

static void mpeg12_execute_slice_threads(AVCodecContext *avctx,
                                         Mpeg1Context *const s)
{
    if (HAVE_THREADS && (avctx->active_thread_type & FF_THREAD_SLICE) &&
        !avctx->hwaccel) {
        MPVContext *const s2 = &s->slice.c;
        int error_count = 0;

        avctx->execute(avctx, slice_decode_thread,
                       s2->mpeg12_contexts, NULL,
                       s->slice_count, sizeof(s2->mpeg12_contexts[0]));

        for (int i = 0; i < s->slice_count; i++) {
            MpegEncContext *const slice = s2->thread_context[i];
            int slice_err = atomic_load_explicit(&slice->er.error_count,
                                                 memory_order_relaxed);
            // error_count can get set to INT_MAX on serious errors.
            // So use saturated addition.
            if ((unsigned)slice_err > INT_MAX - error_count) {
                error_count = INT_MAX;
                break;
            }
            error_count += slice_err;
        }
        atomic_store_explicit(&s2->er.error_count, error_count,
                              memory_order_relaxed);
    }
}

// ==> Start patch MPC
/*static */int decode_chunks(AVCodecContext *avctx, AVFrame *picture,
// ==> End patch MPC
                         int *got_output, const uint8_t *buf, int buf_size)
{
    Mpeg1Context *s = avctx->priv_data;
    MPVContext *const s2 = &s->slice.c;
    const uint8_t *buf_ptr = buf;
    const uint8_t *buf_end = buf + buf_size;
    int ret, input_size;
    int last_code = 0, skip_frame = 0;
    int picture_start_code_seen = 0;

    for (;;) {
        /* find next start code */
        uint32_t start_code = -1;
        buf_ptr = avpriv_find_start_code(buf_ptr, buf_end, &start_code);
        if (start_code > 0x1ff) {
            if (!skip_frame) {
                mpeg12_execute_slice_threads(avctx, s);

                ret = slice_end(avctx, picture, got_output);
                if (ret < 0)
                    return ret;
            }
            s2->pict_type = 0;

            if (avctx->err_recognition & AV_EF_EXPLODE && s2->er.error_count)
                return AVERROR_INVALIDDATA;

            return FFMAX(0, buf_ptr - buf);
        }

        input_size = buf_end - buf_ptr;

        if (avctx->debug & FF_DEBUG_STARTCODE)
            av_log(avctx, AV_LOG_DEBUG, "%3"PRIX32" at %"PTRDIFF_SPECIFIER" left %d\n",
                   start_code, buf_ptr - buf, input_size);

        /* prepare data for next start code */
        switch (start_code) {
        case SEQ_START_CODE:
            if (last_code == 0) {
                mpeg1_decode_sequence(avctx, buf_ptr, input_size);
                if (buf != avctx->extradata)
                    s->sync = 1;
            } else {
                av_log(avctx, AV_LOG_ERROR,
                       "ignoring SEQ_START_CODE after %X\n", last_code);
                if (avctx->err_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
            }
            break;

        case PICTURE_START_CODE:
            if (picture_start_code_seen && s2->picture_structure == PICT_FRAME) {
               /* If it's a frame picture, there can't be more than one picture header.
                  Yet, it does happen and we need to handle it. */
               av_log(avctx, AV_LOG_WARNING, "ignoring extra picture following a frame-picture\n");
               break;
            }
            picture_start_code_seen = 1;

            if (buf == avctx->extradata && avctx->codec_tag == AV_RL32("AVmp")) {
                av_log(avctx, AV_LOG_WARNING, "ignoring picture start code in AVmp extradata\n");
                break;
            }

            if (s2->width <= 0 || s2->height <= 0) {
                av_log(avctx, AV_LOG_ERROR, "Invalid frame dimensions %dx%d.\n",
                       s2->width, s2->height);
                return AVERROR_INVALIDDATA;
            }

            if (s->tmpgexs){
                s2->intra_dc_precision= 3;
                s2->intra_matrix[0]= 1;
            }
            if (s->slice_count) {
                mpeg12_execute_slice_threads(avctx, s);
                s->slice_count = 0;
            }
            if (last_code == 0 || last_code == SLICE_MIN_START_CODE) {
                ret = mpeg_decode_postinit(avctx);
                if (ret < 0) {
                    av_log(avctx, AV_LOG_ERROR,
                           "mpeg_decode_postinit() failure\n");
                    return ret;
                }

                /* We have a complete image: we try to decompress it. */
                if (mpeg1_decode_picture(avctx, buf_ptr, input_size) < 0)
                    s2->pict_type = 0;
                s->first_slice = 1;
                last_code      = PICTURE_START_CODE;
            } else {
                av_log(avctx, AV_LOG_ERROR,
                       "ignoring pic after %X\n", last_code);
                if (avctx->err_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
            }
            break;
        case EXT_START_CODE: {
            GetBitContext gb0, *const gb = &gb0;

            ret = init_get_bits8(gb, buf_ptr, input_size);
            if (ret < 0)
                return ret;

            switch (get_bits(gb, 4)) {
            case 0x1:
                if (last_code == 0) {
                    mpeg_decode_sequence_extension(s, gb);
                } else {
                    av_log(avctx, AV_LOG_ERROR,
                           "ignoring seq ext after %X\n", last_code);
                    if (avctx->err_recognition & AV_EF_EXPLODE)
                        return AVERROR_INVALIDDATA;
                }
                break;
            case 0x2:
                mpeg_decode_sequence_display_extension(s, gb);
                break;
            case 0x3:
                mpeg_decode_quant_matrix_extension(s2, gb);
                break;
            case 0x7:
                mpeg_decode_picture_display_extension(s, gb);
                break;
            case 0x8:
                if (last_code == PICTURE_START_CODE) {
                    int ret = mpeg_decode_picture_coding_extension(s, gb);
                    if (ret < 0)
                       return ret;
                } else {
                    av_log(avctx, AV_LOG_ERROR,
                           "ignoring pic cod ext after %X\n", last_code);
                    if (avctx->err_recognition & AV_EF_EXPLODE)
                        return AVERROR_INVALIDDATA;
                }
                break;
            }
            break;
        }
        case USER_START_CODE:
            mpeg_decode_user_data(avctx, buf_ptr, input_size);
            break;
        case GOP_START_CODE:
            if (last_code == 0) {
                s2->first_field = 0;
                ret = mpeg_decode_gop(avctx, buf_ptr, input_size);
                if (ret < 0)
                    return ret;
                s->sync = 1;
            } else {
                av_log(avctx, AV_LOG_ERROR,
                       "ignoring GOP_START_CODE after %X\n", last_code);
                if (avctx->err_recognition & AV_EF_EXPLODE)
                    return AVERROR_INVALIDDATA;
            }
            break;
        default:
            if (start_code >= SLICE_MIN_START_CODE &&
                start_code <= SLICE_MAX_START_CODE && last_code == PICTURE_START_CODE) {
                if (s2->progressive_sequence && !s2->progressive_frame) {
                    s2->progressive_frame = 1;
                    av_log(s2->avctx, AV_LOG_ERROR,
                           "interlaced frame in progressive sequence, ignoring\n");
                }

                if (s2->picture_structure == 0 ||
                    (s2->progressive_frame && s2->picture_structure != PICT_FRAME)) {
                    av_log(s2->avctx, AV_LOG_ERROR,
                           "picture_structure %d invalid, ignoring\n",
                           s2->picture_structure);
                    s2->picture_structure = PICT_FRAME;
                }

                if (s2->progressive_sequence && !s2->frame_pred_frame_dct)
                    av_log(s2->avctx, AV_LOG_WARNING, "invalid frame_pred_frame_dct\n");

                if (s2->picture_structure == PICT_FRAME) {
                    s2->first_field = 0;
                    s2->v_edge_pos  = 16 * s2->mb_height;
                } else {
                    s2->first_field ^= 1;
                    s2->v_edge_pos   = 8 * s2->mb_height;
                    memset(s2->mbskip_table, 0, s2->mb_stride * s2->mb_height);
                }
            }
            if (start_code >= SLICE_MIN_START_CODE &&
                start_code <= SLICE_MAX_START_CODE && last_code != 0) {
                const int field_pic = s2->picture_structure != PICT_FRAME;
                int mb_y = start_code - SLICE_MIN_START_CODE;
                last_code = SLICE_MIN_START_CODE;
                if (s2->codec_id != AV_CODEC_ID_MPEG1VIDEO && s2->mb_height > 2800/16)
                    mb_y += (*buf_ptr&0xE0)<<2;

                mb_y <<= field_pic;
                if (s2->picture_structure == PICT_BOTTOM_FIELD)
                    mb_y++;

                if (buf_end - buf_ptr < 2) {
                    av_log(s2->avctx, AV_LOG_ERROR, "slice too small\n");
                    return AVERROR_INVALIDDATA;
                }

                if (mb_y >= s2->mb_height) {
                    av_log(s2->avctx, AV_LOG_ERROR,
                           "slice below image (%d >= %d)\n", mb_y, s2->mb_height);
                    return AVERROR_INVALIDDATA;
                }

                if (!s2->last_pic.ptr) {
                    /* Skip B-frames if we do not have reference frames and
                     * GOP is not closed. */
                    if (s2->pict_type == AV_PICTURE_TYPE_B) {
                        if (!s->closed_gop) {
                            skip_frame = 1;
                            av_log(s2->avctx, AV_LOG_DEBUG,
                                   "Skipping B slice due to open GOP\n");
                            break;
                        }
                    }
                }
                if (s2->pict_type == AV_PICTURE_TYPE_I || (s2->avctx->flags2 & AV_CODEC_FLAG2_SHOW_ALL))
                    s->sync = 1;
                if (!s2->next_pic.ptr) {
                    /* Skip P-frames if we do not have a reference frame or
                     * we have an invalid header. */
                    if (s2->pict_type == AV_PICTURE_TYPE_P && !s->sync) {
                        skip_frame = 1;
                        av_log(s2->avctx, AV_LOG_DEBUG,
                               "Skipping P slice due to !sync\n");
                        break;
                    }
                }
                if ((avctx->skip_frame >= AVDISCARD_NONREF &&
                     s2->pict_type == AV_PICTURE_TYPE_B) ||
                    (avctx->skip_frame >= AVDISCARD_NONKEY &&
                     s2->pict_type != AV_PICTURE_TYPE_I) ||
                    avctx->skip_frame >= AVDISCARD_ALL) {
                    skip_frame = 1;
                    break;
                }

                if (!s2->context_initialized)
                    break;

                if (s2->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
                    if (mb_y < avctx->skip_top ||
                        mb_y >= s2->mb_height - avctx->skip_bottom)
                        break;
                }

                if (!s2->pict_type) {
                    av_log(avctx, AV_LOG_ERROR, "Missing picture start code\n");
                    if (avctx->err_recognition & AV_EF_EXPLODE)
                        return AVERROR_INVALIDDATA;
                    break;
                }

                if (s->first_slice) {
                    skip_frame     = 0;
                    s->first_slice = 0;
                    if ((ret = mpeg_field_start(s, buf, buf_size)) < 0)
                        return ret;
                }
                if (!s2->cur_pic.ptr) {
                    av_log(avctx, AV_LOG_ERROR,
                           "current_picture not initialized\n");
                    return AVERROR_INVALIDDATA;
                }

                if (HAVE_THREADS &&
                    (avctx->active_thread_type & FF_THREAD_SLICE) &&
                    !avctx->hwaccel) {
                    int threshold = (s2->mb_height * s->slice_count +
                                     s2->slice_context_count / 2) /
                                    s2->slice_context_count;
                    if (threshold <= mb_y) {
                        Mpeg12SliceContext *const thread_context = s2->mpeg12_contexts[s->slice_count];

                        thread_context->c.start_mb_y = mb_y;
                        thread_context->c.end_mb_y   = s2->mb_height;
                        if (s->slice_count) {
                            s2->thread_context[s->slice_count - 1]->end_mb_y = mb_y;
                            ret = ff_update_duplicate_context(&thread_context->c, s2);
                            if (ret < 0)
                                return ret;
                        }
                        ret = init_get_bits8(&thread_context->gb, buf_ptr, input_size);
                        if (ret < 0)
                            return ret;
                        s->slice_count++;
                    }
                    buf_ptr += 2; // FIXME add minimum number of bytes per slice
                } else {
                    ret = mpeg_decode_slice(&s->slice, mb_y, &buf_ptr, input_size);
                    emms_c();

                    if (ret < 0) {
                        if (avctx->err_recognition & AV_EF_EXPLODE)
                            return ret;
                        if (s2->resync_mb_x >= 0 && s2->resync_mb_y >= 0)
                            ff_er_add_slice(&s2->er, s2->resync_mb_x,
                                            s2->resync_mb_y, s2->mb_x, s2->mb_y,
                                            ER_AC_ERROR | ER_DC_ERROR | ER_MV_ERROR);
                    } else {
                        ff_er_add_slice(&s2->er, s2->resync_mb_x,
                                        s2->resync_mb_y, s2->mb_x - 1, s2->mb_y,
                                        ER_AC_END | ER_DC_END | ER_MV_END);
                    }
                }
            }
            break;
        }
    }
}

static int mpeg_decode_frame(AVCodecContext *avctx, AVFrame *picture,
                             int *got_output, AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int ret;
    int buf_size = avpkt->size;
    Mpeg1Context *s = avctx->priv_data;
    MPVContext *const s2 = &s->slice.c;

    if (buf_size == 0 || (buf_size == 4 && AV_RB32(buf) == SEQ_END_CODE)) {
        /* special case for last picture */
        if (s2->low_delay == 0 && s2->next_pic.ptr) {
            int ret = av_frame_ref(picture, s2->next_pic.ptr->f);
            if (ret < 0)
                return ret;

            ff_mpv_unref_picture(&s2->next_pic);

            *got_output = 1;
        }
        return buf_size;
    }

    if (!s2->context_initialized &&
        (s2->codec_tag == AV_RL32("VCR2") || s2->codec_tag == AV_RL32("BW10")))
        vcr2_init_sequence(avctx);

    s->slice_count = 0;

    if (avctx->extradata && !s->extradata_decoded) {
        ret = decode_chunks(avctx, picture, got_output,
                            avctx->extradata, avctx->extradata_size);
        if (*got_output) {
            av_log(avctx, AV_LOG_ERROR, "picture in extradata\n");
            av_frame_unref(picture);
            *got_output = 0;
        }
        s->extradata_decoded = 1;
        if (ret < 0 && (avctx->err_recognition & AV_EF_EXPLODE)) {
            ff_mpv_unref_picture(&s2->cur_pic);
            return ret;
        }
    }

    ret = decode_chunks(avctx, picture, got_output, buf, buf_size);
    if (ret<0 || *got_output) {
        ff_mpv_unref_picture(&s2->cur_pic);

        if (s->timecode_frame_start != -1 && *got_output) {
            char tcbuf[AV_TIMECODE_STR_SIZE];
            AVFrameSideData *tcside = av_frame_new_side_data(picture,
                                                             AV_FRAME_DATA_GOP_TIMECODE,
                                                             sizeof(int64_t));
            if (!tcside)
                return AVERROR(ENOMEM);
            memcpy(tcside->data, &s->timecode_frame_start, sizeof(int64_t));

            av_timecode_make_mpeg_tc_string(tcbuf, s->timecode_frame_start);
            av_dict_set(&picture->metadata, "timecode", tcbuf, 0);

            s->timecode_frame_start = -1;
        }
    }

    return ret;
}

static av_cold void flush(AVCodecContext *avctx)
{
    Mpeg1Context *s = avctx->priv_data;

    s->sync       = 0;
    s->closed_gop = 0;

    av_buffer_unref(&s->a53_buf_ref);
    ff_mpeg_flush(avctx);
}

static av_cold int mpeg_decode_end(AVCodecContext *avctx)
{
    Mpeg1Context *s = avctx->priv_data;

    av_buffer_unref(&s->a53_buf_ref);
    return ff_mpv_decode_close(avctx);
}

const FFCodec ff_mpeg1video_decoder = {
    .p.name                = "mpeg1video",
    CODEC_LONG_NAME("MPEG-1 video"),
    .p.type                = AVMEDIA_TYPE_VIDEO,
    .p.id                  = AV_CODEC_ID_MPEG1VIDEO,
    .priv_data_size        = sizeof(Mpeg1Context),
    .init                  = mpeg_decode_init,
    .close                 = mpeg_decode_end,
    FF_CODEC_DECODE_CB(mpeg_decode_frame),
    .p.capabilities        = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1 |
                             AV_CODEC_CAP_DELAY | AV_CODEC_CAP_SLICE_THREADS,
    .caps_internal         = FF_CODEC_CAP_SKIP_FRAME_FILL_PARAM,
    .flush                 = flush,
    .p.max_lowres          = 3,
    .hw_configs            = (const AVCodecHWConfigInternal *const []) {
#if CONFIG_MPEG1_NVDEC_HWACCEL
                               HWACCEL_NVDEC(mpeg1),
#endif
#if CONFIG_MPEG1_VDPAU_HWACCEL
                               HWACCEL_VDPAU(mpeg1),
#endif
#if CONFIG_MPEG1_VIDEOTOOLBOX_HWACCEL
                               HWACCEL_VIDEOTOOLBOX(mpeg1),
#endif
                               NULL
                           },
};

#define M2V_OFFSET(x) offsetof(Mpeg1Context, x)
#define M2V_PARAM     AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM

static const AVOption mpeg2video_options[] = {
    { "cc_format", "extract a specific Closed Captions format",
       M2V_OFFSET(cc_format), AV_OPT_TYPE_INT, { .i64 = CC_FORMAT_AUTO },
        CC_FORMAT_AUTO, CC_FORMAT_DISH, M2V_PARAM, .unit = "cc_format" },

       { "auto",   "pick first seen CC substream",  0, AV_OPT_TYPE_CONST,
        { .i64 =   CC_FORMAT_AUTO },                .flags = M2V_PARAM, .unit = "cc_format" },
       { "a53",    "pick A/53 Part 4 CC substream", 0, AV_OPT_TYPE_CONST,
        { .i64 =   CC_FORMAT_A53_PART4 },           .flags = M2V_PARAM, .unit = "cc_format" },
       { "scte20", "pick SCTE-20 CC substream",     0, AV_OPT_TYPE_CONST,
        { .i64 =   CC_FORMAT_SCTE20 },              .flags = M2V_PARAM, .unit = "cc_format" },
       { "dvd",    "pick DVD CC substream",         0, AV_OPT_TYPE_CONST,
        { .i64 =   CC_FORMAT_DVD },                 .flags = M2V_PARAM, .unit = "cc_format" },
       { "dish",   "pick Dish Network CC substream", 0, AV_OPT_TYPE_CONST,
        { .i64 =   CC_FORMAT_DISH },                .flags = M2V_PARAM, .unit = "cc_format" },
    { NULL }
};

static const AVClass mpeg2video_class = {
    .class_name = "MPEG-2 video",
    .item_name  = av_default_item_name,
    .option     = mpeg2video_options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DECODER,
};

const FFCodec ff_mpeg2video_decoder = {
    .p.name         = "mpeg2video",
    CODEC_LONG_NAME("MPEG-2 video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_MPEG2VIDEO,
    .p.priv_class   = &mpeg2video_class,
    .priv_data_size = sizeof(Mpeg1Context),
    .init           = mpeg_decode_init,
    .close          = mpeg_decode_end,
    FF_CODEC_DECODE_CB(mpeg_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1 |
                      AV_CODEC_CAP_DELAY | AV_CODEC_CAP_SLICE_THREADS,
    .caps_internal  = FF_CODEC_CAP_SKIP_FRAME_FILL_PARAM,
    .flush          = flush,
    .p.max_lowres   = 3,
    .p.profiles     = NULL_IF_CONFIG_SMALL(ff_mpeg2_video_profiles),
    .hw_configs     = (const AVCodecHWConfigInternal *const []) {
#if CONFIG_MPEG2_DXVA2_HWACCEL
                        HWACCEL_DXVA2(mpeg2),
#endif
#if CONFIG_MPEG2_D3D11VA_HWACCEL
                        HWACCEL_D3D11VA(mpeg2),
#endif
#if CONFIG_MPEG2_D3D11VA2_HWACCEL
                        HWACCEL_D3D11VA2(mpeg2),
#endif
#if CONFIG_MPEG2_D3D12VA_HWACCEL
                        HWACCEL_D3D12VA(mpeg2),
#endif
#if CONFIG_MPEG2_NVDEC_HWACCEL
                        HWACCEL_NVDEC(mpeg2),
#endif
#if CONFIG_MPEG2_VAAPI_HWACCEL
                        HWACCEL_VAAPI(mpeg2),
#endif
#if CONFIG_MPEG2_VDPAU_HWACCEL
                        HWACCEL_VDPAU(mpeg2),
#endif
#if CONFIG_MPEG2_VIDEOTOOLBOX_HWACCEL
                        HWACCEL_VIDEOTOOLBOX(mpeg2),
#endif
                        NULL
                    },
};

//legacy decoder
const FFCodec ff_mpegvideo_decoder = {
    .p.name         = "mpegvideo",
    CODEC_LONG_NAME("MPEG-1 video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_MPEG2VIDEO,
    .priv_data_size = sizeof(Mpeg1Context),
    .init           = mpeg_decode_init,
    .close          = mpeg_decode_end,
    FF_CODEC_DECODE_CB(mpeg_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DRAW_HORIZ_BAND | AV_CODEC_CAP_DR1 |
                      AV_CODEC_CAP_DELAY | AV_CODEC_CAP_SLICE_THREADS,
    .caps_internal  = FF_CODEC_CAP_SKIP_FRAME_FILL_PARAM,
    .flush          = flush,
    .p.max_lowres   = 3,
};

typedef struct IPUContext {
    Mpeg12SliceContext m;

    int flags;
} IPUContext;

static int ipu_decode_frame(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame, AVPacket *avpkt)
{
    IPUContext *s = avctx->priv_data;
    MPVContext *const m = &s->m.c;
    GetBitContext *const gb = &s->m.gb;
    int16_t (*const block)[64] = s->m.block;
    int ret;

    // Check for minimal intra MB size (considering mb header, luma & chroma dc VLC, ac EOB VLC)
    if (avpkt->size*8LL < (avctx->width+15)/16 * ((avctx->height+15)/16) * (2LL + 3*4 + 2*2 + 2*6))
        return AVERROR_INVALIDDATA;

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    ret = init_get_bits8(gb, avpkt->data, avpkt->size);
    if (ret < 0)
        return ret;

    s->flags = get_bits(gb, 8);
    m->intra_dc_precision = s->flags & 3;
    m->q_scale_type = !!(s->flags & 0x40);
    m->intra_vlc_format = !!(s->flags & 0x20);
    m->alternate_scan = !!(s->flags & 0x10);

    ff_permute_scantable(m->intra_scantable.permutated,
                         s->flags & 0x10 ? ff_alternate_vertical_scan : ff_zigzag_direct,
                         m->idsp.idct_permutation);

    m->last_dc[0] = m->last_dc[1] = m->last_dc[2] = 1 << (7 + (s->flags & 3));
    m->qscale = 1;

    for (int y = 0; y < avctx->height; y += 16) {
        int intraquant;

        for (int x = 0; x < avctx->width; x += 16) {
            if (x || y) {
                if (!get_bits1(gb))
                    return AVERROR_INVALIDDATA;
            }
            if (get_bits1(gb)) {
                intraquant = 0;
            } else {
                if (!get_bits1(gb))
                    return AVERROR_INVALIDDATA;
                intraquant = 1;
            }

            if (s->flags & 4)
                skip_bits1(gb);

            if (intraquant)
                m->qscale = mpeg_get_qscale(gb, m->q_scale_type);

            memset(block, 0, 6 * sizeof(*block));

            for (int n = 0; n < 6; n++) {
                if (s->flags & 0x80) {
                    ret = ff_mpeg1_decode_block_intra(gb,
                                                      m->intra_matrix,
                                                      m->intra_scantable.permutated,
                                                      m->last_dc, block[n],
                                                      n, m->qscale);
                } else {
                    ret = mpeg2_decode_block_intra(&s->m, block[n], n);
                }

                if (ret < 0)
                    return ret;
            }

            m->idsp.idct_put(frame->data[0] + y * frame->linesize[0] + x,
                             frame->linesize[0], block[0]);
            m->idsp.idct_put(frame->data[0] + y * frame->linesize[0] + x + 8,
                             frame->linesize[0], block[1]);
            m->idsp.idct_put(frame->data[0] + (y + 8) * frame->linesize[0] + x,
                             frame->linesize[0], block[2]);
            m->idsp.idct_put(frame->data[0] + (y + 8) * frame->linesize[0] + x + 8,
                             frame->linesize[0], block[3]);
            m->idsp.idct_put(frame->data[1] + (y >> 1) * frame->linesize[1] + (x >> 1),
                             frame->linesize[1], block[4]);
            m->idsp.idct_put(frame->data[2] + (y >> 1) * frame->linesize[2] + (x >> 1),
                             frame->linesize[2], block[5]);
        }
    }

    align_get_bits(gb);
    if (get_bits_left(gb) != 32)
        return AVERROR_INVALIDDATA;

    *got_frame = 1;

    return avpkt->size;
}

static av_cold int ipu_decode_init(AVCodecContext *avctx)
{
    IPUContext *s = avctx->priv_data;
    MPVContext *const m = &s->m.c;

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    m->avctx       = avctx;

    ff_idctdsp_init(&m->idsp, avctx);
    ff_mpeg12_init_vlcs();

    for (int i = 0; i < 64; i++) {
        int j = m->idsp.idct_permutation[i];
        int v = ff_mpeg1_default_intra_matrix[i];
        m->intra_matrix[j]        = v;
        m->chroma_intra_matrix[j] = v;
    }

    return 0;
}

const FFCodec ff_ipu_decoder = {
    .p.name         = "ipu",
    CODEC_LONG_NAME("IPU Video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_IPU,
    .priv_data_size = sizeof(IPUContext),
    .init           = ipu_decode_init,
    FF_CODEC_DECODE_CB(ipu_decode_frame),
    .p.capabilities = AV_CODEC_CAP_DR1,
};
