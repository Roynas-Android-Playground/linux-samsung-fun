/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CS47L90/CS47L91 (Moon) register definitions.
 *
 * Isolated from the shared Arizona register header; values are derived from
 * the Cirrus/Samsung Moon driver at vendor commit
 * 0460c258d6910628410263dc838a81be8bda6776.
 */
#ifndef _WM_ARIZONA_MOON_REGISTERS_H
#define _WM_ARIZONA_MOON_REGISTERS_H

#define CLEARWATER_COMFORT_NOISE_GENERATOR       0xA0
#define CLEARWATER_BOOT_DONE_STS1                0x0080
#define CLEARWATER_DSP_CLOCK_1                   0x120
#define CLEARWATER_DSP_CLOCK_2                   0x122
#define ARIZONA_FLL1_EFS_2                       0x17A
#define ARIZONA_FLL2_EFS_2                       0x19A
#define MOON_FLLAO_CONTROL_1                     0x1D1
#define MOON_FLLAO_CONTROL_2                     0x1D2
#define MOON_FLLAO_CONTROL_3                     0x1D3
#define MOON_FLLAO_CONTROL_4                     0x1D4
#define MOON_FLLAO_CONTROL_5                     0x1D5
#define MOON_FLLAO_CONTROL_6                     0x1D6
#define MOON_FLLAO_CONTROL_7                     0x1D8
#define MOON_FLLAO_CONTROL_8                     0x1DA
#define MOON_FLLAO_CONTROL_9                     0x1DB
#define MOON_FLLAO_CONTROL_10                    0x1DC
#define MOON_FLLAO_CONTROL_11                    0x1DD
#define ARIZONA_MIC_BIAS_CTRL_5                  0x21C
#define ARIZONA_MIC_BIAS_CTRL_6                  0x21E
#define ARIZONA_HP_CTRL_2L                       0x227
#define ARIZONA_HP_CTRL_2R                       0x228
#define ARIZONA_HP_CTRL_3L                       0x229
#define ARIZONA_HP_CTRL_3R                       0x22A
#define CLEARWATER_EDRE_HP_STEREO_CONTROL        0x27E
#define MOON_HEADPHONE_DETECT_0                  0x299
#define ARIZONA_HEADPHONE_DETECT_3               0x29D
#define MOON_MIC_DETECT_0                        0x2A2
#define MOON_MICDET2_CONTROL_0                   0x2B2
#define MOON_MICDET2_CONTROL_1                   0x2B3
#define MOON_MICDET2_CONTROL_2                   0x2B4
#define MOON_MICDET2_CONTROL_3                   0x2B5
#define MOON_MICDET2_LEVEL_1                     0x2B6
#define MOON_MICDET2_LEVEL_2                     0x2B7
#define MOON_MICDET2_LEVEL_3                     0x2B8
#define MOON_MICDET2_LEVEL_4                     0x2B9
#define MOON_MICDET2_CONTROL_4                   0x2BB
#define CLEARWATER_MICD_CLAMP_CONTROL            0x2C6
#define MOON_IN1L_RATE_CONTROL                   0x313
#define MOON_IN1R_RATE_CONTROL                   0x317
#define MOON_IN2L_RATE_CONTROL                   0x31B
#define MOON_IN2R_RATE_CONTROL                   0x31F
#define MOON_IN3L_RATE_CONTROL                   0x323
#define MOON_IN3R_RATE_CONTROL                   0x327
#define MOON_IN4L_RATE_CONTROL                   0x32B
#define MOON_IN4R_RATE_CONTROL                   0x32F
#define ARIZONA_IN5L_CONTROL                     0x330
#define ARIZONA_ADC_DIGITAL_VOLUME_5L            0x331
#define ARIZONA_DMIC5L_CONTROL                   0x332
#define MOON_IN5L_RATE_CONTROL                   0x333
#define ARIZONA_IN5R_CONTROL                     0x334
#define ARIZONA_ADC_DIGITAL_VOLUME_5R            0x335
#define ARIZONA_DMIC5R_CONTROL                   0x336
#define MOON_IN5R_RATE_CONTROL                   0x337
#define MOON_OUT1_CONFIG                         0x412
#define MOON_OUT2_CONFIG                         0x41A
#define CLEARWATER_EDRE_ENABLE                   0x448
#define ARIZONA_HP_TEST_CTRL_5                   0x4A8
#define ARIZONA_HP_TEST_CTRL_6                   0x4A9
#define ARIZONA_AIF2_FRAME_CTRL_9                0x54F
#define ARIZONA_AIF2_FRAME_CTRL_10               0x550
#define ARIZONA_AIF2_FRAME_CTRL_17               0x557
#define ARIZONA_AIF2_FRAME_CTRL_18               0x558
#define ARIZONA_AIF4_BCLK_CTRL                   0x5A0
#define ARIZONA_AIF4_TX_PIN_CTRL                 0x5A1
#define ARIZONA_AIF4_RX_PIN_CTRL                 0x5A2
#define ARIZONA_AIF4_RATE_CTRL                   0x5A3
#define ARIZONA_AIF4_FORMAT                      0x5A4
#define ARIZONA_AIF4_TX_BCLK_RATE                0x5A5
#define ARIZONA_AIF4_RX_BCLK_RATE                0x5A6
#define ARIZONA_AIF4_FRAME_CTRL_1                0x5A7
#define ARIZONA_AIF4_FRAME_CTRL_2                0x5A8
#define ARIZONA_AIF4_FRAME_CTRL_3                0x5A9
#define ARIZONA_AIF4_FRAME_CTRL_4                0x5AA
#define ARIZONA_AIF4_FRAME_CTRL_11               0x5B1
#define ARIZONA_AIF4_FRAME_CTRL_12               0x5B2
#define ARIZONA_AIF4_TX_ENABLES                  0x5B9
#define ARIZONA_AIF4_RX_ENABLES                  0x5BA
#define ARIZONA_AIF2TX7MIX_INPUT_1_SOURCE        0x770
#define ARIZONA_AIF2TX7MIX_INPUT_1_VOLUME        0x771
#define ARIZONA_AIF2TX7MIX_INPUT_2_SOURCE        0x772
#define ARIZONA_AIF2TX7MIX_INPUT_2_VOLUME        0x773
#define ARIZONA_AIF2TX7MIX_INPUT_3_SOURCE        0x774
#define ARIZONA_AIF2TX7MIX_INPUT_3_VOLUME        0x775
#define ARIZONA_AIF2TX7MIX_INPUT_4_SOURCE        0x776
#define ARIZONA_AIF2TX7MIX_INPUT_4_VOLUME        0x777
#define ARIZONA_AIF2TX8MIX_INPUT_1_SOURCE        0x778
#define ARIZONA_AIF2TX8MIX_INPUT_1_VOLUME        0x779
#define ARIZONA_AIF2TX8MIX_INPUT_2_SOURCE        0x77A
#define ARIZONA_AIF2TX8MIX_INPUT_2_VOLUME        0x77B
#define ARIZONA_AIF2TX8MIX_INPUT_3_SOURCE        0x77C
#define ARIZONA_AIF2TX8MIX_INPUT_3_VOLUME        0x77D
#define ARIZONA_AIF2TX8MIX_INPUT_4_SOURCE        0x77E
#define ARIZONA_AIF2TX8MIX_INPUT_4_VOLUME        0x77F
#define ARIZONA_AIF4TX1MIX_INPUT_1_SOURCE        0x7A0
#define ARIZONA_AIF4TX1MIX_INPUT_1_VOLUME        0x7A1
#define ARIZONA_AIF4TX1MIX_INPUT_2_SOURCE        0x7A2
#define ARIZONA_AIF4TX1MIX_INPUT_2_VOLUME        0x7A3
#define ARIZONA_AIF4TX1MIX_INPUT_3_SOURCE        0x7A4
#define ARIZONA_AIF4TX1MIX_INPUT_3_VOLUME        0x7A5
#define ARIZONA_AIF4TX1MIX_INPUT_4_SOURCE        0x7A6
#define ARIZONA_AIF4TX1MIX_INPUT_4_VOLUME        0x7A7
#define ARIZONA_AIF4TX2MIX_INPUT_1_SOURCE        0x7A8
#define ARIZONA_AIF4TX2MIX_INPUT_1_VOLUME        0x7A9
#define ARIZONA_AIF4TX2MIX_INPUT_2_SOURCE        0x7AA
#define ARIZONA_AIF4TX2MIX_INPUT_2_VOLUME        0x7AB
#define ARIZONA_AIF4TX2MIX_INPUT_3_SOURCE        0x7AC
#define ARIZONA_AIF4TX2MIX_INPUT_3_VOLUME        0x7AD
#define ARIZONA_AIF4TX2MIX_INPUT_4_SOURCE        0x7AE
#define ARIZONA_AIF4TX2MIX_INPUT_4_VOLUME        0x7AF
#define CLEARWATER_DSP5LMIX_INPUT_1_SOURCE       0xA40
#define CLEARWATER_DSP5LMIX_INPUT_1_VOLUME       0xA41
#define CLEARWATER_DSP5LMIX_INPUT_2_SOURCE       0xA42
#define CLEARWATER_DSP5LMIX_INPUT_2_VOLUME       0xA43
#define CLEARWATER_DSP5LMIX_INPUT_3_SOURCE       0xA44
#define CLEARWATER_DSP5LMIX_INPUT_3_VOLUME       0xA45
#define CLEARWATER_DSP5LMIX_INPUT_4_SOURCE       0xA46
#define CLEARWATER_DSP5LMIX_INPUT_4_VOLUME       0xA47
#define CLEARWATER_DSP5RMIX_INPUT_1_SOURCE       0xA48
#define CLEARWATER_DSP5RMIX_INPUT_1_VOLUME       0xA49
#define CLEARWATER_DSP5RMIX_INPUT_2_SOURCE       0xA4A
#define CLEARWATER_DSP5RMIX_INPUT_2_VOLUME       0xA4B
#define CLEARWATER_DSP5RMIX_INPUT_3_SOURCE       0xA4C
#define CLEARWATER_DSP5RMIX_INPUT_3_VOLUME       0xA4D
#define CLEARWATER_DSP5RMIX_INPUT_4_SOURCE       0xA4E
#define CLEARWATER_DSP5RMIX_INPUT_4_VOLUME       0xA4F
#define CLEARWATER_DSP5AUX1MIX_INPUT_1_SOURCE    0xA50
#define CLEARWATER_DSP5AUX2MIX_INPUT_1_SOURCE    0xA58
#define CLEARWATER_DSP5AUX3MIX_INPUT_1_SOURCE    0xA60
#define CLEARWATER_DSP5AUX4MIX_INPUT_1_SOURCE    0xA68
#define CLEARWATER_DSP5AUX5MIX_INPUT_1_SOURCE    0xA70
#define CLEARWATER_DSP5AUX6MIX_INPUT_1_SOURCE    0xA78
#define CLEARWATER_ASRC1_1LMIX_INPUT_1_SOURCE    0xA80
#define CLEARWATER_ASRC1_1RMIX_INPUT_1_SOURCE    0xA88
#define CLEARWATER_ASRC1_2LMIX_INPUT_1_SOURCE    0xA90
#define CLEARWATER_ASRC1_2RMIX_INPUT_1_SOURCE    0xA98
#define CLEARWATER_ASRC2_1LMIX_INPUT_1_SOURCE    0xAA0
#define CLEARWATER_ASRC2_1RMIX_INPUT_1_SOURCE    0xAA8
#define CLEARWATER_ASRC2_2LMIX_INPUT_1_SOURCE    0xAB0
#define CLEARWATER_ASRC2_2RMIX_INPUT_1_SOURCE    0xAB8
#define ARIZONA_ISRC4DEC1MIX_INPUT_1_SOURCE      0xBC0
#define ARIZONA_ISRC4DEC2MIX_INPUT_1_SOURCE      0xBC8
#define ARIZONA_ISRC4INT1MIX_INPUT_1_SOURCE      0xBE0
#define ARIZONA_ISRC4INT2MIX_INPUT_1_SOURCE      0xBE8
#define CLEARWATER_DSP6LMIX_INPUT_1_SOURCE       0xC00
#define CLEARWATER_DSP6LMIX_INPUT_1_VOLUME       0xC01
#define CLEARWATER_DSP6LMIX_INPUT_2_SOURCE       0xC02
#define CLEARWATER_DSP6LMIX_INPUT_2_VOLUME       0xC03
#define CLEARWATER_DSP6LMIX_INPUT_3_SOURCE       0xC04
#define CLEARWATER_DSP6LMIX_INPUT_3_VOLUME       0xC05
#define CLEARWATER_DSP6LMIX_INPUT_4_SOURCE       0xC06
#define CLEARWATER_DSP6LMIX_INPUT_4_VOLUME       0xC07
#define CLEARWATER_DSP6RMIX_INPUT_1_SOURCE       0xC08
#define CLEARWATER_DSP6RMIX_INPUT_1_VOLUME       0xC09
#define CLEARWATER_DSP6RMIX_INPUT_2_SOURCE       0xC0A
#define CLEARWATER_DSP6RMIX_INPUT_2_VOLUME       0xC0B
#define CLEARWATER_DSP6RMIX_INPUT_3_SOURCE       0xC0C
#define CLEARWATER_DSP6RMIX_INPUT_3_VOLUME       0xC0D
#define CLEARWATER_DSP6RMIX_INPUT_4_SOURCE       0xC0E
#define CLEARWATER_DSP6RMIX_INPUT_4_VOLUME       0xC0F
#define CLEARWATER_DSP6AUX1MIX_INPUT_1_SOURCE    0xC10
#define CLEARWATER_DSP6AUX2MIX_INPUT_1_SOURCE    0xC18
#define CLEARWATER_DSP6AUX3MIX_INPUT_1_SOURCE    0xC20
#define CLEARWATER_DSP6AUX4MIX_INPUT_1_SOURCE    0xC28
#define CLEARWATER_DSP6AUX5MIX_INPUT_1_SOURCE    0xC30
#define CLEARWATER_DSP6AUX6MIX_INPUT_1_SOURCE    0xC38
#define CLEARWATER_DSP7LMIX_INPUT_1_SOURCE       0xC40
#define CLEARWATER_DSP7LMIX_INPUT_1_VOLUME       0xC41
#define CLEARWATER_DSP7LMIX_INPUT_2_SOURCE       0xC42
#define CLEARWATER_DSP7LMIX_INPUT_2_VOLUME       0xC43
#define CLEARWATER_DSP7LMIX_INPUT_3_SOURCE       0xC44
#define CLEARWATER_DSP7LMIX_INPUT_3_VOLUME       0xC45
#define CLEARWATER_DSP7LMIX_INPUT_4_SOURCE       0xC46
#define CLEARWATER_DSP7LMIX_INPUT_4_VOLUME       0xC47
#define CLEARWATER_DSP7RMIX_INPUT_1_SOURCE       0xC48
#define CLEARWATER_DSP7RMIX_INPUT_1_VOLUME       0xC49
#define CLEARWATER_DSP7RMIX_INPUT_2_SOURCE       0xC4A
#define CLEARWATER_DSP7RMIX_INPUT_2_VOLUME       0xC4B
#define CLEARWATER_DSP7RMIX_INPUT_3_SOURCE       0xC4C
#define CLEARWATER_DSP7RMIX_INPUT_3_VOLUME       0xC4D
#define CLEARWATER_DSP7RMIX_INPUT_4_SOURCE       0xC4E
#define CLEARWATER_DSP7RMIX_INPUT_4_VOLUME       0xC4F
#define CLEARWATER_DSP7AUX1MIX_INPUT_1_SOURCE    0xC50
#define CLEARWATER_DSP7AUX2MIX_INPUT_1_SOURCE    0xC58
#define CLEARWATER_DSP7AUX3MIX_INPUT_1_SOURCE    0xC60
#define CLEARWATER_DSP7AUX4MIX_INPUT_1_SOURCE    0xC68
#define CLEARWATER_DSP7AUX5MIX_INPUT_1_SOURCE    0xC70
#define CLEARWATER_DSP7AUX6MIX_INPUT_1_SOURCE    0xC78
#define CLEARWATER_GP_SWITCH_1                   0x2C8
#define MOON_DFC1MIX_INPUT_1_SOURCE              0xDC0
#define MOON_DFC2MIX_INPUT_1_SOURCE              0xDC8
#define MOON_DFC3MIX_INPUT_1_SOURCE              0xDD0
#define MOON_DFC4MIX_INPUT_1_SOURCE              0xDD8
#define MOON_DFC5MIX_INPUT_1_SOURCE              0xDE0
#define MOON_DFC6MIX_INPUT_1_SOURCE              0xDE8
#define MOON_DFC7MIX_INPUT_1_SOURCE              0xDF0
#define MOON_DFC8MIX_INPUT_1_SOURCE              0xDF8
#define CLEARWATER_DRC2_CTRL1                    0xE88
#define CLEARWATER_DRC2_CTRL2                    0xE89
#define CLEARWATER_DRC2_CTRL3                    0xE8A
#define CLEARWATER_DRC2_CTRL4                    0xE8B
#define CLEARWATER_DRC2_CTRL5                    0xE8C
#define CLEARWATER_ASRC2_ENABLE                  0xED0
#define CLEARWATER_ASRC2_STATUS                  0xED1
#define CLEARWATER_ASRC2_RATE1                   0xED2
#define CLEARWATER_ASRC2_RATE2                   0xED3
#define CLEARWATER_ASRC1_ENABLE                  0xEE0
#define CLEARWATER_ASRC1_STATUS                  0xEE1
#define CLEARWATER_ASRC1_RATE1                   0xEE2
#define CLEARWATER_ASRC1_RATE2                   0xEE3
#define ARIZONA_ISRC_4_CTRL_1                    0xEF9
#define ARIZONA_ISRC_4_CTRL_2                    0xEFA
#define ARIZONA_ISRC_4_CTRL_3                    0xEFB
#define CLEARWATER_FCR_FILTER_CONTROL            0xF71
#define CLEARWATER_FCR_ADC_REFORMATTER_CONTROL   0xF73
#define CLEARWATER_FCR_COEFF_START               0xF74
#define CLEARWATER_FCR_COEFF_END                 0xFC5
#define CLEARWATER_DAC_COMP_1                    0x1300
#define CLEARWATER_DAC_COMP_2                    0x1302
#define CLEARWATER_FRF_COEFFICIENT_1L_1          0x1380
#define CLEARWATER_FRF_COEFFICIENT_1L_2          0x1381
#define CLEARWATER_FRF_COEFFICIENT_1L_3          0x1382
#define CLEARWATER_FRF_COEFFICIENT_1L_4          0x1383
#define CLEARWATER_FRF_COEFFICIENT_1R_1          0x1390
#define CLEARWATER_FRF_COEFFICIENT_1R_2          0x1391
#define CLEARWATER_FRF_COEFFICIENT_1R_3          0x1392
#define CLEARWATER_FRF_COEFFICIENT_1R_4          0x1393
#define CLEARWATER_FRF_COEFFICIENT_2L_1          0x13A0
#define CLEARWATER_FRF_COEFFICIENT_2L_2          0x13A1
#define CLEARWATER_FRF_COEFFICIENT_2L_3          0x13A2
#define CLEARWATER_FRF_COEFFICIENT_2L_4          0x13A3
#define CLEARWATER_FRF_COEFFICIENT_2R_1          0x13B0
#define CLEARWATER_FRF_COEFFICIENT_2R_2          0x13B1
#define CLEARWATER_FRF_COEFFICIENT_2R_3          0x13B2
#define CLEARWATER_FRF_COEFFICIENT_2R_4          0x13B3
#define CLEARWATER_FRF_COEFFICIENT_3L_1          0x13C0
#define CLEARWATER_FRF_COEFFICIENT_3L_2          0x13C1
#define CLEARWATER_FRF_COEFFICIENT_3L_3          0x13C2
#define CLEARWATER_FRF_COEFFICIENT_3L_4          0x13C3
#define CLEARWATER_FRF_COEFFICIENT_3R_1          0x13D0
#define CLEARWATER_FRF_COEFFICIENT_3R_2          0x13D1
#define CLEARWATER_FRF_COEFFICIENT_3R_3          0x13D2
#define CLEARWATER_FRF_COEFFICIENT_3R_4          0x13D3
#define CLEARWATER_FRF_COEFFICIENT_5L_1          0x1400
#define CLEARWATER_FRF_COEFFICIENT_5L_2          0x1401
#define CLEARWATER_FRF_COEFFICIENT_5L_3          0x1402
#define CLEARWATER_FRF_COEFFICIENT_5L_4          0x1403
#define CLEARWATER_FRF_COEFFICIENT_5R_1          0x1410
#define CLEARWATER_FRF_COEFFICIENT_5R_2          0x1411
#define CLEARWATER_FRF_COEFFICIENT_5R_3          0x1412
#define CLEARWATER_FRF_COEFFICIENT_5R_4          0x1413
#define MOON_DFC1_CTRL                           0x1480
#define MOON_DFC1_RX                             0x1482
#define MOON_DFC1_TX                             0x1484
#define MOON_DFC2_CTRL                           0x1486
#define MOON_DFC2_RX                             0x1488
#define MOON_DFC2_TX                             0x148A
#define MOON_DFC3_CTRL                           0x148C
#define MOON_DFC3_RX                             0x148E
#define MOON_DFC3_TX                             0x1490
#define MOON_DFC4_CTRL                           0x1492
#define MOON_DFC4_RX                             0x1494
#define MOON_DFC4_TX                             0x1496
#define MOON_DFC5_CTRL                           0x1498
#define MOON_DFC5_RX                             0x149A
#define MOON_DFC5_TX                             0x149C
#define MOON_DFC6_CTRL                           0x149E
#define MOON_DFC6_RX                             0x14A0
#define MOON_DFC6_TX                             0x14A2
#define MOON_DFC7_CTRL                           0x14A4
#define MOON_DFC7_RX                             0x14A6
#define MOON_DFC7_TX                             0x14A8
#define MOON_DFC8_CTRL                           0x14AA
#define MOON_DFC8_RX                             0x14AC
#define MOON_DFC8_TX                             0x14AE
#define MOON_DFC_STATUS                          0x14B6
#define CLEARWATER_GPIO1_CTRL_1                  0x1700
#define CLEARWATER_GPIO1_CTRL_2                  0x1701
#define CLEARWATER_GPIO2_CTRL_1                  0x1702
#define CLEARWATER_GPIO2_CTRL_2                  0x1703
#define CLEARWATER_GPIO3_CTRL_1                  0x1704
#define CLEARWATER_GPIO3_CTRL_2                  0x1705
#define CLEARWATER_GPIO4_CTRL_1                  0x1706
#define CLEARWATER_GPIO4_CTRL_2                  0x1707
#define CLEARWATER_GPIO5_CTRL_1                  0x1708
#define CLEARWATER_GPIO5_CTRL_2                  0x1709
#define CLEARWATER_GPIO6_CTRL_1                  0x170A
#define CLEARWATER_GPIO6_CTRL_2                  0x170B
#define CLEARWATER_GPIO7_CTRL_1                  0x170C
#define CLEARWATER_GPIO7_CTRL_2                  0x170D
#define CLEARWATER_GPIO8_CTRL_1                  0x170E
#define CLEARWATER_GPIO8_CTRL_2                  0x170F
#define CLEARWATER_GPIO9_CTRL_1                  0x1710
#define CLEARWATER_GPIO9_CTRL_2                  0x1711
#define CLEARWATER_GPIO10_CTRL_1                 0x1712
#define CLEARWATER_GPIO10_CTRL_2                 0x1713
#define CLEARWATER_GPIO11_CTRL_1                 0x1714
#define CLEARWATER_GPIO11_CTRL_2                 0x1715
#define CLEARWATER_GPIO12_CTRL_1                 0x1716
#define CLEARWATER_GPIO12_CTRL_2                 0x1717
#define CLEARWATER_GPIO13_CTRL_1                 0x1718
#define CLEARWATER_GPIO13_CTRL_2                 0x1719
#define CLEARWATER_GPIO14_CTRL_1                 0x171A
#define CLEARWATER_GPIO14_CTRL_2                 0x171B
#define CLEARWATER_GPIO15_CTRL_1                 0x171C
#define CLEARWATER_GPIO15_CTRL_2                 0x171D
#define CLEARWATER_GPIO16_CTRL_1                 0x171E
#define CLEARWATER_GPIO16_CTRL_2                 0x171F
#define CLEARWATER_GPIO17_CTRL_1                 0x1720
#define CLEARWATER_GPIO17_CTRL_2                 0x1721
#define CLEARWATER_GPIO18_CTRL_1                 0x1722
#define CLEARWATER_GPIO18_CTRL_2                 0x1723
#define CLEARWATER_GPIO19_CTRL_1                 0x1724
#define CLEARWATER_GPIO19_CTRL_2                 0x1725
#define CLEARWATER_GPIO20_CTRL_1                 0x1726
#define CLEARWATER_GPIO20_CTRL_2                 0x1727
#define CLEARWATER_GPIO21_CTRL_1                 0x1728
#define CLEARWATER_GPIO21_CTRL_2                 0x1729
#define CLEARWATER_GPIO22_CTRL_1                 0x172A
#define CLEARWATER_GPIO22_CTRL_2                 0x172B
#define CLEARWATER_GPIO23_CTRL_1                 0x172C
#define CLEARWATER_GPIO23_CTRL_2                 0x172D
#define CLEARWATER_GPIO24_CTRL_1                 0x172E
#define CLEARWATER_GPIO24_CTRL_2                 0x172F
#define CLEARWATER_GPIO25_CTRL_1                 0x1730
#define CLEARWATER_GPIO25_CTRL_2                 0x1731
#define CLEARWATER_GPIO26_CTRL_1                 0x1732
#define CLEARWATER_GPIO26_CTRL_2                 0x1733
#define CLEARWATER_GPIO27_CTRL_1                 0x1734
#define CLEARWATER_GPIO27_CTRL_2                 0x1735
#define CLEARWATER_GPIO28_CTRL_1                 0x1736
#define CLEARWATER_GPIO28_CTRL_2                 0x1737
#define CLEARWATER_GPIO29_CTRL_1                 0x1738
#define CLEARWATER_GPIO29_CTRL_2                 0x1739
#define CLEARWATER_GPIO30_CTRL_1                 0x173A
#define CLEARWATER_GPIO30_CTRL_2                 0x173B
#define CLEARWATER_GPIO31_CTRL_1                 0x173C
#define CLEARWATER_GPIO31_CTRL_2                 0x173D
#define CLEARWATER_GPIO32_CTRL_1                 0x173E
#define CLEARWATER_GPIO32_CTRL_2                 0x173F
#define CLEARWATER_GPIO33_CTRL_1                 0x1740
#define CLEARWATER_GPIO33_CTRL_2                 0x1741
#define CLEARWATER_GPIO34_CTRL_1                 0x1742
#define CLEARWATER_GPIO34_CTRL_2                 0x1743
#define CLEARWATER_GPIO35_CTRL_1                 0x1744
#define CLEARWATER_GPIO35_CTRL_2                 0x1745
#define CLEARWATER_GPIO36_CTRL_1                 0x1746
#define CLEARWATER_GPIO36_CTRL_2                 0x1747
#define CLEARWATER_GPIO37_CTRL_1                 0x1748
#define CLEARWATER_GPIO37_CTRL_2                 0x1749
#define CLEARWATER_GPIO38_CTRL_1                 0x174A
#define CLEARWATER_GPIO38_CTRL_2                 0x174B
#define CLEARWATER_IRQ1_STATUS_1                 0x1800
#define CLEARWATER_IRQ1_STATUS_2                 0x1801
#define CLEARWATER_IRQ1_STATUS_3                 0x1802
#define CLEARWATER_IRQ1_STATUS_4                 0x1803
#define CLEARWATER_IRQ1_STATUS_5                 0x1804
#define CLEARWATER_IRQ1_STATUS_6                 0x1805
#define CLEARWATER_IRQ1_STATUS_7                 0x1806
#define CLEARWATER_IRQ1_STATUS_8                 0x1807
#define CLEARWATER_IRQ1_STATUS_9                 0x1808
#define CLEARWATER_IRQ1_STATUS_10                0x1809
#define CLEARWATER_IRQ1_STATUS_11                0x180A
#define CLEARWATER_IRQ1_STATUS_12                0x180B
#define CLEARWATER_IRQ1_STATUS_13                0x180C
#define CLEARWATER_IRQ1_STATUS_14                0x180D
#define CLEARWATER_IRQ1_STATUS_15                0x180E
#define CLEARWATER_IRQ1_STATUS_16                0x180F
#define CLEARWATER_IRQ1_STATUS_17                0x1810
#define CLEARWATER_IRQ1_STATUS_18                0x1811
#define CLEARWATER_IRQ1_STATUS_19                0x1812
#define CLEARWATER_IRQ1_STATUS_20                0x1813
#define CLEARWATER_IRQ1_STATUS_21                0x1814
#define CLEARWATER_IRQ1_STATUS_22                0x1815
#define CLEARWATER_IRQ1_STATUS_23                0x1816
#define CLEARWATER_IRQ1_STATUS_24                0x1817
#define CLEARWATER_IRQ1_STATUS_25                0x1818
#define CLEARWATER_IRQ1_STATUS_26                0x1819
#define CLEARWATER_IRQ1_STATUS_27                0x181A
#define CLEARWATER_IRQ1_STATUS_28                0x181B
#define CLEARWATER_IRQ1_STATUS_29                0x181C
#define CLEARWATER_IRQ1_STATUS_30                0x181D
#define CLEARWATER_IRQ1_STATUS_31                0x181E
#define CLEARWATER_IRQ1_STATUS_32                0x181F
#define MOON_IRQ1_STATUS_33                      0x1820
#define CLEARWATER_IRQ1_MASK_1                   0x1840
#define CLEARWATER_IRQ1_MASK_2                   0x1841
#define CLEARWATER_IRQ1_MASK_3                   0x1842
#define CLEARWATER_IRQ1_MASK_4                   0x1843
#define CLEARWATER_IRQ1_MASK_5                   0x1844
#define CLEARWATER_IRQ1_MASK_6                   0x1845
#define CLEARWATER_IRQ1_MASK_7                   0x1846
#define CLEARWATER_IRQ1_MASK_8                   0x1847
#define CLEARWATER_IRQ1_MASK_9                   0x1848
#define CLEARWATER_IRQ1_MASK_10                  0x1849
#define CLEARWATER_IRQ1_MASK_11                  0x184A
#define CLEARWATER_IRQ1_MASK_12                  0x184B
#define CLEARWATER_IRQ1_MASK_13                  0x184C
#define CLEARWATER_IRQ1_MASK_14                  0x184D
#define CLEARWATER_IRQ1_MASK_15                  0x184E
#define MOON_IRQ1_MASK_16                        0x184F
#define CLEARWATER_IRQ1_MASK_17                  0x1850
#define CLEARWATER_IRQ1_MASK_18                  0x1851
#define CLEARWATER_IRQ1_MASK_19                  0x1852
#define MOON_IRQ1_MASK_20                        0x1853
#define CLEARWATER_IRQ1_MASK_21                  0x1854
#define CLEARWATER_IRQ1_MASK_22                  0x1855
#define CLEARWATER_IRQ1_MASK_23                  0x1856
#define CLEARWATER_IRQ1_MASK_24                  0x1857
#define CLEARWATER_IRQ1_MASK_25                  0x1858
#define MOON_IRQ1_MASK_26                        0x1859
#define CLEARWATER_IRQ1_MASK_27                  0x185A
#define CLEARWATER_IRQ1_MASK_28                  0x185B
#define MOON_IRQ1_MASK_29                        0x185C
#define CLEARWATER_IRQ1_MASK_30                  0x185D
#define CLEARWATER_IRQ1_MASK_31                  0x185E
#define CLEARWATER_IRQ1_MASK_32                  0x185F
#define MOON_IRQ1_MASK_33                        0x1860
#define CLEARWATER_IRQ1_RAW_STATUS_1             0x1880
#define CLEARWATER_IRQ1_RAW_STATUS_2             0x1881
#define CLEARWATER_IRQ1_RAW_STATUS_7             0x1886
#define CLEARWATER_IRQ1_RAW_STATUS_9             0x1888
#define CLEARWATER_IRQ1_RAW_STATUS_11            0x188A
#define CLEARWATER_IRQ1_RAW_STATUS_12            0x188B
#define CLEARWATER_IRQ1_RAW_STATUS_13            0x188C
#define CLEARWATER_IRQ1_RAW_STATUS_14            0x188D
#define CLEARWATER_IRQ1_RAW_STATUS_15            0x188E
#define CLEARWATER_IRQ1_RAW_STATUS_17            0x1890
#define CLEARWATER_IRQ1_RAW_STATUS_18            0x1891
#define CLEARWATER_IRQ1_RAW_STATUS_19            0x1892
#define CLEARWATER_IRQ1_RAW_STATUS_21            0x1894
#define CLEARWATER_IRQ1_RAW_STATUS_22            0x1895
#define CLEARWATER_IRQ1_RAW_STATUS_23            0x1896
#define CLEARWATER_IRQ1_RAW_STATUS_24            0x1897
#define CLEARWATER_IRQ1_RAW_STATUS_25            0x1898
#define CLEARWATER_IRQ1_RAW_STATUS_30            0x189D
#define CLEARWATER_IRQ1_RAW_STATUS_31            0x189E
#define CLEARWATER_IRQ1_RAW_STATUS_32            0x189F
#define CLEARWATER_IRQ2_STATUS_9                 0x1908
#define CLEARWATER_IRQ2_MASK_9                   0x1948
#define CLEARWATER_IRQ2_RAW_STATUS_9             0x1988
#define CLEARWATER_INTERRUPT_DEBOUNCE_7          0x1A06
#define CLEARWATER_IRQ1_CTRL                     0x1A80
#define CLEARWATER_INTERRUPT_RAW_STATUS_1        0x1AA0
#define ARIZONA_WSEQ_SEQUENCE_1                  0x3000
#define ARIZONA_WSEQ_SEQUENCE_508                0x33F6
#define MOON_OTP_HPDET_CALIB_1                   0x020004
#define MOON_OTP_HPDET_CALIB_2                   0x020006
#define CLEARWATER_DSP1_CONFIG                   0x0FFE00
#define MOON_DSP1_PMEM_ERR_ADDR_XMEM_ERR_ADDR    0x0FFE7C
#define CLEARWATER_DSP2_CONFIG                   0x17FE00
#define MOON_DSP2_PMEM_ERR_ADDR_XMEM_ERR_ADDR    0x17FE7C
#define CLEARWATER_DSP3_CONFIG                   0x1FFE00
#define MOON_DSP3_PMEM_ERR_ADDR_XMEM_ERR_ADDR    0x1FFE7C
#define CLEARWATER_DSP4_CONFIG                   0x27FE00
#define MOON_DSP4_PMEM_ERR_ADDR_XMEM_ERR_ADDR    0x27FE7C
#define CLEARWATER_DSP5_CONFIG                   0x2FFE00
#define MOON_DSP5_PMEM_ERR_ADDR_XMEM_ERR_ADDR    0x2FFE7C
#define CLEARWATER_DSP6_CONFIG                   0x37FE00
#define MOON_DSP6_PMEM_ERR_ADDR_XMEM_ERR_ADDR    0x37FE7C
#define CLEARWATER_DSP7_CONFIG                   0x3FFE00
#define MOON_DSP7_PMEM_ERR_ADDR_XMEM_ERR_ADDR    0x3FFE7C
#define CLEARWATER_CTRLIF_ERR_EINT1              0x1000  /* CTRLIF_ERR_EINT1 */
#define CLEARWATER_BOOT_DONE_EINT1               0x0080  /* BOOT_DONE_EINT1 */
#define MOON_FLLAO_LOCK_EINT1                    0x0800  /* FLLAO_LOCK_EINT1 */
#define CLEARWATER_FLL2_LOCK_EINT1               0x0200  /* FLL2_LOCK_EINT1 */
#define CLEARWATER_FLL1_LOCK_EINT1               0x0100  /* FLL1_LOCK_EINT1 */
#define CLEARWATER_MICDET_EINT1                  0x0100  /* MICDET_EINT1 */
#define MOON_MICDET2_EINT1                       0x0200  /* MICDET2_EINT1 */
#define CLEARWATER_HPDET_EINT1                   0x0001  /* HPDET_EINT1 */
#define CLEARWATER_MICD_CLAMP_FALL_EINT1         0x0020  /* MICD_CLAMP_FALL_EINT1 */
#define CLEARWATER_MICD_CLAMP_RISE_EINT1         0x0010  /* MICD_CLAMP_RISE_EINT1 */
#define CLEARWATER_JD1_FALL_EINT1                0x0002  /* JD1_FALL_EINT1 */
#define CLEARWATER_JD1_RISE_EINT1                0x0001  /* JD1_RISE_EINT1 */
#define CLEARWATER_ASRC2_IN1_LOCK_EINT1          0x0400  /* ASRC2_IN1_LOCK_EINT1 */
#define CLEARWATER_ASRC1_IN1_LOCK_EINT1          0x0100  /* ASRC1_IN1_LOCK_EINT1 */
#define CLEARWATER_DRC2_SIG_DET_EINT1            0x0002  /* DRC2_SIG_DET_EINT1 */
#define CLEARWATER_DRC1_SIG_DET_EINT1            0x0001  /* DRC1_SIG_DET_EINT1 */
#define CLEARWATER_DSP_IRQ8_EINT1                0x0080  /* DSP_IRQ8_EINT1 */
#define CLEARWATER_DSP_IRQ7_EINT1                0x0040  /* DSP_IRQ7_EINT1 */
#define CLEARWATER_DSP_IRQ6_EINT1                0x0020  /* DSP_IRQ6_EINT1 */
#define CLEARWATER_DSP_IRQ5_EINT1                0x0010  /* DSP_IRQ5_EINT1 */
#define CLEARWATER_DSP_IRQ4_EINT1                0x0008  /* DSP_IRQ4_EINT1 */
#define CLEARWATER_DSP_IRQ3_EINT1                0x0004  /* DSP_IRQ3_EINT1 */
#define CLEARWATER_DSP_IRQ2_EINT1                0x0002  /* DSP_IRQ2_EINT1 */
#define CLEARWATER_DSP_IRQ1_EINT1                0x0001  /* DSP_IRQ1_EINT1 */
#define CLEARWATER_GP1_EINT1			       0x0001  /* GP1_EINT1 */
#define CLEARWATER_GP2_EINT1			       0x0002  /* GP2_EINT1 */
#define CLEARWATER_GP3_EINT1			       0x0004  /* GP3_EINT1 */
#define CLEARWATER_GP4_EINT1			       0x0008  /* GP4_EINT1 */
#define CLEARWATER_GP5_EINT1			       0x0010  /* GP5_EINT1 */
#define CLEARWATER_GP6_EINT1			       0x0020  /* GP6_EINT1 */
#define CLEARWATER_GP7_EINT1			       0x0040  /* GP7_EINT1 */
#define CLEARWATER_GP8_EINT1			       0x0080  /* GP8_EINT1 */
#define MOON_ADSP_ERROR_STATUS_DSP7                    0x0040  /* IRQ_DSP7_BUS_ERR_EINT1 */
#define MOON_ADSP_ERROR_STATUS_DSP6                    0x0020  /* IRQ_DSP6_BUS_ERR_EINT1 */
#define MOON_ADSP_ERROR_STATUS_DSP5                    0x0010  /* IRQ_DSP5_BUS_ERR_EINT1 */
#define MOON_ADSP_ERROR_STATUS_DSP4                    0x0008  /* IRQ_DSP4_BUS_ERR_EINT1 */
#define MOON_ADSP_ERROR_STATUS_DSP3                    0x0004  /* IRQ_DSP3_BUS_ERR_EINT1 */
#define MOON_ADSP_ERROR_STATUS_DSP2                    0x0002  /* IRQ_DSP2_BUS_ERR_EINT1 */
#define MOON_ADSP_ERROR_STATUS_DSP1                    0x0001  /* IRQ_DSP1_BUS_ERR_EINT1 */

#endif /* _WM_ARIZONA_MOON_REGISTERS_H */
