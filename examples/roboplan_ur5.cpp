// Plans free-space UR5 motions with roboplan's RRT and executes them on a
// kinematic model, logged to Rerun.
//
// The planning branch hands each trajectory to a controller that replies once
// the trajectory has been executed. The controller ticks 100 times per second
// and the planning branch 10 times, so the tree's root runs at 100. Both
// branches get their resources from the "open cell" leaf; nothing is shared
// outside the tree.
//
//   pixi run -e roboplan roboplan-example                        # spawns the
//   viewer
//   ./roboplan_ur5 --save ur5.rrd --seed 3                       # writes a
//   recording instead

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <Eigen/Dense>
#include <pinocchio/multibody.hpp>
#include <rerun.hpp>
#include <rerun/archetypes/coordinate_frame.hpp>
#include <rerun/archetypes/scalars.hpp>
#include <rerun/archetypes/text_log.hpp>
#include <rerun/components/fill_mode.hpp>
#include <rerun/components/line_strip3d.hpp>
#include <rerun/recording_stream.hpp>
#include <roboplan/core/path_utils.hpp>
#include <roboplan/core/scene.hpp>
#include <roboplan/core/types.hpp>
#include <roboplan_example_models/resources.hpp>
#include <roboplan_rrt/rrt.hpp>
#include <roboplan_toppra/toppra.hpp>

#include "beet/beet.hpp"
#include "beet/observe.hpp"
#include "rerun_tree.hpp"

namespace {

using beet::Result;
using beet::Task;
using roboplan::JointPath;
using roboplan::JointTrajectory;

constexpr std::string_view group = "arm";
constexpr std::string_view tool = "tool0";

// Ticks per second of each branch. The controller's rate is also TOPP-RA's
// sampling rate, since it executes one trajectory point per tick.
constexpr std::size_t control_rate = 100;
constexpr std::size_t planning_rate = 10;
constexpr double control_dt = 1.0 / control_rate;

struct Config {
  unsigned seed = 0;
  std::shared_ptr<const rerun::RecordingStream> rec;
};

struct PlanningFailed {
  std::string why;
};
struct ParameterizationFailed {
  std::string why;
};
struct StartMismatch {
  double distance;
};
struct ExecutionReport {
  double duration = 0;
  std::size_t points = 0;
};

std::string describe(const PlanningFailed& e) {
  return "planning failed: " + e.why;
}
std::string describe(const ParameterizationFailed& e) {
  return "time parameterization failed: " + e.why;
}
std::string describe(const StartMismatch& e) {
  return "trajectory starts " + std::to_string(e.distance) +
         " rad from the robot";
}

using Trajectories =
    beet::Channel<JointTrajectory, Result<ExecutionReport, StartMismatch>>;

rerun::Position3D to_position(const Eigen::Matrix4d& tf) {
  return {static_cast<float>(tf(0, 3)), static_cast<float>(tf(1, 3)),
          static_cast<float>(tf(2, 3))};
}

// The kinematic "robot": the controller writes joint positions straight into
// it.
class Robot {
 public:
  Robot(std::shared_ptr<roboplan::Scene> scene, Eigen::VectorXd q)
      : scene_(std::move(scene)),
        data_(scene_->getModel()),
        arm_(scene_->getJointGroupInfo(std::string(group))->q_indices),
        q_(std::move(q)) {
    const auto& frames = scene_->getModel().frames;
    for (const auto& child : frames) {
      if (child.type != pinocchio::BODY) {
        continue;
      }
      const auto& joint = frames[child.parentFrame];
      if (joint.type != pinocchio::JOINT) {
        continue;
      }
      joints_.push_back(
          {joint.name, frames[joint.parentFrame].name, child.name});
    }
  }

  Eigen::VectorXd arm() const { return q_(arm_); }
  void set_arm(const Eigen::VectorXd& q_arm) { q_(arm_) = q_arm; }

  // Animates the URDF that `open_cell` logged by updating the transform between
  // each joint's links.
  void log(const rerun::RecordingStream& rec) {
    for (const auto& j : joints_) {
      const Eigen::Matrix4d tf =
          scene_->forwardKinematics(data_, q_, j.child, j.parent);
      const Eigen::Matrix3f r = tf.topLeftCorner<3, 3>().cast<float>();
      // NOLINTNEXTLINE(modernize-avoid-c-arrays)
      const rerun::Vec3D columns[3] = {{r(0, 0), r(1, 0), r(2, 0)},
                                       {r(0, 1), r(1, 1), r(2, 1)},
                                       {r(0, 2), r(1, 2), r(2, 2)}};
      const auto p = to_position(tf);
      rec.log("world/robot/joints/" + j.name,
              rerun::Transform3D::from_translation_mat3x3({p.x(), p.y(), p.z()},
                                                          columns)
                  .with_parent_frame(j.parent)
                  .with_child_frame(j.child));
      rec.log(
          "joints/" + j.name,
          rerun::Scalars(q_(scene_->getModel()
                                .joints[scene_->getModel().getJointId(j.name)]
                                .idx_q())));
    }
  }

 private:
  struct JointFrames {
    std::string name, parent, child;
  };

  std::shared_ptr<roboplan::Scene> scene_;
  pinocchio::Data data_;
  Eigen::VectorXi arm_;
  Eigen::VectorXd q_;
  std::vector<JointFrames> joints_;
};

// Used only by the offloaded planning job and the TOPP-RA leaf, never at the
// same time.
struct Planner {
  std::shared_ptr<roboplan::Scene> scene;
  roboplan::RRT rrt;
  roboplan::PathShortcutter shortcutter;
  roboplan::PathParameterizerTOPPRA toppra;
};

struct Cell {
  Config config;
  Trajectories trajectories;
  std::shared_ptr<Robot> robot;
  std::shared_ptr<Planner> planner;
};

struct PlanRequest {
  Cell cell;
  Eigen::VectorXd start;
  Eigen::VectorXd goal;
};

struct Planned {
  Cell cell;
  JointPath path;
  std::vector<rerun::Position3D> tree_points;
};

void log_event(const Config& config, const std::string& text) {
  config.rec->log("log", rerun::TextLog(text));
}

Cell open_cell(Config config) {
  const auto share = roboplan::example_models::get_package_share_dir();
  const auto models =
      share / "roboplan_example_models" / "models" / "ur_robot_model";
  auto scene = std::make_shared<roboplan::Scene>(
      "ur5",
      roboplan::loadUrdfSceneDescription(models / "ur5_gripper.urdf", {share}));
  scene->importJointLimitsFromConfig(
      roboplan::loadJointLimitsConfig(models / "ur5_config.yaml"));
  if (auto ok = scene->importSrdf(
          roboplan::loadTextFile(models / "ur5_gripper.srdf"));
      !ok) {
    throw std::runtime_error(ok.error());
  }

  // The floor's top sits just below the base, so the base never touches it. It
  // is deeper than the arm's reach, so no configuration can hide entirely
  // beneath it.
  constexpr double floor_size = 2.0;
  constexpr double floor_thickness = 1.0;
  constexpr double floor_top = -0.005;
  Eigen::Matrix4d floor_tf = Eigen::Matrix4d::Identity();
  floor_tf(2, 3) = floor_top - (floor_thickness / 2);
  if (auto ok = scene->addBoxGeometry(
          "floor", "world",
          roboplan::Box(floor_size, floor_size, floor_thickness), floor_tf,
          Eigen::Vector4d(0.6, 0.6, 0.6, 1.0));
      !ok) {
    throw std::runtime_error(ok.error());
  }
  scene->setRngSeed(config.seed);

  roboplan::RRTOptions rrt_options;
  rrt_options.group_name = group;
  rrt_options.rrt_connect = true;
  rrt_options.max_connection_distance = 1.0;
  rrt_options.max_nodes = 10000;
  rrt_options.max_planning_time = 2.0;
  roboplan::PathShortcuttingOptions shortcut_options;
  shortcut_options.group_name = group;
  shortcut_options.max_step_size = rrt_options.collision_check_step_size;
  auto planner = std::make_shared<Planner>(Planner{
      .scene = scene,
      .rrt = roboplan::RRT(scene, rrt_options),
      .shortcutter = roboplan::PathShortcutter(scene, shortcut_options),
      .toppra = roboplan::PathParameterizerTOPPRA(scene, std::string(group))});
  planner->rrt.setRngSeed(config.seed);

  // Rerun resolves the URDF's package:// mesh URIs through ROS_PACKAGE_PATH.
  setenv("ROS_PACKAGE_PATH", share.c_str(), 1);  // NOLINT(misc-include-cleaner)
  config.rec->log_file_from_path(models / "ur5_gripper.urdf", "world/robot",
                                 true);
  const auto half = static_cast<float>(floor_size / 2);
  config.rec->log_static(
      "world/floor",
      rerun::Boxes3D::from_centers_and_half_sizes(
          {{0.0F, 0.0F, static_cast<float>(floor_tf(2, 3))}},
          {{half, half, static_cast<float>(floor_thickness / 2)}})
          .with_colors({rerun::Color(150, 150, 150)})
          .with_fill_mode(rerun::components::FillMode::Solid),
      rerun::CoordinateFrame("world"));
  for (const char* entity :
       {"world/plan/goal", "world/plan/ee_path", "world/plan/rrt_nodes"}) {
    config.rec->log_static(entity, rerun::CoordinateFrame("world"));
  }

  using std::numbers::pi;
  // Random start poses can land in pockets that are collision-free but walled
  // off by the floor and joint limits.
  Eigen::VectorXd home_arm(6);
  home_arm << 0.0, -pi / 2, pi / 2, -pi / 2, -pi / 2, 0.0;
  const Eigen::VectorXd home =
      scene->toFullJointPositions(std::string(group), home_arm);
  if (scene->hasCollisions(home)) {
    throw std::runtime_error("home pose is in collision");
  }
  auto robot = std::make_shared<Robot>(scene, home);
  return Cell{.config = std::move(config),
              .trajectories = Trajectories{},
              .robot = std::move(robot),
              .planner = std::move(planner)};
}

Result<PlanRequest, PlanningFailed> choose_goal(Cell cell) {
  auto& scene = *cell.planner->scene;
  const auto goal = scene.randomCollisionFreePositions();
  if (!goal) {
    return beet::make_unexpected(PlanningFailed{"no collision-free goal"});
  }
  cell.config.rec->log("world/plan/goal",
                       rerun::Points3D({to_position(scene.forwardKinematics(
                                           *goal, std::string(tool)))})
                           .with_radii({0.03F})
                           .with_colors({rerun::Color(240, 190, 40)}));
  const Eigen::VectorXi arm =
      scene.getJointGroupInfo(std::string(group))->q_indices;
  Eigen::VectorXd start = cell.robot->arm();
  return PlanRequest{
      .cell = std::move(cell), .start = std::move(start), .goal = (*goal)(arm)};
}

// Runs on its own thread, so it computes forward kinematics with private
// pinocchio data.
Result<Planned, PlanningFailed> plan_rrt(PlanRequest req) {
  auto& planner = *req.cell.planner;
  roboplan::JointConfiguration start;
  roboplan::JointConfiguration goal;
  start.positions = req.start;
  goal.positions = req.goal;
  auto path = planner.rrt.plan(start, goal);
  if (!path) {
    return beet::make_unexpected(PlanningFailed{path.error()});
  }

  pinocchio::Data data(planner.scene->getModel());
  std::vector<rerun::Position3D> tree_points;
  const auto [start_nodes, goal_nodes] = planner.rrt.getNodes();
  for (const auto* nodes : {&start_nodes, &goal_nodes}) {
    for (const auto& n : *nodes) {
      tree_points.push_back(to_position(
          planner.scene->forwardKinematics(data, n.config, std::string(tool))));
    }
  }
  return Planned{.cell = std::move(req.cell),
                 .path = planner.shortcutter.shortcut(*path),
                 .tree_points = std::move(tree_points)};
}

Result<beet::Call<JointTrajectory, Result<ExecutionReport, StartMismatch>>,
       ParameterizationFailed>
parameterize(Planned planned) {  // NOLINT(performance-unnecessary-value-param)
  const Config& config = planned.cell.config;
  auto& planner = *planned.cell.planner;
  roboplan::TOPPRAOptions options;
  options.dt = control_dt;
  auto traj = planner.toppra.generate(planned.path, options);
  if (!traj) {
    return beet::make_unexpected(ParameterizationFailed{traj.error()});
  }

  std::vector<rerun::Position3D> ee_path;
  for (const auto& q : traj->positions) {
    ee_path.push_back(to_position(planner.scene->forwardKinematics(
        planner.scene->toFullJointPositions(std::string(group), q),
        std::string(tool))));
  }
  config.rec->log("world/plan/ee_path",
                  rerun::LineStrips3D({rerun::components::LineStrip3D(ee_path)})
                      .with_colors({rerun::Color(80, 160, 240)}));
  config.rec->log("world/plan/rrt_nodes",
                  rerun::Points3D(planned.tree_points).with_radii({0.008F}));
  log_event(config, "planned " + std::to_string(planned.path.positions.size()) +
                        " waypoints, " + std::to_string(traj->times.back()) +
                        " s trajectory");
  return beet::Call<JointTrajectory, Result<ExecutionReport, StartMismatch>>{
      .channel = planned.cell.trajectories, .request = std::move(*traj)};
}

// Serves trajectories forever: one trajectory point per tick, since TOPP-RA
// samples at the control rate.
Task<Result<beet::never, beet::never>> controller(Cell cell) {
  std::optional<Trajectories::Request> active;
  std::size_t step = 0;
  for (;;) {
    if (auto req = cell.trajectories.try_receive()) {
      const double distance =
          (req->value().positions.front() - cell.robot->arm()).norm();
      if (distance > 1e-3) {
        req->reply(beet::make_unexpected(StartMismatch{distance}));
      } else {
        active = std::move(req);
        step = 0;
      }
    }
    if (active && active->cancelled()) {
      log_event(cell.config, "trajectory cancelled, holding position");
      active.reset();
    }
    if (active) {
      const auto& traj = active->value();
      cell.robot->set_arm(traj.positions[step]);
      if (++step == traj.positions.size()) {
        log_event(cell.config, "trajectory done");
        active->reply(ExecutionReport{.duration = traj.times.back(),
                                      .points = traj.positions.size()});
        active.reset();
      }
    }
    cell.robot->log(*cell.config.rec);
    co_await beet::running;
  }
}

// Recover handlers only see the error, so the stream they log to is captured
// when the tree is built.
auto make_tree(const std::shared_ptr<const rerun::RecordingStream>& rec) {
  auto skip_goal = [rec](const auto& e) {
    rec->log("log", rerun::TextLog("skipping goal: " + describe(e))
                        .with_level(rerun::TextLogLevel::Warning));
    return ExecutionReport{};
  };

  return beet::sequence(
      beet::named<"open cell">(open_cell),
      beet::parallel_any(
          beet::named<"missions">(beet::rate<planning_rate>(beet::repeat<3>(
              beet::recover<PlanningFailed, ParameterizationFailed,
                            StartMismatch>(
                  beet::sequence(beet::named<"choose goal">(choose_goal),
                                 beet::named<"rrt">(beet::offload(plan_rrt)),
                                 beet::named<"toppra">(parameterize),
                                 beet::named<"execute">(Trajectories::call())),
                  skip_goal)))),
          beet::named<"controller">(beet::rate<control_rate>(controller))));
}

// Stamps each tick with Rerun time before the tree logs anything, and draws
// the tree's status once the tick ends.
template <class Tree>
auto tick_logger(const rerun::RecordingStream& rec,
                 const rerun_tree::GraphLogger<Tree>& graph,
                 beet::StatusTable<Tree>& table) {
  auto status = table.listeners();
  return beet::Listeners{
      .on_tick_begin =
          [&rec, begin = status.on_tick_begin](std::uint64_t tick) {
            rec.set_time_sequence("tick", static_cast<std::int64_t>(tick));
            rec.set_time_duration_secs("sim_time",
                                       static_cast<double>(tick) * control_dt);
            begin(tick);
          },
      .on_tick_end = [&graph, &table](
                         std::uint64_t /*unused*/,
                         beet::Status /*unused*/) { graph.log(table); },
      .on_start = status.on_start,
      .on_finish = status.on_finish,
      .on_halt = status.on_halt};
}

}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char** argv) {
  std::optional<std::string> save;
  Config config;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string_view flag = argv[i];
    if (flag == "--save") {
      save = argv[i + 1];
    }
    if (flag == "--seed") {
      config.seed = static_cast<unsigned>(std::stoul(argv[i + 1]));
    }
  }

  auto rec = std::make_shared<rerun::RecordingStream>("beet_roboplan_ur5");
  if (save) {
    rec->save(*save).exit_on_failure();
  } else {
    rec->spawn().exit_on_failure();
  }
  config.rec = rec;

  const auto tree = make_tree(rec);
  using Tree = std::remove_cvref_t<decltype(tree)>;
  static_assert(beet::rate_v<Tree> == control_rate);
  const rerun_tree::GraphLogger<Tree> graph(*rec, "tree");
  beet::StatusTable<Tree> table;
  beet::Runner runner{tree, config, tick_logger(*rec, graph, table)};
  runner.run();
}
