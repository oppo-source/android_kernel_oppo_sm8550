/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef __SOC_QCOM_SOCINFO_H__
#define __SOC_QCOM_SOCINFO_H__

#include <linux/types.h>

enum feature_code {
	/* External feature code */
	SOCINFO_FC_UNKNOWN = 0x0,
	SOCINFO_FC_AA,
	SOCINFO_FC_AB,
	SOCINFO_FC_AC,
	SOCINFO_FC_AD,
	SOCINFO_FC_AE,
	SOCINFO_FC_AF,
	SOCINFO_FC_AG,
	SOCINFO_FC_AH,
	SOCINFO_FC_EXT_RESERVE,

	/* Internal feature code */
	SOCINFO_FC_Y0 = 0xf1,
	SOCINFO_FC_Y1,
	SOCINFO_FC_Y2,
	SOCINFO_FC_Y3,
	SOCINFO_FC_Y4,
	SOCINFO_FC_Y5,
	SOCINFO_FC_Y6,
	SOCINFO_FC_Y7,
	SOCINFO_FC_Y8,
	SOCINFO_FC_Y9,
	SOCINFO_FC_YA,
	SOCINFO_FC_YB,
	SOCINFO_FC_YC,
	SOCINFO_FC_YD,
	SOCINFO_FC_YE,
	SOCINFO_FC_YF,
	SOCINFO_FC_INT_RESERVE
};

enum pcode {
	SOCINFO_PCODE_UNKNOWN = 0,
	SOCINFO_PCODE_0,
	SOCINFO_PCODE_1,
	SOCINFO_PCODE_2,
	SOCINFO_PCODE_3,
	SOCINFO_PCODE_4,
	SOCINFO_PCODE_5,
	SOCINFO_PCODE_6,
	SOCINFO_PCODE_7,
	SOCINFO_PCODE_8,
	SOCINFO_PCODE_RESERVE = 0x7fffffff
};

enum socinfo_parttype {
	SOCINFO_PART_GPU = 1,
	SOCINFO_PART_VIDEO,
	SOCINFO_PART_CAMERA,
	SOCINFO_PART_DISPLAY,
	SOCINFO_PART_AUDIO,
	SOCINFO_PART_MODEM,
	SOCINFO_PART_WLAN,
	SOCINFO_PART_COMP,
	SOCINFO_PART_SENSORS,
	SOCINFO_PART_NPU,
	SOCINFO_PART_SPSS,
	SOCINFO_PART_NAV,
	SOCINFO_PART_COMPUTE_1,
	SOCINFO_PART_DISPLAY_1,
	SOCINFO_PART_MAX_PARTTYPE
};

#if IS_ENABLED(CONFIG_QCOM_SOCINFO)
uint32_t socinfo_get_id(void);
uint32_t socinfo_get_serial_number(void);
const char *socinfo_get_id_string(void);
int socinfo_get_feature_code(void);
int socinfo_get_pcode(void);
char *socinfo_get_partinfo_part_name(unsigned int part_id);
uint32_t socinfo_get_partinfo_chip_id(unsigned int part_id);
uint32_t socinfo_get_partinfo_vulkan_id(unsigned int part_id);
#else
static inline uint32_t socinfo_get_id(void)
{
	return 0;
}

static inline uint32_t socinfo_get_serial_number(void)
{
	return 0;
}

static inline const char *socinfo_get_id_string(void)
{
	return "N/A";
}
int socinfo_get_feature_code(void)
{
	return -EINVAL;
}
int socinfo_get_pcode(void)
{
	return -EINVAL;
}
const char *socinfo_get_partinfo_part_name(unsigned int part_id)
{
	return NULL;
}
uint32_t socinfo_get_partinfo_chip_id(unsigned int part_id)
{
	return 0;
}
uint32_t socinfo_get_partinfo_vulkan_id(unsigned int part_id)
{
	return 0;
}
#endif /* CONFIG_QCOM_SOCINFO */

#endif /* __SOC_QCOM_SOCINFO_H__ */
