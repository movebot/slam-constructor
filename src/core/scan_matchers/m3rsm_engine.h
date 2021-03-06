#ifndef SLAM_CTOR_CORE_M3RSM_ENGINE_H
#define SLAM_CTOR_CORE_M3RSM_ENGINE_H

#include <cmath>
#include <memory>
#include <queue>
#include <iostream>
#include <cstdlib>
#include <set>

#include "../geometry_primitives.h"
#include "grid_scan_matcher.h"
#include "../maps/rescalable_caching_grid_map.h"

// Many-to-Many Multiple Resolution Scan Matching Engine based on Olson (2015).

template <typename BackGridMap>
class M3RSMRescalableGridMap : public RescalableCachingGridMap<BackGridMap> {
private:
  using AreaId = typename RescalableCachingGridMap<BackGridMap>::AreaId;
public:
  M3RSMRescalableGridMap(std::shared_ptr<ObservationImpactEstimator> oie,
                         std::shared_ptr<GridCell> prototype,
                         const GridMapParams& params = MapValues::gmp)
    : RescalableCachingGridMap<BackGridMap>{prototype, params}
    , _oie{oie} {
    assert(_oie.get() && "OIE is null");
  }

public:
  using RescalableCachingGridMap<BackGridMap>::scale_id;
  using RescalableCachingGridMap<BackGridMap>::finest_scale_id;
  using RescalableCachingGridMap<BackGridMap>::coarsest_scale_id;

public:

  bool validate() const override {
    const auto &finest_map = map(finest_scale_id());
    for (auto id = finest_scale_id() + 1; id <= coarsest_scale_id(); ++id) {
      const auto &coarser_map = map(id);
      auto raw_crd = DiscretePoint2D{0, 0};
      for (raw_crd.x = 0; raw_crd.x < coarser_map.width(); ++raw_crd.x) {
        for (raw_crd.y = 0; raw_crd.y < coarser_map.height(); ++raw_crd.y) {
          auto area_id = coarser_map.internal2external(raw_crd);
          auto area = coarser_map.world_cell_bounds(area_id);

          auto max_f_impact = estimate_max_spe_impact(finest_map, area);
          auto c_impact = estimate_spe_obstacle_impact(coarser_map[area_id]);
          // NB: use le, since probability of coarser areas are not descreased
          //     is the current impleentation (see post_area_update)
          if (!less_or_equal(max_f_impact, c_impact)) {
            std::cout << max_f_impact << " " << c_impact << std::endl;
            return false;
          }
        }
      }
    }
    return true;
  }

protected:
  using RescalableCachingGridMap<BackGridMap>::active_map;
  using RescalableCachingGridMap<BackGridMap>::map;

  // estimates impact of the obstacle observation inside the area
  // to scan probability. Additive model is assumed.
  double estimate_spe_obstacle_impact(const GridCell &area) const {
    return _oie->estimate_obstacle_impact(area);
  }

  double estimate_max_spe_impact(const GridMap &map,
                                 const Rectangle &area) const {
    double max = std::numeric_limits<double>::lowest();
    for (auto &a_id : GridRasterizedRectangle{map, area, false}.to_vector()) {
      if (map[a_id].is_unknown()) { continue; }
      max = std::max(estimate_spe_obstacle_impact(map[a_id]), max);
    }

    if (are_equal(max, std::numeric_limits<double>::lowest())) {
      max = estimate_spe_obstacle_impact(*GridMap::cell_prototype());
    }
    return max;
  }

  void pre_area_update(const AreaId &area_id) {
    _prev_fine_impact = estimate_spe_obstacle_impact(active_map()[area_id]); 
  }

  void post_area_update(const AreaId &area_id) {
    // TODO: update if a "non-finest" cell is updated?
    assert(scale_id() == finest_scale_id());
    const auto &modified_area = active_map()[area_id];
    auto modified_space = active_map().world_cell_bounds(area_id);
    auto fine_impact = estimate_spe_obstacle_impact(modified_area);

    // TODO: implement coarcer maps update on impact decrease
    // assert(less_or_equal(_prev_fine_impact, fine_impact));
    update_coarser_maps(area_id, fine_impact);
  }

  void update_coarser_maps(const AreaId &area_id, const double impact) {
    const auto &modified_area = active_map()[area_id];
    auto modified_space = active_map().world_cell_bounds(area_id);

    for (unsigned coarser_scale_id = scale_id() + 1;
         coarser_scale_id <= coarsest_scale_id(); ++coarser_scale_id) {
      auto& coarser_map = map(coarser_scale_id);
      bool coarser_area_is_updated = false;
      using GRRectangle = GridRasterizedRectangle;
      auto cm_coords = GRRectangle{coarser_map, modified_space, false};
      while (cm_coords.has_next()) {
        auto coord = cm_coords.next();
        auto &coarser_area = coarser_map[coord];
        auto coarser_impact = estimate_spe_obstacle_impact(coarser_area);
        // TODO: move "propagate max" to the strategy if required.
        auto coarser_update_is_not_required =
          !coarser_area.is_unknown() &&
          less_or_equal(impact, coarser_impact);
        if (coarser_update_is_not_required) { continue; }

        coarser_map.reset(coord, modified_area);
        coarser_area_is_updated = true;
      }
      if (!coarser_area_is_updated) { break; }
    }
  }

private:
  std::shared_ptr<ObservationImpactEstimator> _oie;
  double _prev_fine_impact;
};

struct Match {
private: // types
  using SPEParams = ScanProbabilityEstimator::SPEParams;
  using SPEPtr = std::shared_ptr<ScanProbabilityEstimator>;
  using Rect = decltype(SPEParams{}.sp_analysis_area);
public:
  // result
  double prob_upper_bound; // max scan probability given the assumptions below
  // assumptions
  double rotation;
  Rect translation_drift;
  // args
  SPEPtr spe;
  std::shared_ptr<LaserScan2D> filtered_scan;
  bool scan_is_prerotated;
  const RobotPose *pose; // owner - user of the engine
  GridMap *map;          // owner - user of the engine

  static Match invalid_match() {
    static auto invalid_match = Match{std::numeric_limits<double>::quiet_NaN()};
    return invalid_match;
  }

  Match(double rot, const Rect &tdrift, SPEPtr scan_prob_est,
        std::shared_ptr<LaserScan2D> filtered_lscan, bool is_rotated,
        const RobotPose &robot_pose, GridMap &grid_map, bool is_root = true)
      : rotation{rot}, translation_drift{tdrift}, spe{scan_prob_est}
      , filtered_scan{filtered_lscan}, scan_is_prerotated{is_rotated}
      , pose{&robot_pose}, map{&grid_map}
      , _abs_rotation{std::abs(rotation)}
      , _drift_amount{tdrift.hside_len() + tdrift.vside_len()} {

    Point2D avg_drift = translation_drift.center();

    auto drifted_pose = *pose;
    if (scan_is_prerotated) {
      drifted_pose = {pose->x + avg_drift.x, pose->y + avg_drift.y, 0};
    } else {
      drifted_pose += RobotPoseDelta{avg_drift.x, avg_drift.y, rotation};
    }
    if (scan_is_prerotated && is_root) { filter_prerotated_scan(); }
    auto target_scale = std::max(translation_drift.vside_len(),
                                 translation_drift.hside_len());
    map->rescale(target_scale);
    prob_upper_bound = spe->estimate_scan_probability(
       *filtered_scan, drifted_pose, *map,
       SPEParams{translation_drift, scan_is_prerotated});
  }

  Match(const Rect &tdrift, const Match &that)
      : Match(that.rotation, tdrift, that.spe, that.filtered_scan,
              that.scan_is_prerotated, *that.pose, *that.map, false) {}

  bool is_valid() const { return !std::isnan(prob_upper_bound); }
  double is_finest(double threashold = 0) const {
    return _drift_amount <= threashold;
  }

  // returns true if the matching is _less_ prepefable than a given one
  bool operator<(const Match &that) const {
    if (!are_equal(prob_upper_bound, that.prob_upper_bound)) {
      // greater is "better" -> correctness
      return less(prob_upper_bound, that.prob_upper_bound);
    }
    if (!are_equal(_drift_amount, that._drift_amount)) {
      // finer is "better" -> speed up
      return _drift_amount > that._drift_amount;
    }
    // smaller is "better" -> fixes "blindness" of SPE
    return _abs_rotation > that._abs_rotation;
  }

private: // methods
  Match(double prob) : prob_upper_bound{prob}  {}

  void filter_prerotated_scan() {
    return;
    /*
    GridMap::Coord curr_area_id;
    auto points_in_area = std::vector<ScanPoint2D>{};

    map->rescale(0);
    auto scan = filtered_scan;
    auto filtered_pts = std::vector<ScanPoint2D>{};
    for (const auto &sp : scan->points()) {
      auto world_point = sp.move_origin(pose->x, pose->y);
      auto area_id = map->world_to_cell(world_point);

      if (points_in_area.size() == 0 || area_id == curr_area_id) {
        curr_area_id = area_id;
        points_in_area.push_back(sp);
      } else {
        auto effective_sp = points_in_area[0].set_factor(points_in_area.size());
        filtered_pts.push_back(effective_sp);
        //std::cout << points_in_area.size() << std::endl;
        curr_area_id = area_id;
        points_in_area.clear();
      }
    }
    if (points_in_area.size()) {
      auto effective_sp = points_in_area[0].set_factor(points_in_area.size());
      filtered_pts.push_back(effective_sp);
    }

    scan->points() = std::move(filtered_pts);
    */
  }

private: // fields
  double _abs_rotation, _drift_amount;
};

std::ostream& operator<<(std::ostream &os, const Match &match) {
  return os << "[" << rad2deg(match.rotation) << "]"
            << " + " << match.translation_drift
            << " -> " << match.prob_upper_bound;
}


class M3RSMEngine {
private: // types
  using Matches = std::priority_queue<Match>;
  using SPEParams = ScanProbabilityEstimator::SPEParams;
public: // types
  using Rect = decltype(SPEParams{}.sp_analysis_area);
public:

  M3RSMEngine(double max_finest_prob_diff = 0)
    : _max_finest_prob_diff{max_finest_prob_diff} {
    reset_engine_state();
  }

  void reset_engine_state() {
    _matches = Matches{};
    _best_finest_probability = 0.0;
  }

  void set_translation_lookup_range(double max_x_error, double max_y_error) {
    _max_x_error = max_x_error;
    _max_y_error = max_y_error;
  }

  void set_rotation_lookup_range(double sector, double step) {
    _rotation_sector = sector;
    _rotation_resolution = step;
  }

  void add_match(Match&& match) {
    double match_prob = match.prob_upper_bound;
    if (match_prob < _best_finest_probability) { return; }

    if (match.is_finest()) {
      _best_finest_probability = std::max(_best_finest_probability,
                                          match_prob - _max_finest_prob_diff);
    }
    _matches.push(std::move(match));
  }

  void add_scan_matching_request(std::shared_ptr<ScanProbabilityEstimator> spe,
                                 const RobotPose &pose,
                                 const LaserScan2D &raw_scan, GridMap &map,
                                 bool prerotate_scan = false) {
    // generate pose translation ranges to be checked
    const auto empty_trs_range = Rect{0, 0, 0, 0};
    const auto entire_trs_range = Rect{-_max_y_error, _max_y_error,
                                       -_max_x_error, _max_x_error};

    double rotation_drift = 0;
    auto fscan = std::make_shared<LaserScan2D>(spe->filter_scan(raw_scan, pose,
                                                                map));
    auto rotation_resolution = _rotation_resolution;
    while (less_or_equal(2 * rotation_drift, _rotation_sector)) {
      for (auto &rot : std::set<double>{rotation_drift, -rotation_drift}) {
        auto scan = prerotate_scan ?
            std::make_shared<LaserScan2D>(fscan->to_cartesian(rot + pose.theta))
          : fscan;
        add_match(Match{rot, empty_trs_range, spe,
                        scan, prerotate_scan, pose, map});
        add_match(Match{rot, entire_trs_range, spe,
                        scan, prerotate_scan, pose, map});
      }
      rotation_drift += rotation_resolution;
    }
  }

  Match next_best_match(double translation_step) {
    while (!_matches.empty()) {
      auto best_match = _matches.top();
      _matches.pop();
      auto coarse_drift = best_match.translation_drift;
      auto should_branch_horz = less(translation_step,
                                     coarse_drift.hside_len());
      auto should_branch_vert = less(translation_step,
                                     coarse_drift.vside_len());
      if (!should_branch_horz && !should_branch_vert) {
        return best_match;
      }
      branch(best_match, should_branch_horz, should_branch_vert);
    }
    return Match::invalid_match();
  }

private:

  void branch(const Match &coarse_match, bool is_horz, bool is_vert) {
    auto coarse_drift = coarse_match.translation_drift;
    auto finer_drifts = std::vector<Rect>{};

    if (is_horz && is_vert) {
      finer_drifts = coarse_drift.split4_evenly();
    } else if (is_horz) {
      finer_drifts = coarse_drift.split_horz();
    } else if (is_vert) {
      finer_drifts = coarse_drift.split_vert();
    }

    // update matches with finer ones
    for (auto& finer_drift : finer_drifts) {
      auto finer_match = Match{finer_drift, coarse_match};
      assert(less_or_equal(finer_match.prob_upper_bound,
                           coarse_match.prob_upper_bound, 1e-5) &&
             "BUG: Bounding assumption is violated");
      add_match(std::move(finer_match));
    }
  }

private:
  double _max_finest_prob_diff;
  std::priority_queue<Match> _matches;
  double _best_finest_probability;
  double _max_x_error, _max_y_error;
  double _rotation_sector, _rotation_resolution;
};

#endif
