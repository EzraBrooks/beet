# 🫜 beet

> [!WARNING]
> This repository is in very early development and is mostly AI-generated. It exists primarily to gauge community interest in this approach to behavior trees. Expect rough edges and breaking changes, and do not rely on it in production.

beet is a header-only C++20 library for behavior trees in which a tree is ordinary C++ code: nodes are typed functions, composites are function calls, and the compiler checks how every part connects before the tree ever ticks.

**Why beet:**
- **Typed dataflow, no blackboard.** Nodes pass values to each other along the tree's edges. If one node cannot accept what the previous one produces, the tree does not compile.
- **Exhaustive, typed failure.** Every node declares what can go wrong. Error sets combine as nodes compose, handling an error removes it, and a root of type `Result<Out, never>` is a compile-time proof that the tree cannot fail.
- **Trees are plain C++.** Composites and decorators are higher-order functions that take their children, so the code nests exactly as the tree does.
- **You pay only for what you use.** Tree structure, tracing, and dispatch are resolved at compile time, so an untraced tree of plain functions runs as ordinary calls. See [Performance](#performance).

```cpp
auto mission = beet::recover(
    beet::recover<LowBattery>(
        beet::sequence(
            plan_path,                                  // Pose -> Path, can fail with PlanError
            beet::parallel_all(drive, watch_battery),   // Path -> tuple<Odom, Level>
            report),                                    // tuple<Odom, Level> -> Summary
        [](LowBattery) { return Summary{"docked"}; }),
    [](const auto& e) noexcept { return Summary{describe(e)}; });   // PlanError, Stuck, or beet::Exception

static_assert(std::is_same_v<beet::error_t<decltype(mission)>, beet::never>);  // cannot fail

beet::Runner runner{mission, goal};
while (runner.tick() == beet::Status::Running) {}
```

> [!NOTE]
> See [examples/](examples/README.md) for runnable trees, including the mission above, a live tree view in [Rerun](https://rerun.io), and UR5 motion planning.

## Design philosophy

Most behavior tree libraries connect nodes through a blackboard: a shared, string-keyed map that nodes read from and write to. The tree's structure says nothing about what data flows where. A typo in a key, a missing write, or a type mismatch only shows up at runtime, and usually far from where it was caused.

beet takes the opposite approach. Every node is a function from an input to either an output or an error:

```text
Node<In, Out, Err>  =  In -> Task<Result<Out, Err>>
```

Three consequences follow from that shape.

**Data flows along the edges.** `sequence(a, b)` passes `a`'s output straight into `b`'s input. There is no shared state to keep in sync, and every value a node sees comes from the node before it. If `b` cannot accept what `a` produces, the tree does not compile. The tradeoff is that every dependency travels through signatures. A resource such as a channel is created by a leaf and passed along in outputs, so some nodes carry values they only pass through. Parallel nodes give their single input to every child. A child that needs only part of it can start with a small leaf that extracts that part, as in `sequence(pick_part, child)`.

**Failure is part of the type.** A node's `Err` is the set of everything that can go wrong inside it. Composing nodes combines their error sets, so the root's type lists every failure the tree can produce. Nothing escapes untyped. An error set is a set of types, so two failures of the same type merge into one. Give each failure its own type (`PlanError`, `Stuck`) instead of reusing something like `std::string`.

**Handling an error removes it from the type.** `recover<E>(node, handler)` handles `E` and drops it from the error set. Once every error is handled, the set becomes `never`, a type with no values, so `Result<Out, never>` is a compile-time proof that the tree always produces an `Out`. If a refactor adds a new failure mode, that proof stops holding and the compiler points at the gap. Nothing is silently dropped at runtime. A handler sees only the error, not where in the tree it happened, so the error should carry the context needed to handle it. Anything else can be captured when the tree is built.

The same rules apply inside coroutine nodes. `co_await child(x)` yields the child's value and propagates its failure upward, but only if the caller's error set covers the child's errors. Otherwise the build fails.

**Exceptions are errors too.** Any leaf or handler that is not `noexcept` gets `beet::Exception` in its error set, and beet catches whatever it throws and returns it as that error. `Exception::what()` gives the message, and `recover<beet::Exception>` handles it like any other error. A `noexcept` callable opts out: plain functions that throw anyway terminate, and `noexcept` coroutines rethrow from `tick()`. When exceptions are disabled (`-fno-exceptions`, or `BEET_NO_EXCEPTIONS`), nothing gains `beet::Exception` and beet never throws.

**Counts are part of the type too.** `retry<3>`, `repeat<3>` and `timeout_ticks<10>` take their counts as template arguments, so a count of zero fails to compile and loop bounds are constants. A count that varies at runtime belongs in a coroutine leaf.

## Behavior tree semantics

| Concept | beet |
| --- | --- |
| Sequence | `sequence(a, b, c)` |
| Selector | `fallback(a, b, c)` |
| Parallel | `parallel_all`, `parallel_any`, `parallel_n<K>`, with an optional `ThreadPoolExecutor` |
| Running | `co_await beet::running;` suspends a node until the next tick |
| Halt | Destroying a suspended node; cleanup lives in destructors or `finally(node, f)` |
| Decorators | `recover`, `retry`, `repeat`, `timeout_ticks`, `condition` |

Trees are written top-down: every composite and decorator is a function that takes its children, so the code nests exactly as the tree does, with the root as the outermost call.

Leaves can be plain functions (`Out(In)` or `Result<Out, E>(In)`) or coroutines (`Task<Result<Out, E>>(In)`). Use `AnyNode<In, Out, Err>` to hide a subtree's concrete type behind a stable interface.

## Performance

beet aims to do as much work as possible at compile time, so the compiler can inline and optimize a tree like any other code:
- **Structure is static.** A tree's shape, node IDs, and type names are computed at compile time from its type.
- **Plain functions stay plain.** A subtree made only of plain functions finishes on the tick it starts, so beet runs it as ordinary nested calls with no coroutine frames. beet allocates nothing to tick a tree made entirely of plain functions.
- **Tracing compiles away.** Without an observer, every tracing hook is discarded at compile time (see [Opt-in tracing](#opt-in-tracing)).
- **No virtual dispatch on the default path.** Parallel nodes on the default inline executor tick their children with a compile-time fold, and a `ThreadPoolExecutor` is called directly rather than through a virtual interface.
- **Counts are constants.** Decorator counts are template arguments, so their loop bounds are known to the compiler.

## Long-running work and concurrent branches

`beet::offload(fn)` runs a plain function on its own thread and stays running until it returns, so slow work such as motion planning does not stall the tick. `fn` may take a trailing `std::stop_token`, which is stopped if the node is halted. Cancellation is cooperative: halting destroys the node right away, but the thread only receives a stop request, because C++ has no safe way to kill a thread.

`beet::Channel<Req, Reply>` links branches that run side by side, such as a planner and a controller under `parallel_any`. `Channel::call()` is a node that takes a `beet::Call{channel, request}`, waits for the reply, and fails with the reply's errors when `Reply` is a `Result`. The server takes requests with `try_receive()` and answers with `reply()`; halting the caller cancels its request.

Channels are ordinary values. A leaf creates them, and later nodes receive them through their inputs (see [Design philosophy](#design-philosophy)).

Current limitations:
- **Branches that never finish:** a controller can return `Result<never, never>`, but `parallel_any` still reports `variant<Out, never>` instead of collapsing it to `Out`.
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

## Building

The developer environment is managed by [Pixi](https://pixi.sh):

```sh
pixi run test   # configure, build, run tests
```

beet is header-only and depends on [tl::expected](https://github.com/TartanLlama/expected). To use it from CMake, link `beet::beet`.

## License

[MIT](LICENSE)
