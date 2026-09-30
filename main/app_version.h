/**
 * @file app_version.h
 * @brief 版本号**解析与比较**（供上电 OTA 判断高低用）
 *
 * 本机版本号的唯一来源是 version.h（SNAP_FW_VERSION）。
 * 本文件**不定义**版本号，只提供"把两个版本字符串比大小"的能力：
 *   本机 "0.1.1"  vs  服务端清单 "0.1.2"  -> 后者更高才升级。
 *
 * 为什么不直接用 SNAP_FW_VERSION_NUM 比：
 *   那需要清单也发一个整数，且"主.次.修"三个数要手工编码成 0xMMmmpp，
 *   改版本时容易忘记同步改 NUM（事实上它至今没被任何代码使用过）。
 *   这里改成两边都用可读的字符串，靠解析比较，少一个可忘的步骤。
 *
 * 比较规则（严格，防降级）：
 *   只有 candidate > current 才返回 true；相等 / 更低一律 false。
 *   这样伪造一份低版本清单无法把设备刷回带漏洞的旧固件。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 解析 "v1.2.3" / "1.2.3" / "1.2" / "1" 为数值
 *
 * 前缀（v、release- 等非数字字符）自动跳过；只认数字和 '.'，最多三段。
 * "-rc1" / "+build" 之后的部分被忽略（比较时不计预发布标记）。
 *
 * @param s      输入字符串，NULL 视为空串
 * @param major  输出，主版本号（可为 NULL）
 * @param minor  输出，次版本号（可为 NULL）
 * @param patch  输出，修订号（可为 NULL）
 * @return true = 字符串里至少解析出一个数字；false = 完全没有数字
 */
bool app_version_parse(const char *s, int *major, int *minor, int *patch);

/**
 * @brief 语义化比较两个版本号
 * @return a > b 返回 1，a < b 返回 -1，数值部分相等返回 0
 */
int app_version_cmp(const char *a, const char *b);

/**
 * @brief candidate 是否比 current 新（严格更高）
 * @note  相等或更低都返回 false —— 这是"不允许降级"的执行点
 */
bool app_version_is_newer(const char *candidate, const char *current);

#ifdef __cplusplus
}
#endif
