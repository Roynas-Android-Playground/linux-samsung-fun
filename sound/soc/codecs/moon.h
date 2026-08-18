/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SND_SOC_MOON_H
#define _SND_SOC_MOON_H

#include <sound/pcm.h>

#define MOON_RATES SNDRV_PCM_RATE_KNOT
#define MOON_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE | \
		      SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)

#endif /* _SND_SOC_MOON_H */
