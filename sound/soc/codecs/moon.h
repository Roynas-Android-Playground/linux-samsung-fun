/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ASoC driver for Cirrus Logic CS47L90/CS47L91 (Moon)
 */

#ifndef _SND_SOC_MOON_H
#define _SND_SOC_MOON_H

#include <sound/pcm.h>

#include "arizona.h"

#define MOON_FLL1		1
#define MOON_FLL2		2
#define MOON_FLL1_REFCLK	4
#define MOON_FLL2_REFCLK	5

#define MOON_RATES	 SNDRV_PCM_RATE_KNOT
#define MOON_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE | \
			 SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)

#endif /* _SND_SOC_MOON_H */
