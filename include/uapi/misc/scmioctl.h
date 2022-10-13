/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note
 * * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 *  */
#ifndef __SCMIOCTL_H__
#define __SCMIOCTL_H__

#include <linux/types.h>

#define SCM_HAND_SHAKE_IOCTL       _IOWR('R', 10, struct scm_hand_shake)

#define MAX_QCOM_SCM_RESULT 3

#define MAX_QCOM_SCM_IN 10

struct scm_hand_shake {
	unsigned int svc;
	unsigned int cmd;
	unsigned int arginfo;
	unsigned int args_buffer[MAX_QCOM_SCM_IN];
	unsigned int ret;
	unsigned int arg_type;
	unsigned int qcom_scm_res[MAX_QCOM_SCM_RESULT];
};

#endif
