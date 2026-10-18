#pragma once
// 快捷键：手柄（XInput）和键盘组合，多个组合用 | 隔开。按下的那一刻触发一次。
#include <string>
#include <vector>

struct Combo {
    unsigned short pad = 0;      // XINPUT_GAMEPAD_* 按钮位
    bool lt = false, rt = false; // 扳机
    std::vector<int> keys;       // 键盘虚拟键
    bool IsPad() const { return pad || lt || rt; }
};

struct Hotkey {
    std::vector<Combo> alts;
    bool was = false;
    std::string bad;             // 认不出的写法（给日志）
};

Hotkey ParseHotkey(const std::string& spec);

// ---- 设置快捷键时「按一下就认」用 ----
// 手柄按键位：XINPUT_GAMEPAD_* 的 16 位，加上两个扳机
constexpr unsigned kPadLT = 1u << 16, kPadRT = 1u << 17;
// 键盘上现在按着的、能当快捷键的键（虚拟键码；Esc 留给「取消」，空格回车这类不算；
// Delete / Backspace 也报，单按它们在面板里是「清除」）
std::vector<int> KeysHeld();
// 一组键 → 快捷键写法（ParseHotkey 认得的），比如 LB+RB+LT+RT、Ctrl+F5
std::string PadComboText(unsigned mask);
std::string KeyComboText(const std::vector<int>& vks);
// 同上，但不适合当快捷键时返回空串，why 里是原因（手柄单键、字母键不带修饰键……）
std::string PadComboSpec(unsigned mask, std::string& why);
std::string KeyComboSpec(const std::vector<int>& vks, std::string& why);

// 每帧调一次，返回这一帧是不是刚按下。只在游戏窗口在前台时生效。
class Input {
public:
    bool Init();
    void Poll();
    bool Fired(Hotkey& hk);
    unsigned PadHeld() const;    // 所有手柄现在按着的键（kPadLT/kPadRT 是扳机）
private:
    bool Held(const Combo& c) const;
    bool focused_ = false;
    bool padOk_ = false;
    unsigned short buttons_ = 0;
    unsigned char lt_ = 0, rt_ = 0;
};
