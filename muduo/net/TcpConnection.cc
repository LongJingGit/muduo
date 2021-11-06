// Copyright 2010, Shuo Chen.  All rights reserved.
// http://code.google.com/p/muduo/
//
// Use of this source code is governed by a BSD-style license
// that can be found in the License file.

// Author: Shuo Chen (chenshuo at chenshuo dot com)

#include "muduo/net/TcpConnection.h"

#include "muduo/base/Logging.h"
#include "muduo/base/WeakCallback.h"
#include "muduo/net/Channel.h"
#include "muduo/net/EventLoop.h"
#include "muduo/net/Socket.h"
#include "muduo/net/SocketsOps.h"

#include <errno.h>

using namespace muduo;
using namespace muduo::net;

void muduo::net::defaultConnectionCallback(const TcpConnectionPtr &conn)
{
  LOG_TRACE << conn->localAddress().toIpPort() << " -> "
            << conn->peerAddress().toIpPort() << " is "
            << (conn->connected() ? "UP" : "DOWN");
  // do not call conn->forceClose(), because some users want to register message callback only.
}

void muduo::net::defaultMessageCallback(const TcpConnectionPtr &,
                                        Buffer *buf,
                                        Timestamp)
{
  buf->retrieveAll();
}

/**
 * @brief
 * TcpConnection 是建立真正的连接，入参 sockfd 从监听 socket 的回调函数中获取到的连接 socket
 * 数据收发都是通过该连接 sockfd 来完成的
 *
 * channel_：连接描述符会建立自己的 channel_，然后将要监听事件注册给 poll；如果内核监听到事件，会执行事件回调函数，即可以从连接 socket 上读取和写入数据
 */

// sockfd: 已经建立好连接的 socket fd
TcpConnection::TcpConnection(EventLoop *loop,
                             const string &nameArg,
                             int sockfd,
                             const InetAddress &localAddr,
                             const InetAddress &peerAddr)
    : loop_(CHECK_NOTNULL(loop)),
      name_(nameArg),
      state_(kConnecting),
      reading_(true),
      socket_(new Socket(sockfd)),
      channel_(new Channel(loop, sockfd)),
      localAddr_(localAddr),
      peerAddr_(peerAddr),
      highWaterMark_(64 * 1024 * 1024)
{
  channel_->setReadCallback(
      std::bind(&TcpConnection::handleRead, this, _1)); // 设置该 channel 的回调函数，最终会由 EventLoop 调用
  channel_->setWriteCallback(
      std::bind(&TcpConnection::handleWrite, this));
  channel_->setCloseCallback(
      std::bind(&TcpConnection::handleClose, this));
  channel_->setErrorCallback(
      std::bind(&TcpConnection::handleError, this));
  LOG_DEBUG << "TcpConnection::ctor[" << name_ << "] at " << this
            << " fd=" << sockfd;
  socket_->setKeepAlive(true); // 定期探查 TCP 连接是否还存在
}

TcpConnection::~TcpConnection()
{
  LOG_DEBUG << "TcpConnection::dtor[" << name_ << "] at " << this
            << " fd=" << channel_->fd()
            << " state=" << stateToString();
  assert(state_ == kDisconnected);
}

bool TcpConnection::getTcpInfo(struct tcp_info *tcpi) const
{
  return socket_->getTcpInfo(tcpi);
}

string TcpConnection::getTcpInfoString() const
{
  char buf[1024];
  buf[0] = '\0';
  socket_->getTcpInfoString(buf, sizeof buf);
  return buf;
}

/**
 * @brief
 * send 接口由用户调用，但是用户无需关注能否一次性发送结束。
 */
void TcpConnection::send(const void *data, int len)
{
  send(StringPiece(static_cast<const char *>(data), len));
}

void TcpConnection::send(const StringPiece &message)
{
  if (state_ == kConnected)
  {
    if (loop_->isInLoopThread())
    {
      sendInLoop(message);
    }
    else
    {
      void (TcpConnection::*fp)(const StringPiece &message) = &TcpConnection::sendInLoop;
      loop_->runInLoop(
          std::bind(fp,
                    this, // FIXME
                    message.as_string()));
      //std::forward<string>(message)));
    }
  }
}

// FIXME efficiency!!!
void TcpConnection::send(Buffer *buf)
{
  if (state_ == kConnected)
  {
    if (loop_->isInLoopThread())
    {
      sendInLoop(buf->peek(), buf->readableBytes());
      buf->retrieveAll();
    }
    else
    {
      void (TcpConnection::*fp)(const StringPiece &message) = &TcpConnection::sendInLoop;
      loop_->runInLoop(
          std::bind(fp,
                    this, // FIXME
                    buf->retrieveAllAsString()));
      //std::forward<string>(message)));
    }
  }
}

void TcpConnection::sendInLoop(const StringPiece &message)
{
  sendInLoop(message.data(), message.size());
}

/**
 * @brief
 * send 接口由用户调用，然后调用到 sendInLoop 中。用户无需关注是否能够一次性发送完毕。因为在 tcpConnection 维护了发送缓冲区。
 * 先尝试发送数据，如果能够一次性发送完，则不会启用 writeCallback；如果只发送了部分数据，则会把剩余的数据放入到 outputBuffer_，并开始关注 writeCallback，
 * 剩余的数据将在 writeCallback 中完成发送。
 *
 * 如果当前 outputBuffer_ 中已经有待发送的数据，则就不能先尝试发送了（会造成数据乱序）
 */
void TcpConnection::sendInLoop(const void *data, size_t len)
{
  loop_->assertInLoopThread();
  ssize_t nwrote = 0;
  size_t remaining = len;
  bool faultError = false;
  if (state_ == kDisconnected)
  {
    LOG_WARN << "disconnected, give up writing";
    return;
  }
  // if no thing in output queue, try writing directly
  if (!channel_->isWriting() && outputBuffer_.readableBytes() == 0)
  {
    nwrote = sockets::write(channel_->fd(), data, len);
    if (nwrote >= 0)
    {
      remaining = len - nwrote;
      /**
       * @brief 已经发送完毕，调用用户回调
       * 需要注意的是: writeCompleteCallback_ 是完全发送之后的回调函数（即发送缓冲区被清空），用于通知用户已经完全发送完毕.
       * 需要和 writeCallback（和 TcpConnection::handleWrite 绑定） 区分开:
       *
       * writeCompleteCallback_: 是用户注册的回调函数
       * writeCallback: 无须用户关注，是 tcpconnection 无法一次性发送完所有数据的时候，会注册 writeable 事件，该事件会触发 writeCallback 回调
       */
      if (remaining == 0 && writeCompleteCallback_)
      {
        loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
      }
    }
    else // nwrote < 0
    {
      nwrote = 0;
      if (errno != EWOULDBLOCK)
      {
        LOG_SYSERR << "TcpConnection::sendInLoop";
        if (errno == EPIPE || errno == ECONNRESET) // FIXME: any others?
        {
          faultError = true;
        }
      }
    }
  }

  // 无法一次性发送完数据，将剩余数据发到发送缓冲区，然后注册 EPOLLOUT 事件
  assert(remaining <= len);
  if (!faultError && remaining > 0)
  {
    size_t oldLen = outputBuffer_.readableBytes();

    /**
     * @brief 高水位回调，用于处理发送数据的速度高于对方接收数据的速度，数据一直堆积在发送端，造成内存暴涨的问题：
     * 如果输出缓冲区的长度超过用户指定的大小，就会触发回调（只在上升沿触发一次）
     *
     * highWaterMark_：用户指定的缓冲区大小
     * highWaterMarkCallback_：用户指定的高水位回调函数
     */
    // 如果发送缓冲区中已有的数据累加上这次需要发送的数据的大小，超过了用户指定的触发高水位回调的标志位并且用户设置了高水位回调函数，则触发高水位回调函数
    if (oldLen + remaining >= highWaterMark_ && oldLen < highWaterMark_ && highWaterMarkCallback_)
    {
      loop_->queueInLoop(std::bind(highWaterMarkCallback_, shared_from_this(), oldLen + remaining));
    }

    // 将剩余没有发送完的数据放入到 outputBuffer_ 中
    outputBuffer_.append(static_cast<const char *>(data) + nwrote, remaining);
    if (!channel_->isWriting())
    {
      // 注册 POLLOUT 事件，剩余数据将在 TcpConnection::handleWrite 中发送
      channel_->enableWriting();
    }
  }
}

void TcpConnection::shutdown()
{
  // FIXME: use compare and swap
  if (state_ == kConnected)
  {
    setState(kDisconnecting);   // 设置标志位，表示正在执行关闭流程
    // FIXME: shared_from_this()?
    loop_->runInLoop(std::bind(&TcpConnection::shutdownInLoop, this));
  }
}

/**
 * 发送缓冲区里有数据，如何执行关闭流程？
 * 当缓冲区有数据的时候，不会执行关闭流程。
 * 只有在 TcpConnection::handleWrite 中将数据全部发送完毕之后，TcpConnection::handleWrite 中会重新调用 shutdownInLoop 执行关闭流程。
 */
void TcpConnection::shutdownInLoop()
{
  loop_->assertInLoopThread();
  // 判断发送缓冲区是否有数据
  if (!channel_->isWriting())
  {
    // we are not writing
    socket_->shutdownWrite();
  }
}

// void TcpConnection::shutdownAndForceCloseAfter(double seconds)
// {
//   // FIXME: use compare and swap
//   if (state_ == kConnected)
//   {
//     setState(kDisconnecting);
//     loop_->runInLoop(std::bind(&TcpConnection::shutdownAndForceCloseInLoop, this, seconds));
//   }
// }

// void TcpConnection::shutdownAndForceCloseInLoop(double seconds)
// {
//   loop_->assertInLoopThread();
//   if (!channel_->isWriting())
//   {
//     // we are not writing
//     socket_->shutdownWrite();
//   }
//   loop_->runAfter(
//       seconds,
//       makeWeakCallback(shared_from_this(),
//                        &TcpConnection::forceCloseInLoop));
// }

void TcpConnection::forceClose()
{
  // FIXME: use compare and swap
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    setState(kDisconnecting);
    loop_->queueInLoop(std::bind(&TcpConnection::forceCloseInLoop, shared_from_this()));
  }
}

void TcpConnection::forceCloseWithDelay(double seconds)
{
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    setState(kDisconnecting);
    loop_->runAfter(
        seconds,
        makeWeakCallback(shared_from_this(),
                         &TcpConnection::forceClose)); // not forceCloseInLoop to avoid race condition
  }
}

void TcpConnection::forceCloseInLoop()
{
  loop_->assertInLoopThread();
  if (state_ == kConnected || state_ == kDisconnecting)
  {
    // as if we received 0 byte in handleRead();
    handleClose();
  }
}

const char *TcpConnection::stateToString() const
{
  switch (state_)
  {
  case kDisconnected:
    return "kDisconnected";
  case kConnecting:
    return "kConnecting";
  case kConnected:
    return "kConnected";
  case kDisconnecting:
    return "kDisconnecting";
  default:
    return "unknown state";
  }
}

// 禁用 Nagle 算法，避免连续发包出现延迟
void TcpConnection::setTcpNoDelay(bool on)
{
  socket_->setTcpNoDelay(on);
}

void TcpConnection::startRead()
{
  loop_->runInLoop(std::bind(&TcpConnection::startReadInLoop, this));
}

void TcpConnection::startReadInLoop()
{
  loop_->assertInLoopThread();
  if (!reading_ || !channel_->isReading())
  {
    channel_->enableReading();
    reading_ = true;
  }
}

void TcpConnection::stopRead()
{
  loop_->runInLoop(std::bind(&TcpConnection::stopReadInLoop, this));
}

void TcpConnection::stopReadInLoop()
{
  loop_->assertInLoopThread();
  if (reading_ || channel_->isReading())
  {
    channel_->disableReading();
    reading_ = false;
  }
}

void TcpConnection::connectEstablished()
{
  loop_->assertInLoopThread();
  assert(state_ == kConnecting);
  setState(kConnected);
  channel_->tie(shared_from_this());
  channel_->enableReading(); // 注册 channel_

  connectionCallback_(shared_from_this()); // 调用用户设置的 connectionCallback_，通知用户有新的连接建立
}

void TcpConnection::connectDestroyed()
{
  loop_->assertInLoopThread();
  // 用户也可以调用 handleClose 去关闭连接，在 handleClose 中会执行 setState(kDisconnected)，所以 if 条件里的代码不会执行两次
  if (state_ == kConnected)
  {
    setState(kDisconnected);
    channel_->disableAll();

    connectionCallback_(shared_from_this()); // 通知用户连接已经断开
  }
  channel_->remove();
}

void TcpConnection::handleRead(Timestamp receiveTime)
{
  loop_->assertInLoopThread();
  int savedErrno = 0;
  // 数据会在 buffer 中不断堆积，直到用户取走
  ssize_t n = inputBuffer_.readFd(channel_->fd(), &savedErrno);
  if (n > 0)
  {
    // messageCallback_ 是用户注册的回调函数
    // 注意这里的 receiveTime 是 epoll_wait 返回的时间(即网络层收到数据的确切时间)，并不是 readv 的时间。可以用这种方式来测量消息的处理延迟！
    messageCallback_(shared_from_this(), &inputBuffer_, receiveTime);
  }
  // 读取到数据为 0 的时候，执行该连接的关闭流程
  else if (n == 0)
  {
    handleClose();
  }
  else
  {
    errno = savedErrno;
    LOG_SYSERR << "TcpConnection::handleRead";
    handleError();
  }
}

/**
 * 当 socket 变得可写时，Channel 会立即调用 TcpConnection::handleWrite，然后继续发送 outputBuffer_ 中的数据。
 * 一旦发送完毕，立刻停止监听 writeable 事件，避免 busy-loop
 */
void TcpConnection::handleWrite()
{
  loop_->assertInLoopThread();
  if (channel_->isWriting())
  {
    ssize_t n = sockets::write(channel_->fd(),
                               outputBuffer_.peek(),
                               outputBuffer_.readableBytes());
    if (n > 0)
    {
      outputBuffer_.retrieve(n);
      if (outputBuffer_.readableBytes() == 0)   // 缓冲区中数据已经完全发送结束（可读为0）
      {
        // 发送完毕，停止监听 writeable 事件，避免 busy-loop
        channel_->disableWriting();
        if (writeCompleteCallback_)
        {
          loop_->queueInLoop(std::bind(writeCompleteCallback_, shared_from_this()));
        }

        /**
         * 当发送端缓冲区全部发送完毕，尝试执行关闭流程
         * 前提是用户已经调用了 TcpConnection::shutdown 尝试关闭连接，但是由于缓冲区有数据，不能立即关闭连接。所以在这里将缓冲区的数据发送完毕后，再次执行关闭流程
         * 应用层调用 shutdown 关闭连接的时候，已经将 state_ 设置成了 kDisconnecting
         */
        if (state_ == kDisconnecting)
        {
          shutdownInLoop();
        }
      }
    }
    else
    {
      LOG_SYSERR << "TcpConnection::handleWrite";
      // if (state_ == kDisconnecting)
      // {
      //   shutdownInLoop();
      // }
    }
  }
  else
  {
    LOG_TRACE << "Connection fd = " << channel_->fd()
              << " is down, no more writing";
  }
}

void TcpConnection::handleClose()
{
  loop_->assertInLoopThread();
  LOG_TRACE << "fd = " << channel_->fd() << " state = " << stateToString();
  assert(state_ == kConnected || state_ == kDisconnecting);
  // we don't close fd, leave it to dtor, so we can find leaks easily.
  setState(kDisconnected);
  channel_->disableAll();

  TcpConnectionPtr guardThis(shared_from_this());
  connectionCallback_(guardThis);
  // must be the last line
  closeCallback_(guardThis); // TcpClient 、TcpServer 注册的关闭连接回调函数（一般绑定的是 TcpConnection::connectDestroyed）
}

void TcpConnection::handleError()
{
  int err = sockets::getSocketError(channel_->fd());
  LOG_ERROR << "TcpConnection::handleError [" << name_
            << "] - SO_ERROR = " << err << " " << strerror_tl(err);
}
