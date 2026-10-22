/*
 * ota.h —— OTA 启动校验（分区表已支持 ota_0/ota_1 与 otadata 回滚）
 *
 * 升级流程（APP 侧下发 URL，固件侧可用 esp_https_ota 或二次开发实现）：
 *   1) 新固件写入未运行的 OTA 分区并 otadata 置为待验证；
 *   2) 重启后若本函数未在宽限期内调用 otadata 回滚，bootloader 会退回旧固件；
 *   3) 因此 app_main 在硬件自检通过后立即调用 ota_boot_mark_valid()。
 */
#ifndef PB_OTA_H
#define PB_OTA_H

/* 自检通过后调用：把当前运行分区标记为有效，取消自动回滚 */
void ota_boot_mark_valid(void);

/* 打印当前运行分区与固件版本（调试用） */
void ota_log_info(void);

#endif /* PB_OTA_H */
