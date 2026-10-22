# Examples

Each example is a complete program built with [Pixi](https://pixi.sh). The Rerun and roboplan examples use separate Pixi environments, so the default one stays small.

## Robot patrol

[robot_patrol.cpp](robot_patrol.cpp) runs the mission from the main README. The robot plans a path to each goal and drives it while watching its battery. Low battery and planning failures are handled with `recover`. The tree itself lives in [robot_mission.hpp](robot_mission.hpp). Every connection between its nodes is a typed value. The mission runs at `beet::rate<10>`, and `runner.run(100ms)` uses a 100ms period instead of the default one second, so the patrol runs ten times faster than real time.

```sh
pixi run example
```

A CTest check runs `nm` on this binary and fails if any tracing symbols appear in it.

## Tree status in Rerun

[rerun_status.cpp](rerun_status.cpp) runs the same mission and logs it to [Rerun](https://rerun.io) as a graph, one frame per tick. The graph's edges come from `beet::describe`, so its structure is derived entirely from the tree's type. A `StatusTable`, fed by its `listeners()`, colors each node by status. The logging code is in [rerun_tree.hpp](rerun_tree.hpp) and works with any tree.

```sh
pixi run -e rerun rerun-example   # spawns the viewer
pixi run -e rerun rerun-record    # writes build-rerun/mission.rrd instead
```

## UR5 motion planning

[roboplan_ur5.cpp](roboplan_ur5.cpp) plans free-space UR5 motions with [roboplan](https://github.com/open-planning/roboplan)'s RRT and executes them on a kinematic model, logged to Rerun. It shows the long-running-work tools working together:
- **`beet::offload`** runs the planner on its own thread, so planning does not stall the tick.
- **`beet::Channel`** sends each trajectory from the planning branch to a controller branch under `parallel_any`. The controller replies once it has executed the trajectory.
- **`beet::rate`** runs the controller at 100 ticks per second and the planning branch at 10, and `runner.run()` paces the tree at the controller's rate.
- **Resources as values.** An "open cell" leaf creates the channel and the robot model and passes them to both branches through their inputs. Nothing is shared outside the tree.

```sh
pixi run -e roboplan roboplan-example   # spawns the viewer
pixi run -e roboplan roboplan-record    # writes build-roboplan/ur5.rrd instead
```

Pass `--seed N` to the binary to vary the planner's random seed.
