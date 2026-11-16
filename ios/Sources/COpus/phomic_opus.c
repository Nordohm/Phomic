#include <stddef.h>

#include "phomic_opus.h"

OpusEncoder *phomic_opus_encoder_create(int bitrate, int complexity, int *error)
{
    OpusEncoder *encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, error);
    if (!encoder)
    {
        return NULL;
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(complexity));
    opus_encoder_ctl(encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(encoder, OPUS_SET_VBR_CONSTRAINT(1));
    return encoder;
}

int phomic_opus_set_bitrate(OpusEncoder *encoder, int bitrate)
{
    return opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate));
}
