// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/net/EventLoopThread.h"

#include "muduo/net/EventLoop.h"

using namespace muduo;
using namespace muduo::net;

/**
 * 在任意一个线程创建并运行 EventLoop。
 *
 * EventLoopThread 会创建新的线程，并在新的线程中调用 EventLoopThread::threadFunc，创建新的 EventLoop 实例，并且执行 EventLoop::loop()
 *
 * 其他线程可以通过调用 EventLoopThread::startLoop() 接口获取到 EventLoopThread 创建的 EventLoop 对象的地址。
 *
 * 可以按照优先级将不同的 socket 分给不同的 IO 线程，避免优先级反转。
 */
EventLoopThread::EventLoopThread(const ThreadInitCallback& cb,
                                 const string& name)
  : loop_(NULL),
    exiting_(false),
    thread_(std::bind(&EventLoopThread::threadFunc, this), name),
    mutex_(),
    cond_(mutex_),
    callback_(cb)
{
}

EventLoopThread::~EventLoopThread()
{
  exiting_ = true;
  if (loop_ != NULL) // not 100% race-free, eg. threadFunc could be running callback_.
  {
    // still a tiny chance to call destructed object, if threadFunc exits just now.
    // but when EventLoopThread destructs, usually programming is exiting anyway.
    loop_->quit();
    thread_.join();
  }
}

// 应用程序调用该接口，返回新线程中 EventLoop 对象的地址
EventLoop* EventLoopThread::startLoop()
{
  assert(!thread_.started());
  // 启动新线程，在新的线程中执行线程的回调函数 EventLoopThread::threadFunc()
  thread_.start();

  EventLoop* loop = NULL;
  {
    MutexLockGuard lock(mutex_);
    while (loop_ == NULL)
    {
      cond_.wait();
    }

    /**
     * 在这里 loop 已经是另一个线程的 loop 了:
     *
     * 当前 main 线程在这里 wait
     * 另一个线程执行了 EventLoopThread::threadFunc，并创建了新的loop，将新的 loop 赋值给了 loop_，然后 notify 等待的线程
     * 所以可以在 main 线程中获得新创建的线程中的 loop
     *
     * loop_ 是两个线程之间的共享资源
     **/
    loop = loop_;
  }

  return loop;
}

void EventLoopThread::threadFunc()
{
  EventLoop loop;

  if (callback_)
  {
    callback_(&loop);
  }

  // 新的线程中创建了 loop，并且将 loop 赋值给了多线程共享资源 loop_。这样，其他线程就可以获取到该 loop 并开始执行事件循环了
  {
    MutexLockGuard lock(mutex_);
    loop_ = &loop;
    cond_.notify();
  }

  // 新的线程也在这里执行事件循环
  loop.loop();
  //assert(exiting_);
  MutexLockGuard lock(mutex_);
  loop_ = NULL;
}

