// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/net/EventLoop.h"

#include "muduo/base/Logging.h"
#include "muduo/base/Mutex.h"
#include "muduo/net/Channel.h"
#include "muduo/net/Poller.h"
#include "muduo/net/SocketsOps.h"
#include "muduo/net/TimerQueue.h"

#include <algorithm>

#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace muduo;
using namespace muduo::net;

namespace
{
  __thread EventLoop *t_loopInThisThread = 0; // 线程局部存储

  const int kPollTimeMs = 10000;

  int createEventfd()
  {
    int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (evtfd < 0)
    {
      LOG_SYSERR << "Failed in eventfd";
      abort();
    }
    return evtfd;
  }

#pragma GCC diagnostic ignored "-Wold-style-cast"
  class IgnoreSigPipe
  {
  public:
    IgnoreSigPipe()
    {
      ::signal(SIGPIPE, SIG_IGN);
      // LOG_TRACE << "Ignore SIGPIPE";
    }
  };
#pragma GCC diagnostic error "-Wold-style-cast"

  IgnoreSigPipe initObj;
} // namespace

EventLoop *EventLoop::getEventLoopOfCurrentThread()
{
  return t_loopInThisThread;
}

/**
 * EventLoop:
 * 首先需要明确：
 * 1. EventLoop 控制着 Poller 的生命周期
 * 2. Poller 负责监听所有 fd（Poller 维护了 channel 和 fd 的映射表）
 * 3. EventLoop 的 poller 对象在 loop 中被调用。也就是说，其实可以认为 EventLoop 在监听着本线程内所有模块的所有 fd。当 loop 循环启动时，会一直监听注册到
 * 内核监听队列中的所有 fd，用户可以选择从内核监听队列中删除某些 fd ，从而不再监听对应的消息。
 * 4. 用户可以调用 quit 从 loop 循环中退出。
 *
 * 如何实现监听本线程内所有模块的 fd :
 * 1. EventLoop 实例会通过指针的方式传入到本线程内所有需要的模块
 * 2. 每个模块如果需要监听某个事件，则创建自己的 fd，然后使用 fd 创建自己的 channel 实例，并设置该 channel 的回调函数
 * 3. 每个模块通过传入的 EventLoop 实例的指针，将自己的 channel 和 fd 注册到(EventLoop的) poller 的映射表中，将 fd 注册到 poller 内核监听队列中
 * 4. 如果 poller 监听的 fd 更新了状态(包括可读/可写等)，会返回给 EventLoop，由 EventLoop 执行每个 channel 的回调函数
 */

/**
 * Poller
 * 1. poller 的生命中周期由 EventLoop 唯一控制
 * 2. poller 维护着 <fd, channel*> 的映射表，其他模块创建自己的监听描述符和 channel，由 EventLoop 更新到 poller 的映射表中，由 Poller 监听所有的 fd
 * 3. poller 监听到某个 fd 的事件，会将 fd 对应的 channel 返回给 EventLoop，由 EventLoop 执行 channel 的回调
 */

/**
 * wakeupFd_ 描述符的作用：
 * wakeupFd_ 同样是需要监听的文件描述符，并且有属于自己的 channel 和回调函数。该 fd 仍然是由 EventLoop 的 poller 的内核监听队列来管理的。
 *
 * wakeupFd 在监听什么？—— one loop per thread
 * 每个线程只有一个 eventloop 实例，如果在另一个线程中调用其他线程的 loop 回调函数，需要将该 loop 转移到 loop 自己的线程中执行。
 *
 * 转移操作使用了消息队列来执行。比如 A 线程将回调函数放入到队列，B 线程从队列中获取回调函数并执行。但是 B 线程如何知道何时需要从队列中获取回调函数？
 * 所以这里维护了一个 fd，当 B 线程将回调函数放入到队列时，会向该 fd 写入特定内容（仅为唤醒 A 线程的 fd）；
 * A 线程监听到该 fd 上的可读事件，读取 fd 的内容（为了清除该 fd 的可读事件，无其他用途），然后从队列中获取 B 线程写入的回调函数并执行
 *
 * 也就是说，wakeupFd_ 仅做信号通知的用途（仅用来传输控制信息，并不传输数据）
 *
 * pendingFunctors_
 * 两个线程之间使用了 std::vector<callBack> pendingFunctors_ 容器作为回调函数在多个线程之间转移的方式，该容器被多线程共享。
 *
 * 比如 B 线程将自己的 EventLoop 实例的指针传递给 A 线程并初始化了 A 线程内的所有模块，则在 A 线程中执行 B 线程的回调函数的时候，
 * 需要将该回调函数转移到 B 线程的 EventLoop 实例中执行。
 * 具体的操作方式是将该回调函数放入 pendingFunctors_ 中，然后向 wakeupFd_ 写入某个消息，触发 B 线程监听事件；
 * B 线程会先读取 wakeupFd_ 上的消息（但是并不处理）,然后从 pendingFunctors_ 中读取回调函数并执行
 */

/**
 * Channel:
 * 每个文件描述符 fd 对应着一个 channel，并且每个 channel 都有自己的回调函数。fd 会被注册到 poll 的内核监听队列中。
 *
 * poll 由 EventLoop 中的 Poller 实例进行回调
 */

/**
 * TimerQueue：
 * 指定了回调函数的执行时间，需要延迟执行/定时执行的回调函数，可以放入到时间队列中，当时间到达之后，再执行回调
 */

/**
 * GUARDED_BY(mutex_):
 * clang 的线程安全分析模块，通过代码注释的方式，告诉编译器哪些成员变量和成员函数是受哪个 mutex 保护的。这样如果忘记加锁，编译器会给警告。
 */

EventLoop::EventLoop()
    : looping_(false),
      quit_(false),
      eventHandling_(false),
      callingPendingFunctors_(false),
      iteration_(0),
      threadId_(CurrentThread::tid()),
      poller_(Poller::newDefaultPoller(this)),
      timerQueue_(new TimerQueue(this)),
      wakeupFd_(createEventfd()),
      wakeupChannel_(new Channel(this, wakeupFd_)),
      currentActiveChannel_(NULL)
{
  LOG_DEBUG << "EventLoop created " << this << " in thread " << threadId_;
  if (t_loopInThisThread)
  {
    LOG_FATAL << "Another EventLoop " << t_loopInThisThread
              << " exists in this thread " << threadId_;
  }
  else
  {
    t_loopInThisThread = this;
  }
  /**
   * @brief
   * FIXME: channel 的 readCallback 的回调函数参数是 Timestamp 类型，但是执行绑定的回调函数 EventLoop::handleRead 的参数为空，能执行
   * 这样的绑定吗？
   */
  wakeupChannel_->setReadCallback(std::bind(&EventLoop::handleRead, this)); // 设置 channel 的回调函数
  // we are always reading the wakeupfd
  wakeupChannel_->enableReading(); // 更新 channel 到 poller 的映射表中，更新 channel->fd 到 poll 的内核监听队列中
}

EventLoop::~EventLoop()
{
  LOG_DEBUG << "EventLoop " << this << " of thread " << threadId_
            << " destructs in thread " << CurrentThread::tid();
  wakeupChannel_->disableAll();
  wakeupChannel_->remove();
  ::close(wakeupFd_);
  t_loopInThisThread = NULL;
}

void EventLoop::loop()
{
  assert(!looping_);
  assertInLoopThread();
  looping_ = true;
  quit_ = false; // FIXME: what if someone calls quit() before loop() ?
  LOG_TRACE << "EventLoop " << this << " start looping";

  while (!quit_)
  {
    activeChannels_.clear();
    pollReturnTime_ = poller_->poll(kPollTimeMs, &activeChannels_);
    ++iteration_;
    if (Logger::logLevel() <= Logger::TRACE)
    {
      printActiveChannels();
    }
    // TODO sort channel by priority
    eventHandling_ = true;
    for (Channel *channel : activeChannels_)
    {
      currentActiveChannel_ = channel;
      currentActiveChannel_->handleEvent(pollReturnTime_); // 执行 channel 的回调函数
    }
    currentActiveChannel_ = NULL;
    eventHandling_ = false;
    doPendingFunctors(); // 执行从其他线程传递到本线程的 loop 中的回调函数
  }

  LOG_TRACE << "EventLoop " << this << " stop looping";
  looping_ = false;
}

void EventLoop::quit()
{
  quit_ = true;
  // There is a chance that loop() just executes while(!quit_) and exits,
  // then EventLoop destructs, then we are accessing an invalid object.
  // Can be fixed using mutex_ in both places.
  if (!isInLoopThread())
  {
    wakeup();
  }
}

void EventLoop::runInLoop(Functor cb)
{
  if (isInLoopThread())
  {
    cb();
  }
  else
  {
    queueInLoop(std::move(cb));
  }
}

void EventLoop::queueInLoop(Functor cb)
{
  {
    MutexLockGuard lock(mutex_);
    pendingFunctors_.push_back(std::move(cb));
  }

  if (!isInLoopThread() || callingPendingFunctors_)
  {
    wakeup();
  }
}

size_t EventLoop::queueSize() const
{
  MutexLockGuard lock(mutex_);
  return pendingFunctors_.size();
}

TimerId EventLoop::runAt(Timestamp time, TimerCallback cb)
{
  return timerQueue_->addTimer(std::move(cb), time, 0.0);
}

TimerId EventLoop::runAfter(double delay, TimerCallback cb)
{
  Timestamp time(addTime(Timestamp::now(), delay));
  return runAt(time, std::move(cb));
}

TimerId EventLoop::runEvery(double interval, TimerCallback cb)
{
  Timestamp time(addTime(Timestamp::now(), interval));
  return timerQueue_->addTimer(std::move(cb), time, interval);
}

void EventLoop::cancel(TimerId timerId)
{
  return timerQueue_->cancel(timerId);
}

void EventLoop::updateChannel(Channel *channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  poller_->updateChannel(channel);
}

void EventLoop::removeChannel(Channel *channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  if (eventHandling_)
  {
    assert(currentActiveChannel_ == channel ||
           std::find(activeChannels_.begin(), activeChannels_.end(), channel) == activeChannels_.end());
  }
  poller_->removeChannel(channel);
}

bool EventLoop::hasChannel(Channel *channel)
{
  assert(channel->ownerLoop() == this);
  assertInLoopThread();
  return poller_->hasChannel(channel);
}

void EventLoop::abortNotInLoopThread()
{
  LOG_FATAL << "EventLoop::abortNotInLoopThread - EventLoop " << this
            << " was created in threadId_ = " << threadId_
            << ", current thread id = " << CurrentThread::tid();
}

void EventLoop::wakeup()
{
  uint64_t one = 1;
  ssize_t n = sockets::write(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::wakeup() writes " << n << " bytes instead of 8";
  }
}

void EventLoop::handleRead()
{
  uint64_t one = 1;
  ssize_t n = sockets::read(wakeupFd_, &one, sizeof one);
  if (n != sizeof one)
  {
    LOG_ERROR << "EventLoop::handleRead() reads " << n << " bytes instead of 8";
  }
}

void EventLoop::doPendingFunctors()
{
  std::vector<Functor> functors;
  callingPendingFunctors_ = true;

  {
    MutexLockGuard lock(mutex_);
    functors.swap(pendingFunctors_);
  }

  for (const Functor &functor : functors)
  {
    functor();
  }
  callingPendingFunctors_ = false;
}

void EventLoop::printActiveChannels() const
{
  for (const Channel *channel : activeChannels_)
  {
    LOG_TRACE << "{" << channel->reventsToString() << "} ";
  }
}
