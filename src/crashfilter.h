#pragma once
// 哪些异常值得写进日志。
//
// 这个判断发生在「第一次异常」的处理器里：那一刻别的线程可能正拿着 loader lock
// 或者堆锁，处理器里只要做文件 IO / 查模块表就可能把它等死。所以正常的控制流
// 异常一律不碰 —— 尤其是 CLR 的 0xE0434352 和 C++ 的 0xE06D7363，SharpPluginLoader
// 一启动就抛几百上千条，记下来只会把日志刷满（实测一次启动 96 条 CLR 异常、
// 870 行栈记录），而且退出阶段 CLR 还会继续抛，正好落在最危险的那一刻。
namespace crashfilter {
inline bool Recordable(unsigned long code)
{
    switch (code) {
    case 0x40010006u:   // OutputDebugString
    case 0x406D1388u:   // SetThreadName
    case 0xC0000194u:   // POSSIBLE_DEADLOCK：锁争用的警告，不是崩溃
    case 0xE0434352u:   // .NET / CLR 异常
    case 0xE06D7363u:   // C++ throw
        return false;
    default:
        return true;
    }
}
} // namespace crashfilter
