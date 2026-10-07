# beet

> [!WARNING]
> This repository is in very early development and is mostly AI-generated. It exists primarily to gauge community interest in this approach to behavior trees. Expect rough edges and breaking changes, and do not rely on it in production.

Typed, composable behavior trees for C++20. A tree is ordinary C++ code, and the compiler checks how its parts connect.

```cpp
auto mission = beet::node(plan_path)                        // Pose -> Path, can fail with PlanError
    .then(beet::parallel_all(drive, watch_battery))         // Path -> tuple<Odom, Level>
    .then(report)                                           // tuple<Odom, Level> -> Summary
    .recover<LowBattery>([](LowBattery) { return Summary{"docked"}; })
    .recover([](const auto& e) { return Summary{describe(e)}; });

static_assert(std::is_same_v<beet::error_t<decltype(mission)>, beet::never>);  // cannot fail

beet::Runner runner{mission, goal};
while (runner.tick() == beet::Status::Running) {}
```

## Philosophy

Most behavior tree libraries connect nodes through a blackboard: a shared, string-keyed map that nodes read from and write to. The tree's structure says nothing about what data flows where. A typo in a key, a missing write, or a type mismatch only shows up at runtime, and usually far from where it was caused.

beet takes the opposite approach. Every node is a function from an input to either an output or an error:

```text
Node<In, Out, Err>  =  In -> Task<Result<Out, Err>>
```

Three consequences follow from that shape.

**Data flows along the edges.** `a.then(b)` passes `a`'s output straight into `b`'s input. There is no shared state to keep in sync, and every value a node sees comes from the node before it. If `b` cannot accept what `a` produces, the tree does not compile.

**Failure is part of the type.** A node's `Err` is the set of everything that can go wrong inside it. Composing nodes combines their error sets, so the root's type lists every failure the tree can produce. Nothing escapes untyped.

**Handling an error removes it from the type.** `.recover<E>(handler)` handles `E` and drops it from the error set. Once every error is handled, the set becomes `never`, a type with no values, so `Result<Out, never>` is a compile-time proof that the tree always produces an `Out`. If a refactor adds a new failure mode, that proof stops holding and the compiler points at the gap. Nothing is silently dropped at runtime.

The same rules apply inside coroutine nodes. `co_await child(x)` yields the child's value and propagates its failure upward, but only if the caller's error set covers the child's errors. Otherwise the build fails.

## Behavior tree semantics

| Concept | beet |
| --- | --- |
| Sequence | `a.then(b)`, `a \| b`, `sequence(a, b, c)` |
| Selector | `a.fallback(b)`, `fallback(a, b, c)` |
| Parallel | `parallel_all`, `parallel_any`, `parallel_n<K>`, with an optional `ThreadPoolExecutor` |
| Running | `co_await beet::running;` suspends a node until the next tick |
| Halt | Destroying a suspended node; cleanup lives in destructors or `.finally()` |
| Decorators | `retry`, `repeat`, `timeout_ticks`, `condition` |

Leaves can be plain functions (`Out(In)` or `Result<Out, E>(In)`) or coroutines (`Task<Result<Out, E>>(In)`). Use `AnyNode<In, Out, Err>` to hide a subtree's concrete type behind a stable interface.

## Long-running work and concurrent branches

`beet::offload(fn)` runs a plain function on its own thread and stays running until it returns, so slow work such as motion planning does not stall the tick. `fn` may take a trailing `std::stop_token`, which is stopped if the node is halted.

`beet::Channel<Req, Reply>` links branches that run side by side, such as a planner and a controller under `parallel_any`. `Channel::call()` is a node that takes a `beet::Call{channel, request}`, waits for the reply, and fails with the reply's errors when `Reply` is a `Result`. The server takes requests with `try_receive()` and answers with `reply()`; halting the caller cancels its request.

Resources such as channels are ordinary values. A leaf creates them, and later nodes receive them through their inputs, so nothing is shared outside the tree and every dependency appears in a node's signature. [examples/roboplan_ur5.cpp](examples/roboplan_ur5.cpp) plans UR5 motions with [roboplan](https://github.com/open-planning/roboplan)'s RRT on an offloaded thread and hands the trajectories to a controller that runs on every tick, all logged to Rerun:

```sh
pixi run -e roboplan roboplan-example
```

Known gaps that example exposes:
- **Threading resources:** a resource reaches later nodes only by riding along in every intermediate output, so node signatures carry values they only pass through.
- **Parallel inputs:** parallel children must take an identical input type, even if each needs only part of it.
- **Recover handlers** see only the error, not the node's input, so context they need has to travel in the error or be captured when the tree is built.
- **Construction-time parameters:** decorator arguments such as the `repeat` count are fixed when the tree is built and cannot come from the tree's input.
- **Same-type errors merge:** error sets are keyed by type, so two failures that are both `std::string` are indistinguishable. Wrap them in distinct types.
- **Branches that never finish:** a controller can return `Result<never, never>`, but `parallel_any` still reports `variant<Out, never>` instead of collapsing it to `Out`.
- **Cancellation:** halting an offloaded job only requests a stop; the function runs to completion unless it checks its `std::stop_token`.
- **Thread safety:** offloaded functions run on other threads, and the compiler cannot check what they share.

## Opt-in tracing

A tree's type already spells out its shape, so beet derives the structure at compile time rather than recording it at runtime. `beet::describe<Tree>()` returns a `constexpr` table with one entry per node, in depth-first order. Each entry holds the node's kind, label, parent, and the names of its input, output and error types. Labels are attached with `beet::named<"plan">(node)` and live only in the type.

To watch a tree run, pass an observer to the `Runner`:

```cpp
#include "beet/observe.hpp"

beet::StatusTable<decltype(tree)> table;  // latest status of every node, by ID
beet::Runner runner{tree, input, table};
runner.tick();
table.status(5);                          // NodeStatus::Running
```

An observer is any type with `on_tick_begin`, `on_tick_end`, `on_start(id)`, `on_finish(id, status)` and `on_halt(id)`. At runtime it only receives integer node IDs; everything else comes from the static table. Halts are reported innermost first. If the tree uses a `ThreadPoolExecutor`, the observer must be thread-safe. `StatusTable` is.

**Tracing costs nothing unless you use it.** Every node runs through a trace parameter. A `Runner` without an observer passes an empty `untraced` value, and on that path every hook is discarded by `if constexpr`. The traced wrappers are only instantiated when a `Runner` is constructed with an observer, and `beet/observe.hpp` is not included by `beet/beet.hpp`. A CTest check runs `nm` on the untraced example binary and fails if any tracing symbols appear in it.

Current limits:
- **User coroutine leaves:** subtrees awaited inside a user coroutine leaf (`co_await subtree(x)`) are reported as part of that leaf.
- **`AnyNode`:** it appears as a single opaque node.

[examples/rerun_status.cpp](examples/rerun_status.cpp) logs the robot mission to [Rerun](https://rerun.io). The edges come from `describe`, and each tick logs a `GraphNodes` frame colored by status. It builds in a separate Pixi environment so the default one stays small:

```sh
pixi run -e rerun rerun-example
```

## Building

The developer environment is managed by [Pixi](https://pixi.sh):

```sh
pixi run test      # configure, build, run tests
pixi run example   # run examples/robot_patrol.cpp
```

beet is header-only and depends on [tl::expected](https://github.com/TartanLlama/expected). To use it from CMake, link `beet::beet`.

## License

[MIT](LICENSE)
