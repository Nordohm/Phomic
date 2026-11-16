#ifndef PHOMIC_OPUS_H
#define PHOMIC_OPUS_H

#include "opus.h"

/* Swift can't call opus_encoder_ctl (it's variadic), so Phomic's encoder settings live here. */

/* 48 kHz mono, low-delay mode (CELT only, 10 ms frames), constrained VBR. */
OpusEncoder *phomic_opus_encoder_create(int bitrate, int complexity, int *error);
int phomic_opus_set_bitrate(OpusEncoder *encoder, int bitrate);

#endif
