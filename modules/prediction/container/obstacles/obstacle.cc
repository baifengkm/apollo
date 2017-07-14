/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/prediction/container/obstacles/obstacle.h"

#include <iomanip>
#include <cmath>

#include "modules/common/log.h"
#include "modules/common/math/math_utils.h"
#include "modules/prediction/common/prediction_gflags.h"

namespace apollo {
namespace prediction {

using apollo::perception::PerceptionObstacle;
using apollo::common::math::KalmanFilter;
using apollo::common::ErrorCode;
using apollo::common::Point3D;

std::mutex Obstacle::mutex_;

namespace {

double Damp(const double x, const double sigma) {
    return 1 / (1 + exp(1 / (std::fabs(x) + sigma)));
}

}  // namespace

Obstacle::Obstacle() : 
    id_(-1),
    type_(PerceptionObstacle::UNKNOWN_MOVABLE),
    feature_history_(0),
    kf_motion_tracker_(),
    is_motion_tracker_enabled_(false),
    kf_lane_tracker_map_(0) {

}

Obstacle::~Obstacle() {
  id_ = -1;
  type_ = PerceptionObstacle::UNKNOWN_UNMOVABLE;
  feature_history_.clear();
  is_motion_tracker_enabled_ = false;
  kf_lane_tracker_map_.clear();
  // TODO(author) current_lanes_.clear();
}

int Obstacle::id() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return id_;
}

double Obstacle::timestamp() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (feature_history_.size() > 0) {
    return feature_history_.front().timestamp();
  } else {
    return 0.0;
  }
}

const Feature& Obstacle::feature(size_t i) {
  std::lock_guard<std::mutex> lock(mutex_);
  CHECK(i < feature_history_.size());
  return feature_history_[i];
}

Feature* Obstacle::mutable_feature(size_t i) {
  std::lock_guard<std::mutex> lock(mutex_);
  CHECK(i < feature_history_.size());
  return &feature_history_[i];
}

const Feature& Obstacle::latest_feature() {
  std::lock_guard<std::mutex> lock(mutex_);
  CHECK(feature_history_.size() > 0);
  return feature_history_.front();
}

Feature* Obstacle::mutable_latest_feature() {
  std::lock_guard<std::mutex> lock(mutex_);

  CHECK(feature_history_.size() > 0);
  return &(feature_history_.front());
}

size_t Obstacle::history_size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return feature_history_.size();
}

const KalmanFilter<double, 4, 2, 0>& Obstacle::kf_lane_tracker(
      const std::string& lane_id) {
    CHECK(kf_lane_tracker_map_.find(lane_id) != kf_lane_tracker_map_.end());
    return kf_lane_tracker_map_[lane_id];
}

void Obstacle::Insert(const PerceptionObstacle& perception_obstacle,
                      const double timestamp) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (feature_history_.size() > 0 &&
      timestamp <= feature_history_.front().timestamp()) {
    AINFO << "Obstacle [" << id_ << "] received an older frame ["
          << timestamp << "] than the most recent timestamp [ "
          << feature_history_.front().timestamp() << "].";
    return;
  }

  Feature feature;
  if (SetId(perception_obstacle, &feature) == ErrorCode::PREDICTION_ERROR) {
    return;
  }
  if (SetType(perception_obstacle) == ErrorCode::PREDICTION_ERROR) {
    return;
  }
  SetTimestamp(perception_obstacle, timestamp, &feature);
  SetPosition(perception_obstacle, &feature);
  SetVelocity(perception_obstacle, &feature);
  SetAcceleration(&feature);
  SetTheta(perception_obstacle, &feature);
}

ErrorCode Obstacle::SetId(const PerceptionObstacle& perception_obstacle,
                          Feature* feature) {
  if (!perception_obstacle.has_id()) {
    AERROR << "Obstacle has no ID.";
    return ErrorCode::PREDICTION_ERROR;
  }

  int id = perception_obstacle.id();
  if (id_ < 0) {
    id_ = id;
    AINFO << "Obstacle set id [" << id_ << "].";
  } else {
    if (id_ != id) {
      AERROR << "Obstacle [" << id_ << "] has a mismatched ID [" << id
             << "] from perception obstacle.";
      return ErrorCode::PREDICTION_ERROR;
    } else {
      feature->set_id(id);
    }
  }
  return ErrorCode::OK;
}

ErrorCode Obstacle::SetType(const PerceptionObstacle& perception_obstacle) {
  if (perception_obstacle.has_type()) {
    type_ = perception_obstacle.type();
    AINFO << "Obstacle [" << id_ << "] set type [" << type_ << "].";
  } else {
    AERROR << "Obstacle [" << id_ << "] has no type.";
    return ErrorCode::PREDICTION_ERROR;
  }
  return ErrorCode::OK;
}

void Obstacle::SetTimestamp(const PerceptionObstacle& perception_obstacle,
                            const double timestamp, Feature* feature) {
  double ts = timestamp;
  if (perception_obstacle.has_timestamp() &&
      perception_obstacle.timestamp() > 0.0) {
    ts = perception_obstacle.timestamp();
  }
  feature->set_timestamp(ts);

  AINFO << "Obstacle [" << id_ << "] set timestamp [" << std::fixed
        << std::setprecision(6) << ts << "].";
}


void Obstacle::SetPosition(const PerceptionObstacle& perception_obstacle,
                           Feature* feature) {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;

  if (perception_obstacle.has_position()) {
    if (perception_obstacle.position().has_x()) {
      x = perception_obstacle.position().x();
    }
    if (perception_obstacle.position().has_y()) {
      y = perception_obstacle.position().y();
    }
    if (perception_obstacle.position().has_z()) {
      z = perception_obstacle.position().z();
    }
  }

  feature->mutable_position()->set_x(x);
  feature->mutable_position()->set_y(y);
  feature->mutable_position()->set_z(z);

  AINFO << "Obstacle [" << id_ << "] set position [" << std::fixed
        << std::setprecision(6) << x << ", " << std::fixed
        << std::setprecision(6) << y << ", " << std::fixed
        << std::setprecision(6) << z << "].";
}

void Obstacle::SetVelocity(const PerceptionObstacle& perception_obstacle,
                           Feature* feature) {
  double velocity_x = 0.0;
  double velocity_y = 0.0;
  double velocity_z = 0.0;

  if (perception_obstacle.has_velocity()) {
    if (perception_obstacle.velocity().has_x()) {
      velocity_x = perception_obstacle.velocity().x();
    }
    if (perception_obstacle.velocity().has_y()) {
      velocity_y = perception_obstacle.velocity().y();
    }
    if (perception_obstacle.velocity().has_z()) {
      velocity_z = perception_obstacle.velocity().z();
    }
  }

  feature->mutable_velocity()->set_x(velocity_x);
  feature->mutable_velocity()->set_y(velocity_y);
  feature->mutable_velocity()->set_z(velocity_z);

  double speed = std::hypot(std::hypot(velocity_x, velocity_y), velocity_z);
  double velocity_heading = std::atan2(velocity_y, velocity_x);
  feature->set_velocity_heading(velocity_heading);
  feature->set_speed(speed);

  AINFO << "Obstacle [" << id_ << "] set velocity [" << std::fixed
        << std::setprecision(6) << velocity_x << ", " << std::fixed
        << std::setprecision(6) << velocity_y << ", " << std::fixed
        << std::setprecision(6) << velocity_z << "], "
        << "velocity heading [" << velocity_heading << "] and speed [" << speed
        << "].";
}

void Obstacle::SetAcceleration(Feature* feature) {
  double acc_x = 0.0;
  double acc_y = 0.0;
  double acc_z = 0.0;

  if (feature_history_.size() > 0) {
    double curr_ts = feature->timestamp();
    double prev_ts = feature_history_.front().timestamp();

    const Point3D& curr_velocity = feature->velocity();
    const Point3D& prev_velocity = feature_history_.front().velocity();

    if (apollo::common::math::DoubleCompare(curr_ts, prev_ts) == 1) {
      double damping_x = Damp(curr_velocity.x(), 0.001);
      double damping_y = Damp(curr_velocity.y(), 0.001);
      double damping_z = Damp(curr_velocity.z(), 0.001);

      acc_x = (curr_velocity.x() - prev_velocity.x()) / (curr_ts - prev_ts);
      acc_y = (curr_velocity.y() - prev_velocity.y()) / (curr_ts - prev_ts);
      acc_z = (curr_velocity.z() - prev_velocity.z()) / (curr_ts - prev_ts);

      acc_x = apollo::common::math::Clamp(
          acc_x * damping_x, FLAGS_min_acc, FLAGS_max_acc);
      acc_y = apollo::common::math::Clamp(
          acc_y * damping_y, FLAGS_min_acc, FLAGS_max_acc);
      acc_z = apollo::common::math::Clamp(
          acc_z * damping_z, FLAGS_min_acc, FLAGS_max_acc);
    }
  }

  feature->mutable_acceleration()->set_x(acc_x);
  feature->mutable_acceleration()->set_y(acc_y);
  feature->mutable_acceleration()->set_z(acc_z);
  double acc = std::hypot(std::hypot(acc_x, acc_y), acc_z);
  feature->set_acc(acc);

  LOG(INFO) << "Obstacle [" << id_ << "] set acc [" << std::fixed
            << std::setprecision(6) << acc_x << ", " << std::fixed
            << std::setprecision(6) << acc_y << ", " << std::fixed
            << std::setprecision(6) << acc_z << "], "
            << "and acc [" << acc << "].";
}


void Obstacle::SetTheta(const PerceptionObstacle& perception_obstacle,
                        Feature* feature) {
  double theta = 0.0;
  if (perception_obstacle.has_theta()) {
    theta = perception_obstacle.theta();
  }
  feature->set_theta(theta);

  AINFO << "Obstacle [" << id_ << "] set theta [" << std::fixed
        << std::setprecision(6) << theta << "].";
}

}  // namespace prediction
}  // namespace apollo
