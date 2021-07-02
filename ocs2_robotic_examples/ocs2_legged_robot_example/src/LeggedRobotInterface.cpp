/******************************************************************************
Copyright (c) 2021, Farbod Farshidian. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

 * Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include <pinocchio/fwd.hpp>  // forward declarations must be included first.

#include "ocs2_legged_robot_example/LeggedRobotInterface.h"

#include "ocs2_legged_robot_example/LeggedRobotPreComputation.h"
#include "ocs2_legged_robot_example/LeggedRobotRefernceUpdate.h"
#include "ocs2_legged_robot_example/constraint/FrictionConeConstraint.h"
#include "ocs2_legged_robot_example/constraint/NormalVelocityConstraintCppAd.h"
#include "ocs2_legged_robot_example/constraint/ZeroForceConstraint.h"
#include "ocs2_legged_robot_example/constraint/ZeroVelocityConstraintCppAd.h"
#include "ocs2_legged_robot_example/cost/LeggedRobotStateInputQuadraticCost.h"
#include "ocs2_legged_robot_example/dynamics/LeggedRobotDynamicsAD.h"

#include <ocs2_core/soft_constraint/StateInputSoftConstraint.h>
#include <ocs2_core/soft_constraint/penalties/RelaxedBarrierPenalty.h>

#include <ocs2_centroidal_model/AccessHelperFunctions.h>
#include <ocs2_centroidal_model/ModelHelperFunctions.h>

#include <ros/package.h>

#include <pinocchio/algorithm/frames.hpp>
#include <pinocchio/algorithm/jacobian.hpp>
#include <pinocchio/algorithm/kinematics.hpp>

namespace ocs2 {
namespace legged_robot {

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
LeggedRobotInterface::LeggedRobotInterface(const std::string& taskFileFolderName, const std::string& targetCommandFile,
                                           const ::urdf::ModelInterfaceSharedPtr& urdfTree) {
  // Load the task file
  const std::string taskFolder = ros::package::getPath("ocs2_legged_robot_example") + "/config/" + taskFileFolderName;
  const std::string taskFile = taskFolder + "/task.info";
  std::cerr << "Loading task file: " << taskFile << std::endl;

  boost::property_tree::ptree pt;
  boost::property_tree::read_info(taskFile, pt);
  loadData::loadPtreeValue(pt, display_, "legged_robot_interface.display", true);

  // load setting from loading file
  modelSettings_ = loadModelSettings(taskFile);
  ddpSettings_ = ddp::loadSettings(taskFile);
  mpcSettings_ = mpc::loadSettings(taskFile);
  rolloutSettings_ = rollout::loadSettings(taskFile, "rollout");

  // OptimalConrolProblem
  setupOptimalConrolProblem(taskFile, targetCommandFile, urdfTree);

  // initial state
  initialState_.setZero(centroidalModelInfo_.stateDim);
  loadData::loadEigenMatrix(taskFile, "initialState", initialState_);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::shared_ptr<GaitSchedule> LeggedRobotInterface::loadGaitSchedule(const std::string& taskFile) {
  const auto initModeSchedule = loadModeSchedule(taskFile, "initialModeSchedule", false);
  const auto defaultModeSequenceTemplate = loadModeSequenceTemplate(taskFile, "defaultModeSequenceTemplate", false);

  const auto defaultGait = [&] {
    Gait gait{};
    gait.duration = defaultModeSequenceTemplate.switchingTimes.back();
    // Events: from time -> phase
    std::for_each(defaultModeSequenceTemplate.switchingTimes.begin() + 1, defaultModeSequenceTemplate.switchingTimes.end() - 1,
                  [&](double eventTime) { gait.eventPhases.push_back(eventTime / gait.duration); });
    // Modes:
    gait.modeSequence = defaultModeSequenceTemplate.modeSequence;
    return gait;
  }();

  // display
  std::cerr << "\nInitial Modes Schedule: \n" << initModeSchedule << std::endl;
  std::cerr << "\nDefault Modes Sequence Template: \n" << defaultModeSequenceTemplate << std::endl;

  return std::make_shared<GaitSchedule>(initModeSchedule, defaultModeSequenceTemplate, modelSettings_.phaseTransitionStanceTime);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void LeggedRobotInterface::setupOptimalConrolProblem(const std::string& taskFile, const std::string& targetCommandFile,
                                                     const ::urdf::ModelInterfaceSharedPtr& urdfTree) {
  // PinocchioInterface
  pinocchioInterfacePtr_.reset(new PinocchioInterface(centroidal_model::createPinocchioInterface(urdfTree, modelSettings_.jointNames)));

  // CentroidalModelInfo
  centroidalModelInfo_ = centroidal_model::createCentroidalModelInfo(
      *pinocchioInterfacePtr_, centroidal_model::loadCentroidalType(taskFile),
      centroidal_model::loadDefaultJointState(12, targetCommandFile), modelSettings_.contactNames3DoF, modelSettings_.contactNames6DoF);

  // Swing trajectory planner
  std::unique_ptr<SwingTrajectoryPlanner> swingTrajectoryPlanner(
      new SwingTrajectoryPlanner(loadSwingTrajectorySettings(taskFile, "swing_trajectory_config"), centroidalModelInfo_));

  // Mode schedule manager
  modeScheduleManagerPtr_ =
      std::make_shared<SwitchedModelModeScheduleManager>(loadGaitSchedule(taskFile), std::move(swingTrajectoryPlanner));

  // Initialization
  constexpr bool extendNormalizedMomentum = true;
  initializerPtr_.reset(new LeggedRobotInitializer(centroidalModelInfo_, *modeScheduleManagerPtr_, extendNormalizedMomentum));

  // Dynamics
  bool useAnalyticalGradientsDynamics = false;
  loadData::loadCppDataType(taskFile, "legged_robot_interface.useAnalyticalGradientsDynamics", useAnalyticalGradientsDynamics);
  std::unique_ptr<SystemDynamicsBase> dynamicsPtr;
  if (useAnalyticalGradientsDynamics) {
    throw std::runtime_error("[LeggedRobotInterface::setupOptimalConrolProblem] The analytical dynamics class is not yet implemented.");
  } else {
    const std::string modelName = "dynamics";
    dynamicsPtr.reset(new LeggedRobotDynamicsAD(*pinocchioInterfacePtr_, centroidalModelInfo_, modelName, modelSettings_));
  }

  // Rollout
  rolloutPtr_.reset(new TimeTriggeredRollout(*dynamicsPtr, rolloutSettings_));

  // Desired trajectory reference
  referenceTrajectoryPtr_.reset(new CostDesiredTrajectories);
  referenceUpdateModulePtr_.reset(new LeggedRobotRefernceUpdate(referenceTrajectoryPtr_));
  solverModules_.push_back(referenceUpdateModulePtr_);

  // Friction cone soft constraint settings
  scalar_t frictionCoefficient = 1.0;
  scalar_t mu = 1e-2;
  scalar_t delta = 1e-3;
  boost::property_tree::ptree pt;
  boost::property_tree::read_info(taskFile, pt);
  const std::string prefix = "frictionConeSoftConstraint.";
  if (display_) {
    std::cerr << "\n #### FrictionCone Settings: ";
    std::cerr << "\n #### =============================================================================\n";
  }
  loadData::loadPtreeValue(pt, frictionCoefficient, prefix + "frictionCoefficient", display_);
  loadData::loadPtreeValue(pt, mu, prefix + "mu", display_);
  loadData::loadPtreeValue(pt, delta, prefix + "delta", display_);
  if (display_) {
    std::cerr << " #### =============================================================================" << std::endl;
  }

  // Optimal control problem
  problemPtr_.reset(new OptimalControlProblem);
  problemPtr_->costPtr->add("baseTrackingCost", getBaseTrackingCost(taskFile, centroidalModelInfo_));
  problemPtr_->dynamicsPtr = std::move(dynamicsPtr);

  bool useAnalyticalGradientsConstraints = false;
  loadData::loadCppDataType(taskFile, "legged_robot_interface.useAnalyticalGradientsConstraints", useAnalyticalGradientsConstraints);
  for (size_t i = 0; i < centroidalModelInfo_.numThreeDofContacts; i++) {
    const std::string& footName = modelSettings_.contactNames3DoF[i];

    std::unique_ptr<EndEffectorKinematics<scalar_t>> eeKinematicsPtr;
    if (useAnalyticalGradientsConstraints) {
      throw std::runtime_error(
          "[LeggedRobotInterface::setupOptimalConrolProblem] The analytical end-effector linear constraint is not implemented!");
    } else {
      const auto infoCppAd = centroidalModelInfo_.toCppAd();
      const CentroidalModelPinocchioMappingCppAd pinocchioMappingCppAd(infoCppAd);
      auto velocityUpdateCallback = [&infoCppAd](const ad_vector_t& state, PinocchioInterfaceCppAd& pinocchioInterfaceAd) {
        const ad_vector_t q = centroidal_model::getGeneralizedCoordinates(state, infoCppAd);
        updateCentroidalDynamics(pinocchioInterfaceAd, infoCppAd, q);
      };
      eeKinematicsPtr.reset(new PinocchioEndEffectorKinematicsCppAd(*pinocchioInterfacePtr_, pinocchioMappingCppAd, {footName},
                                                                    centroidalModelInfo_.stateDim, centroidalModelInfo_.inputDim,
                                                                    velocityUpdateCallback, footName, modelSettings_.modelFolderCppAd,
                                                                    modelSettings_.recompileLibrariesCppAd, modelSettings_.verboseCppAd));
    }

    problemPtr_->softConstraintPtr->add(footName + "_frictionCone", getFrictionConeConstraint(i, frictionCoefficient, mu, delta));
    problemPtr_->equalityConstraintPtr->add(footName + "_zeroForce", getZeroForceConstraint(i));
    problemPtr_->equalityConstraintPtr->add(footName + "_zeroVelocity",
                                            getZeroVelocityConstraint(*eeKinematicsPtr, i, useAnalyticalGradientsConstraints));
    problemPtr_->equalityConstraintPtr->add(footName + "_normalVelocity",
                                            getNormalVelocityConstraint(*eeKinematicsPtr, i, useAnalyticalGradientsConstraints));
  }

  // Pre-computation
  if (modelSettings_.usePreComputation) {
    problemPtr_->preComputationPtr.reset(new LeggedRobotPreComputation(
        *pinocchioInterfacePtr_, centroidalModelInfo_, *modeScheduleManagerPtr_->getSwingTrajectoryPlanner(), modelSettings_));
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::unique_ptr<MPC_DDP> LeggedRobotInterface::getMpcPtr() const {
  std::unique_ptr<MPC_DDP> mpcPtr(new MPC_DDP(mpcSettings_, ddpSettings_, *rolloutPtr_, *problemPtr_, *initializerPtr_));
  mpcPtr->getSolverPtr()->setModeScheduleManager(modeScheduleManagerPtr_);

  return mpcPtr;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void LeggedRobotInterface::initializeInputCostWeight(const std::string& taskFile, const CentroidalModelInfo& info, matrix_t& R) {
  vector_t initialState(centroidalModelInfo_.stateDim);
  loadData::loadEigenMatrix(taskFile, "initialState", initialState);

  const auto& model = pinocchioInterfacePtr_->getModel();
  auto& data = pinocchioInterfacePtr_->getData();
  const auto q = centroidal_model::getGeneralizedCoordinates(initialState, centroidalModelInfo_);
  pinocchio::computeJointJacobians(model, data, q);
  pinocchio::updateFramePlacements(model, data);

  matrix_t baseToFeetJacobians(3 * info.numThreeDofContacts, 12);
  for (size_t i = 0; i < info.numThreeDofContacts; i++) {
    matrix_t jacobianWorldToContactPointInWorldFrame = matrix_t::Zero(6, info.generalizedCoordinatesNum);
    pinocchio::getFrameJacobian(model, data, model.getBodyId(modelSettings_.contactNames3DoF[i]), pinocchio::LOCAL_WORLD_ALIGNED,
                                jacobianWorldToContactPointInWorldFrame);

    baseToFeetJacobians.block(3 * i, 0, 3, 12) = (jacobianWorldToContactPointInWorldFrame.topRows<3>()).block(0, 6, 3, 12);
  }

  const size_t totalContactDim = 3 * info.numThreeDofContacts;
  R.block(totalContactDim, totalContactDim, 12, 12) =
      (baseToFeetJacobians.transpose() * R.block(totalContactDim, totalContactDim, 12, 12) * baseToFeetJacobians).eval();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::unique_ptr<StateInputCost> LeggedRobotInterface::getBaseTrackingCost(const std::string& taskFile, const CentroidalModelInfo& info) {
  matrix_t Q(info.stateDim, info.stateDim);
  loadData::loadEigenMatrix(taskFile, "Q", Q);
  matrix_t R(info.inputDim, info.inputDim);
  loadData::loadEigenMatrix(taskFile, "R", R);

  initializeInputCostWeight(taskFile, info, R);

  if (display_) {
    std::cerr << "\n #### Base Tracking Cost Coefficients: ";
    std::cerr << "\n #### =============================================================================\n";
    std::cerr << "Q:\n" << Q << "\n";
    std::cerr << "R:\n" << R << "\n";
    std::cerr << " #### =============================================================================\n";
  }

  return std::unique_ptr<StateInputCost>(
      new LeggedRobotStateInputQuadraticCost(std::move(Q), std::move(R), info, *modeScheduleManagerPtr_));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::unique_ptr<StateInputCost> LeggedRobotInterface::getFrictionConeConstraint(size_t contactPointIndex, scalar_t frictionCoefficient,
                                                                                scalar_t mu, scalar_t delta) {
  FrictionConeConstraint::Config frictionConeConConfig(frictionCoefficient);
  std::unique_ptr<FrictionConeConstraint> frictionConeConstraintPtr(
      new FrictionConeConstraint(*modeScheduleManagerPtr_, std::move(frictionConeConConfig), contactPointIndex, centroidalModelInfo_));

  std::unique_ptr<PenaltyBase> penalty(new RelaxedBarrierPenalty({mu, delta}));

  return std::unique_ptr<StateInputCost>(new StateInputSoftConstraint(std::move(frictionConeConstraintPtr), std::move(penalty)));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::unique_ptr<StateInputConstraint> LeggedRobotInterface::getZeroForceConstraint(size_t contactPointIndex) {
  return std::unique_ptr<StateInputConstraint>(new ZeroForceConstraint(*modeScheduleManagerPtr_, contactPointIndex, centroidalModelInfo_));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::unique_ptr<StateInputConstraint> LeggedRobotInterface::getZeroVelocityConstraint(const EndEffectorKinematics<scalar_t>& eeKinematics,
                                                                                      size_t contactPointIndex,
                                                                                      bool useAnalyticalGradients) {
  auto eeZeroVelConConfig = [](scalar_t positionErrorGain) {
    EndEffectorLinearConstraint::Config config;
    config.b.setZero(3);
    config.Av.setIdentity(3, 3);
    if (!numerics::almost_eq(positionErrorGain, 0.0)) {
      config.Ax.setZero(3, 3);
      config.Ax(2, 2) = positionErrorGain;
    }
    return config;
  };

  if (useAnalyticalGradients) {
    throw std::runtime_error(
        "[LeggedRobotInterface::getZeroVelocityConstraint] The analytical end-effector zero velocity constraint is not implemented!");
  } else {
    return std::unique_ptr<StateInputConstraint>(new ZeroVelocityConstraintCppAd(*modeScheduleManagerPtr_, eeKinematics, contactPointIndex,
                                                                                 eeZeroVelConConfig(modelSettings_.positionErrorGain)));
  }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
std::unique_ptr<StateInputConstraint> LeggedRobotInterface::getNormalVelocityConstraint(const EndEffectorKinematics<scalar_t>& eeKinematics,
                                                                                        size_t contactPointIndex,
                                                                                        bool useAnalyticalGradients) {
  if (useAnalyticalGradients) {
    throw std::runtime_error(
        "[LeggedRobotInterface::getNormalVelocityConstraint] The analytical end-effector normal velocity constraint is not implemented!");
  } else {
    return std::unique_ptr<StateInputConstraint>(
        new NormalVelocityConstraintCppAd(*modeScheduleManagerPtr_, eeKinematics, contactPointIndex));
  }
}

}  // namespace legged_robot
}  // namespace ocs2
