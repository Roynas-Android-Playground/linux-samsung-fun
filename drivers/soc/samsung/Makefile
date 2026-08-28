# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_EXYNOS_ASV_ARM)	+= exynos5422-asv.o
obj-$(CONFIG_EXYNOS_CHIPID)	+= exynos_chipid.o
exynos_chipid-y			+= exynos-chipid.o exynos-asv.o
obj-$(CONFIG_EXYNOS8890_ECT)	+= exynos8890-ect.o
obj-$(CONFIG_EXYNOS8890_CPUIDLE)	+= exynos8890-cpuidle.o
obj-$(CONFIG_EXYNOS8890_CPUPM)	+= exynos8890-cpupm.o
obj-$(CONFIG_EXYNOS8890_CPCTL)	+= exynos8890-cpctl.o

obj-$(CONFIG_EXYNOS_USI)	+= exynos-usi.o

obj-$(CONFIG_EXYNOS_PMU)	+= exynos_pmu.o
exynos_pmu-y			+= exynos-pmu.o gs101-pmu.o

obj-$(CONFIG_EXYNOS_PMU_ARM_DRIVERS)	+= exynos3250-pmu.o exynos4-pmu.o \
					exynos5250-pmu.o exynos5420-pmu.o
obj-$(CONFIG_EXYNOS_REGULATOR_COUPLER) += exynos-regulator-coupler.o

obj-$(CONFIG_SAMSUNG_PM_CHECK)	+= s3c-pm-check.o
