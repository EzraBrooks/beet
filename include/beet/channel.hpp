#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "beet/node.hpp"
#include "beet/result.hpp"
#include "beet/task.hpp"

namespace beet {

template <class Req, class Reply>
class Channel;

/// Input of `Channel<Req, Reply>::call()`: the channel to call and the request to send on it.
template <class Req, class Reply>
struct Call {
  Channel<Req, Reply> channel;
  Req request;
};

/// A typed request/reply link between branches of a running tree, such as a planner and a controller ticked side
/// by side. Copies share the same channel. `Reply` may be a `Result<Out, E>`, whose errors reach the caller.
template <class Req, class Reply>
class Channel {
  struct exchange {
    explicit exchange(Req v) : value(std::move(v)) {}

    std::mutex mutex;
    Req value;
    std::optional<Reply> reply;
    bool cancelled = false;
  };

  struct state {
    std::mutex mutex;
    std::deque<std::shared_ptr<exchange>> queue;
  };

 public:
  /// A request taken by the server. The caller keeps waiting until `reply()` is called or it is halted.
  class Request {
   public:
    const Req& value() const { return ex_->value; }

    /// True once the caller has been halted; the reply would be dropped.
    bool cancelled() const {
      std::lock_guard lock(ex_->mutex);
      return ex_->cancelled;
    }

    void reply(Reply r) {
      std::lock_guard lock(ex_->mutex);
      ex_->reply.emplace(std::move(r));
    }

   private:
    friend Channel;
    explicit Request(std::shared_ptr<exchange> ex) : ex_(std::move(ex)) {}
    std::shared_ptr<exchange> ex_;
  };

  Channel() : state_(std::make_shared<state>()) {}

  /// Takes the oldest request whose caller is still waiting, if any.
  std::optional<Request> try_receive() {
    std::lock_guard lock(state_->mutex);
    while (!state_->queue.empty()) {
      auto ex = std::move(state_->queue.front());
      state_->queue.pop_front();
      std::lock_guard ex_lock(ex->mutex);
      if (!ex->cancelled) return Request(std::move(ex));
    }
    return std::nullopt;
  }

  /// A node that sends its request and stays running until the server replies. Halting it cancels the request.
  static auto call() {
    using L = detail::lift_of<Reply>;
    using Out = typename L::out;
    using Err = typename L::err;
    return node<Call<Req, Reply>>([](Call<Req, Reply> c) -> Task<Result<Out, Err>> {
      auto ex = std::make_shared<exchange>(std::move(c.request));
      {
        std::lock_guard lock(c.channel.state_->mutex);
        c.channel.state_->queue.push_back(ex);
      }

      struct cancel_on_exit {
        exchange& ex;
        ~cancel_on_exit() {
          std::lock_guard lock(ex.mutex);
          ex.cancelled = true;
        }
      } guard{*ex};

      for (;;) {
        {
          std::lock_guard lock(ex->mutex);
          if (ex->reply) co_return detail::to_result<Out, Err>(std::move(*ex->reply));
        }
        co_await running;
      }
    });
  }

 private:
  std::shared_ptr<state> state_;
};

}  // namespace beet
