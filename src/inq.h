#pragma once
// ============================================================================
//  inq.h —— IN-Q 的核心逻辑（移植自 INQ.CT）
//
//  CT 里所有状态都放在 alloc 出来的 INQ 这块内存里，钩子和 Lua 定时器通过它
//  通信；这里换成一个结构体，含义和偏移一一对应（注释里的 INQ+x）。
// ============================================================================
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"

namespace inq {

struct State {
    // INQ+0：重开流程的状态机
    //   0 空闲  1 请求重开  2 重置中（拦住回据点的加载）  3 等接任务  4 等出发
    volatile std::int32_t restart = 0;
    volatile std::uint8_t optReset = 0;       // INQ+4  重置后从据点重接（不勾 = 直接重开）
    volatile std::uint8_t optAcceptOnly = 0;  // INQ+5  只接任务不出发
    volatile std::uint8_t optResetOnly = 0;   // INQ+6  这一次只重置（不接任务、不出发）
    volatile std::uint8_t optWyvern = 0;      // INQ+7  普通地图用翼龙起始
    volatile std::uint32_t questId = 0;       // INQ+8  0 = 用上一次的任务
    volatile std::int32_t food = 0;           // INQ+C  进任务时套用的猫饭（编号，0 = 不管）
    volatile std::int32_t equipLoadout = 0;   // INQ+10 待套用的装备套装 1..224
    volatile std::int32_t itemLoadout = 0;    // INQ+14 待套用的道具套装 1..80
    volatile std::uint32_t message = 0;       // INQ+50 提示位：1 装备套装无效 2 接任务失败
                                              //                4 现在不能用 8 请先关闭窗口
};
extern State g;

// 等游戏代码解密后核对字节、装钩子。report 是给人看的结果（写日志、发聊天框）。
// 返回 false = 核心钩子没装上，重开功能不可用。
bool Install(const Config& cfg, std::string& report);
bool Ready();
// 运行时执行总闸。卸载态只做逻辑旁路：钩子仍驻留，但回调不再改游戏状态。
bool Enabled();
void SetEnabled(bool enabled);
// 取消尚未提交的重开/套装/提示；不卸钩，也不改持久 Config。
void CancelPending();

// 重开（快捷键或面板）。会先套用 Active 那套全套配置。重开方式看配置里的 Reset / AcceptOnly。
void RequestRestart(const Config& cfg);

// 面板主动选择时请求立即换装备，不重开、不套道具、不改猫饭/任务。
// 只排队，不在调用线程写游戏；后一次覆盖前一次。空/无效值清掉旧的立即待办。
// true = 编号已接受或空选择已取消，false = 无效编号/未 Ready/卸载态。
// 游戏线程下一次匹配 ApplyResetAndEquip UI 调用且非读盘、对象链有效时提交；
// 若尚未到该落点会继续等待。正在重开时不插队，RequestRestart 会清掉旧立即待办。
// Config 及调用方的 Model 仍由主循环现有锁保护；不要从渲染线程直接调用。
// 调用方只应响应显式选择事件；切 profile/slot/载入配置不要自动调用，需 CancelPending。
// SetEnabled(false) 会排空发布者/回调并清队列，重新启用不会补做卸载态的选择。
bool RequestEquipApply(const Config& cfg, const std::string& equip);
// activeSet 为空/不存在/未指定装备时，相当于传空串：不脱装备，仅取消未提交的立即待办。
bool RequestActiveSetEquipApply(const Config& cfg);

// 只重置（原 IN-Q 的 Reset only / INQ+6）：把当前任务重置掉、回据点，不接任务也不出发。
// 不套用全套配置 —— 这一趟只是把任务清掉。
void RequestResetOnly();

// 每 100ms 调一次（CT 里 Lua 定时器做的事）
void Tick(const Config& cfg);

// 「刚进去的那一关」：游戏自己记着最后一次进的任务（不管是我们重开的还是玩家在柜台接的）。
// Tick 里发现变了就记下来，主循环取走写进全局任务 —— 取消套用之后重开才不会退回更早的选择。
int TakeEnteredQuest();

// 把配置里的开关同步进状态（启动时和改配置后）
void ApplyOptions(const Config& cfg);

// 装备/道具套装：配置里写的是编号或游戏里的名字，这里解析成编号（0 = 找不到）
int ResolveEquip(const std::string& s);
int ResolveItem(const std::string& s);

// 游戏里的套装名（给面板列选项用）。装备按存放格，道具按游戏里的显示编号，只列有内容的。
// number：游戏里看到的编号（道具 = 列表显示编号，装备 = 第几套）
// store ：道具套装的存放格（存档里真正的位置，跟显示编号可能不一样）；装备与 number 相同
using NamedLoadout = RefName;
std::vector<NamedLoadout> ListEquipLoadouts();
std::vector<NamedLoadout> ListItemLoadouts();

// 把配置里「按名字写的」装备/道具换成编号（导入原版 config.txt 之后调一次）。
// 返回对不上、已留空的条数。
int NormalizeEquipItemNumbers(Config& cfg);

// 老配置一次性迁移：读取存档里的名单（没读到就返回 0，什么也不改），把名字换成编号。
// 返回改了几处。调用方负责在改动后存盘。
int MigrateLoadoutRefs(Config& cfg);

} // namespace inq
