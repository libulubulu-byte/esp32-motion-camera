/**
 * @file human_stub.c
 * @brief 人形检测 stub（只打日志）
 *
 * ★★ 免责声明（对应任务书 0.11）：
 *   本文件不包含任何真实的人形检测、身份识别、人脸比对或活体检测算法。
 *   detect() 返回的 score 是用 JPEG 体积做的启发式假分数，唯一用途是
 *   验证"检测结果 → 日志/元数据"这条链路是通的，绝不能用于任何实际判断。
 */

#include "human_stub.h"
#include "logger.h"

#include <string.h>

#include "sdkconfig.h"

static const char *TAG = LOG_T_PIPE;

/* 假分数阈值：仅用于把日志分成 "hit"/"miss" 两类，无实际意义 */
#define STUB_THRESHOLD  50

void human_stub_init(void)
{
    LOGW(TAG, "human detect stub enabled: LOG-ONLY, 不含任何真实检测算法");
}

bool human_stub_detect(const uint8_t *jpeg, size_t len, int *out_score)
{
    if (!jpeg || len == 0) {
        if (out_score) {
            *out_score = 0;
        }
        return false;
    }

    /* 启发式：以 60KB 为"1 个人"的参考体积，线性映射到 0..100 并 clamp。
     * 这纯粹是演示用的假分数。 */
    int score = (int)((len * 100ULL) / (60ULL * 1024ULL));
    if (score > 100) {
        score = 100;
    }

    bool hit = (score >= STUB_THRESHOLD);
    LOGI(TAG, "human stub: %s score=%d jpeg_len=%u (STUB, 无真实检测能力)",
         hit ? "hit" : "miss", score, (unsigned)len);

    if (out_score) {
        *out_score = score;
    }
    return hit;
}
