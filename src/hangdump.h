#pragma once
// 退出挂死诊断。
//
// 症状：面板开过一次之后退出游戏，窗口没了，进程却一直在（BGM 还在响）。
// 我们的收尾日志写完之后就再没有任何一行 —— 说明卡住的不是我们自己的收尾代码，
// 而是游戏后续的销毁流程。光看代码分不清是「卡在 DXGI/驱动」「卡在等 loader lock」
// 还是「主线程还在消息循环里」。
//
// 这里在窗口销毁后起一个诊断线程：进程要是还在，就把每个线程此刻的调用栈
// （模块 + 偏移）写进日志。只读操作（SuspendThread + StackWalk64），不改进程状态，
// 也绝不结束进程。dbghelp 在插件启动时就加载好，避免卡在 loader lock 时再去 LoadLibrary。
namespace hangdump {
void Init();                            // Worker 启动时调一次：加载 dbghelp、初始化符号
void Arm(int firstDelayMs, int secondDelayMs);   // 窗口销毁后调用
}
