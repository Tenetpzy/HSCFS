#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "communication/channel.h"

// spdk设备信息
typedef struct comm_dev
{
    struct spdk_nvme_ctrlr *nvme_ctrlr;  // SPDK扫描到的设备控制器
    struct spdk_nvme_ns *ns;  // 控制器的namespace
    struct comm_channel_controller channel_ctrlr;  // 该设备的qpair管理器
} comm_dev;

#ifdef __cplusplus
}
#endif