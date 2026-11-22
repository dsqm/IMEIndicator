#include "settings_internal.h"

/* ================= 「有未保存改动」判定 =================
   保存按钮与标题星号都跟着这个判断走。抽成独立文件是为了能单测：
   它只依赖 ImeCfg，不碰任何窗口。 */

/* 两个状态样式是否等价。
   ★ 除了比字段值，还要比「自定义」位（SF_SIZE / SF_OFFSETX / SF_OFFSETY /
     SF_ALPHA）：值相同但一个是本状态的显式值、一个是跟随 cn_cn 时，存出来的
     JSON 不一样（跟随的不会写这个字段），必须算改动。否则取消勾选「自定义」
     后值没变，保存按钮不会亮。
   ★ 其余 have 位不参与比较：CommitForm 会无条件给当前页的状态补 SF_ENABLED
     等位，比了会在打开设置时误判成有改动。 */
static int StyleSame(const StateStyle* a, const StateStyle* b) {
    const unsigned kCustomBits = SF_SIZE | SF_OFFSETX | SF_OFFSETY | SF_ALPHA;
    if ((a->have ^ b->have) & kCustomBits) return 0;
    return a->enabled == b->enabled && a->rgb == b->rgb && a->shape == b->shape &&
           wcscmp(a->image, b->image) == 0 && a->size == b->size &&
           a->offsetX == b->offsetX && a->offsetY == b->offsetY &&
           a->alpha == b->alpha && a->transient == b->transient &&
           a->inherit == b->inherit;
}

int SettingsCfgSame(const ImeCfg* a, const ImeCfg* b) {
    if (a->pollMs != b->pollMs || a->trackMs != b->trackMs ||
        a->imeStrategy != b->imeStrategy ||
        a->caretTimeoutMs != b->caretTimeoutMs ||
        a->hideFullscreen != b->hideFullscreen ||
        a->hideComposition != b->hideComposition ||
        a->wpsCom != b->wpsCom || a->autoHideMs != b->autoHideMs)
        return 0;
    if (!StyleSame(&a->def, &b->def)) return 0;
    for (int i = 0; i < IMEST_COUNT; i++)
        if (!StyleSame(&a->st[i], &b->st[i])) return 0;
    if (a->ignoreCount != b->ignoreCount) return 0;
    for (int i = 0; i < a->ignoreCount; i++)
        if (_wcsicmp(a->ignore[i], b->ignore[i]) != 0) return 0;
    return 1;
}
