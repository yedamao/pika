// Copyright (c) 2019-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef PIKA_REPL_SERVER_THREAD_H_
#define PIKA_REPL_SERVER_THREAD_H_

#include "pink/src/holy_thread.h"

#include "include/pika_repl_server_conn.h"

class PikaReplServerThread : public pink::HolyThread {
 public:
  PikaReplServerThread(const std::set<std::string>& ips, int port, int cron_interval);
  virtual ~PikaReplServerThread() = default;

  void Write(const std::string& msg, const std::string& ip_port, int fd);

 private:
  class ReplServerConnFactory : public pink::ConnFactory {
   public:
    virtual std::shared_ptr<pink::PinkConn> NewPinkConn(
        int connfd,
        const std::string& ip_port,
        pink::Thread* thread,
        void* worker_specific_data,
        pink::PinkEpoll* pink_epoll) const override {
      return std::make_shared<PikaReplServerConn>(connfd, ip_port, thread, worker_specific_data);
    }
  };

  class Handles : public pink::ServerHandle {
   public:
    void CronHandle() const override;
    using pink::ServerHandle::AccessHandle;
    bool AccessHandle(std::string& ip) const override;
  };

  void NotifyWrite(const std::string& ip_port, int fd);
  virtual void ProcessNotifyEvents(const pink::PinkFiredEvent* pfe) override;

  ReplServerConnFactory conn_factory_;
  Handles handles_;

  slash::Mutex write_buf_mu_;
  std::map<int, std::string> write_buf_;
};

#endif
