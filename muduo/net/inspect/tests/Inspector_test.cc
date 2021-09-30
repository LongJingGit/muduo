#include "muduo/net/inspect/Inspector.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/EventLoopThread.h"

using namespace muduo;
using namespace muduo::net;

/**
 * 主线程 Main 我们在这里称为 A 线程，t.startLoop 中启动的另一个线程称为 B 线程。
 *
 * B 线程的 loop 传给了 A 线程，并在 A 线程内传递给了内部的 acceptor/channel/tcpServer 等其他所有模块，也就是说，A 线程内其他模块初始化的时候，使用的不是本线程的 EventLoop 实例，而是 B 线程的 EventLoop 实例。
 * 那么，如果在 A 线程中执行这些模块的回调函数（比如 Acceptor::listen），就需要判断是不是在当前 loop 的线程(即 B 线程内)，如果不是，则 A 线程需要将回调函数转移到 B 线程；
 *
 * Muduo 的限制是在于，当前 loop 的所有回调函数，必须在 loop 自己的线程内执行。
 */

int main()
{
  EventLoop loop;
  EventLoopThread t;
  Inspector ins(t.startLoop(), InetAddress(12345), "test");   // 用另一个线程的 EventLoop 实例初始化了 Inspector 以及内部的所有模块
  loop.loop();
}

