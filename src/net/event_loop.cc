#include "event_loop.h"

#if defined(__gnu_linux__)
#include <sys/prctl.h>
#endif
#include <unistd.h>

#include "libevent_reactor.h"
#include "log.h"
#include "util.h"

namespace pikiwidb {

static thread_local EventLoop* g_this_loop = nullptr;

std::atomic<int> EventLoop::obj_id_generator_{0};
std::atomic<TimerId> EventLoop::timerid_generator_{0};

EventLoop::EventLoop() {
  assert(!g_this_loop && "There must be only one EventLoop per thread");
  g_this_loop = this;

  reactor_.reset(new internal::LibeventReactor());
  notifier_ = std::make_shared<internal::PipeObject>();
}

void EventLoop::Run() {
#if defined(__gnu_linux__)
  if (!name_.empty()) {
    prctl(PR_SET_NAME, ToValue<unsigned long>(name_.c_str()));
  }
#endif

  Register(notifier_, kEventRead);

  while (running_) {
    if (task_mutex_.try_lock()) {
      decltype(tasks_) funcs;
      funcs.swap(tasks_);
      task_mutex_.unlock();

      for (const auto& f : funcs) f();
    }

    if (!reactor_->Poll()) {
      ERROR("Reactor poll failed");
    }
  }

  for (auto& pair : objects_) {
    reactor_->Unregister(pair.second.get());
  }

  objects_.clear();
  reactor_.reset();
}

void EventLoop::Stop() {
  running_ = false;
  notifier_->Notify();
}

std::future<bool> EventLoop::Cancel(TimerId id) {
  if (InThisLoop()) {
    bool ok = reactor_ ? reactor_->Cancel(id) : false;

    std::promise<bool> prom;
    auto fut = prom.get_future();
    prom.set_value(ok);
    return fut;
  } else {
    auto fut = Execute([id, this]() -> bool {
      if (!reactor_) {
        return false;
      }
      bool ok = reactor_->Cancel(id);
      INFO("cancell timer {} {}", id, ok ? "succ" : "fail");
      return ok;
    });
    return fut;
  }
}

bool EventLoop::InThisLoop() const { return this == g_this_loop; }
EventLoop* EventLoop::Self() { return g_this_loop; }

bool EventLoop::Register(std::shared_ptr<EventObject> obj, int events) {
  if (!obj) return false;

  assert(InThisLoop());
  assert(obj->GetUniqueId() == -1);

  if (!reactor_) {
    return false;
  }

  // alloc unique id
  int id = -1;
  do {
    id = obj_id_generator_.fetch_add(1) + 1;
    if (id < 0) {
      obj_id_generator_.store(0);
    }
  } while (id < 0 || objects_.count(id) != 0);

  obj->SetUniqueId(id);
  if (reactor_->Register(obj.get(), events)) {
    objects_.insert({obj->GetUniqueId(), obj});
    return true;
  }

  return false;
}

bool EventLoop::Modify(std::shared_ptr<EventObject> obj, int events) {
  if (!obj) return false;

  assert(InThisLoop());
  assert(obj->GetUniqueId() >= 0);
  assert(objects_.count(obj->GetUniqueId()) == 1);

  if (!reactor_) {
    return false;
  }
  return reactor_->Modify(obj.get(), events);
}

void EventLoop::Unregister(std::shared_ptr<EventObject> obj) {
  if (!obj) return;

  int id = obj->GetUniqueId();
  assert(InThisLoop());
  assert(id >= 0);
  assert(objects_.count(id) == 1);

  if (!reactor_) {
    return;
  }
  reactor_->Unregister(obj.get());
  objects_.erase(id);
}

bool EventLoop::Listen(const char* ip, int port, NewTcpConnCallback ccb) {
  auto s = std::make_shared<TcpListenerObj>(this);
  s->SetNewConnCallback(ccb);

  return s->Bind(ip, port);
}

std::shared_ptr<TcpObject> EventLoop::Connect(const char* ip, int port, NewTcpConnCallback ccb,
                                              TcpConnFailCallback fcb) {
  auto c = std::make_shared<TcpObject>(this);
  c->SetNewConnCallback(ccb);
  c->SetFailCallback(fcb);

  if (!c->Connect(ip, port)) {
    c.reset();
  }

  return c;
}

void EventLoop::Reset() {
  for (auto& kv : objects_) {
    Unregister(kv.second);
  }
  objects_.clear();

  {
    std::unique_lock<std::mutex> guard(task_mutex_);
    tasks_.clear();
  }

  reactor_.reset(new internal::LibeventReactor());
  notifier_ = std::make_shared<internal::PipeObject>();
}

}  // namespace pikiwidb
