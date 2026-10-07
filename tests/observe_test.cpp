#include <doctest/doctest.h>

#include <mutex>
#include <string>
#include <vector>

#include "beet/beet.hpp"
#include "beet/observe.hpp"

namespace {

using beet::Result;
using beet::Status;
using beet::Task;

struct Bad {};

Result<int, Bad> check(int x) {
  if (x < 0) return beet::make_unexpected(Bad{});
  return x;
}
int twice(int x) { return x * 2; }

Task<Result<int, Bad>> slow(int x) {
  for (int i = 0; i < x; ++i) co_await beet::running;
  co_return x;
}

struct Recorder {
  std::mutex mutex;
  std::vector<std::string> events;

  void push(std::string e) {
    std::lock_guard lock(mutex);
    events.push_back(std::move(e));
  }
  void on_tick_begin(std::uint64_t t) { push("tick:" + std::to_string(t)); }
  void on_tick_end(std::uint64_t, Status) {}
  void on_start(std::uint32_t id) { push("start:" + std::to_string(id)); }
  void on_finish(std::uint32_t id, Status s) {
    push("finish:" + std::to_string(id) + (s == Status::Success ? ":ok" : ":fail"));
  }
  void on_halt(std::uint32_t id) { push("halt:" + std::to_string(id)); }
};
static_assert(beet::observer<Recorder>);

// then(recover(then(check, twice)), named(slow)) has IDs:
//   0 then, 1 recover, 2 then, 3 check, 4 twice, 5 slow
auto make_tree() {
  return beet::node(check)
      .then(twice)
      .recover([](Bad) { return -1; })
      .then(beet::named<"wait">(slow));
}
using Tree = decltype(make_tree());

constexpr auto info = beet::describe<Tree>();
static_assert(info.size() == 6);
static_assert(info[0].kind == "then" && info[0].parent == beet::no_parent);
static_assert(info[1].kind == "recover" && info[1].parent == 0);
static_assert(info[2].kind == "then" && info[2].parent == 1);
static_assert(info[3].kind == "leaf" && info[3].parent == 2);
static_assert(info[4].kind == "leaf" && info[4].parent == 2);
static_assert(info[5].kind == "leaf" && info[5].parent == 0 && info[5].label == "wait");
static_assert(info[3].input == "int");

}  // namespace

TEST_CASE("describe reports type names") {
  CHECK(info[3].error.find("Bad") != std::string_view::npos);
  CHECK(info[0].output == "int");
}

TEST_CASE("an untraced runner and a traced runner produce the same result") {
  auto tree = make_tree();
  beet::Runner plain{tree, 2};
  Recorder rec;
  beet::Runner traced{tree, 2, rec};
  while (plain.tick() == Status::Running) {
  }
  while (traced.tick() == Status::Running) {
  }
  CHECK(plain.result().value() == traced.result().value());
}

TEST_CASE("events follow depth-first execution and running nodes stay open across ticks") {
  Recorder rec;
  beet::Runner r{make_tree(), 1, rec};  // `wait` receives 2, so it runs for two ticks
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Running);
  CHECK(r.tick() == Status::Success);
  CHECK(rec.events == std::vector<std::string>{"tick:0", "start:0", "start:1", "start:2", "start:3", "finish:3:ok",
                                               "start:4", "finish:4:ok", "finish:2:ok", "finish:1:ok", "start:5",
                                               "tick:1", "tick:2", "finish:5:ok", "finish:0:ok"});
}

TEST_CASE("a short-circuited failure still finishes every enclosing node") {
  Recorder rec;
  beet::Runner r{make_tree(), -1, rec};
  while (r.tick() == Status::Running) {
  }
  CHECK(rec.events == std::vector<std::string>{"tick:0", "start:0", "start:1", "start:2", "start:3",
                                               "finish:3:fail", "finish:2:fail", "finish:1:ok", "start:5",
                                               "finish:5:ok", "finish:0:ok"});
}

TEST_CASE("halting reports every node that was still running, innermost first") {
  Recorder rec;
  beet::Runner r{make_tree(), 3, rec};
  r.tick();
  rec.events.clear();
  r.halt();
  CHECK(rec.events == std::vector<std::string>{"halt:5", "halt:0"});
}

TEST_CASE("parallel_any halts the losing children") {
  auto tree = beet::parallel_any(slow, slow, slow);
  Recorder rec;
  beet::Runner r{tree, 1, rec};
  while (r.tick() == Status::Running) {
  }
  CHECK(rec.events.back() == "finish:0:ok");
  CHECK(rec.events == std::vector<std::string>{"tick:0", "start:0", "start:1", "start:2", "start:3", "tick:1",
                                               "finish:1:ok", "finish:2:ok", "finish:3:ok", "finish:0:ok"});

  auto racing = beet::parallel_any(beet::node(slow), beet::node([](int x) { return x; }));
  Recorder rec2;
  beet::Runner r2{racing, 5, rec2};
  r2.tick();
  CHECK(rec2.events ==
        std::vector<std::string>{"tick:0", "start:0", "start:1", "start:2", "finish:2:ok", "halt:1", "finish:0:ok"});
}

TEST_CASE("retry reports each attempt on the same ID") {
  auto tree = beet::retry(3, check);
  Recorder rec;
  beet::Runner r{tree, -1, rec};
  r.tick();
  CHECK(rec.events == std::vector<std::string>{"tick:0", "start:0", "start:1", "finish:1:fail", "start:1",
                                               "finish:1:fail", "start:1", "finish:1:fail", "finish:0:fail"});
}

TEST_CASE("StatusTable tracks the latest status of each node") {
  auto tree = make_tree();
  beet::StatusTable<decltype(tree)> table;
  beet::Runner r{tree, 1, table};
  r.tick();
  CHECK(table.status(0) == beet::NodeStatus::Running);
  CHECK(table.status(3) == beet::NodeStatus::Success);
  CHECK(table.status(5) == beet::NodeStatus::Running);
  r.tick();
  r.tick();
  CHECK(table.status(0) == beet::NodeStatus::Success);
  CHECK(table.changed_at(5) == 2);
  CHECK(table.changed_at(3) == 0);
}

TEST_CASE("StatusTable works with a thread pool") {
  beet::ThreadPoolExecutor pool(4);
  auto tree = beet::parallel_all(pool, slow, slow, slow, slow);
  beet::StatusTable<decltype(tree)> table;
  beet::Runner r{tree, 2, table};
  CHECK(r.tick() == Status::Running);
  for (std::uint32_t id = 0; id < table.size; ++id) CHECK(table.status(id) == beet::NodeStatus::Running);
  while (r.tick() == Status::Running) {
  }
  for (std::uint32_t id = 0; id < table.size; ++id) CHECK(table.status(id) == beet::NodeStatus::Success);
}
