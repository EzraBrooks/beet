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

## Building

The developer environment is managed by [Pixi](https://pixi.sh):

```sh
pixi run test      # configure, build, run tests
pixi run example   # run examples/robot_patrol.cpp
```

beet is header-only and depends on [tl::expected](https://github.com/TartanLlama/expected). To use it from CMake, link `beet::beet`.

## License

[MIT](LICENSE)
