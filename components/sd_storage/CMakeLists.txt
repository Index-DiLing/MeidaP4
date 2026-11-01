# MeidaP4 · SD 卡存储卷（WS-storage）
#
# 头文件只暴露 esp_err_t/bool/uint64_t，所以除了 fatfs/sdmmc 这些"必须出现在 .c 里"的
# 组件之外，别的依赖都可以放 PRIV_REQUIRES。
#
# 依赖说明（IDF 6.x 会校验 include 是否落在 requirements 里）：
#   esp_driver_sdmmc  -> driver/sdmmc_host.h、driver/sdmmc_types.h（SDMMC_HOST_DEFAULT / slot 配置）
#   sdmmc             -> sdmmc_cmd.h、sd_pwr_ctrl.h、sd_pwr_ctrl_interface.h
#   esp_hw_support    -> esp_ldo_regulator.h（plan §4.4 点名的片内 LDO 供电）
#   fatfs             -> esp_vfs_fat.h（esp_vfs_fat_sdmmc_mount / esp_vfs_fat_info）
#   esp_timer         -> esp_timer_get_time()（挂载耗时/uptime 日志）
idf_component_register(
    SRCS "sd_storage.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_driver_sdmmc
        sdmmc
        esp_hw_support
        fatfs
        vfs
        esp_timer
)
