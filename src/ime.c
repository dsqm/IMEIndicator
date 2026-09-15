#include "ime_status.h"

/* 前台线程的焦点窗口：GetForegroundWindow 只管"哪个顶层窗口在前台"，
   输入附着在焦点控件（hwndFocus）上，键盘布局 / IME 都要按它的线程取。 */
HWND ImeFocusedWindow(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return NULL;
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    GUITHREADINFO gi;
    ZeroMemory(&gi, sizeof(gi));
    gi.cbSize = sizeof(gi);
    if (tid && GetGUIThreadInfo(tid, &gi)) {
        if (gi.hwndFocus) return gi.hwndFocus;
        if (gi.hwndActive) return gi.hwndActive;
    }
    return fg;
}

int ImeIsCapsLock(void) {
    return (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0;
}

/* 前台键盘布局是否英文语言：HKL 低 16 位是语言 ID，低字节是主语言
   （0x09 = LANG_ENGLISH）。英文布局（0409/0809/0c09…）都算英文键盘。 */
int ImeIsEnglishKeyboard(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    HKL hkl = GetKeyboardLayout(tid);
    return (((DWORD)(UINT_PTR)hkl) & 0xFF) == 0x09;
}