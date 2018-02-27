/*
 * Copyright (c) 2016, Graphics Lab, Georgia Tech Research Corporation
 * Copyright (c) 2016, Humanoid Lab, Georgia Tech Research Corporation
 * Copyright (c) 2016, Personal Robotics Lab, Carnegie Mellon University
 * All rights reserved.
 *
 * This file is provided under the following "BSD-style" License:
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 *   CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 *   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 *   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 *   USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 *   AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *   ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *   POSSIBILITY OF SUCH DAMAGE.
 */

#include "dart/experimental/BodyNodeViRiqnDrnea.hpp"

#include "dart/dynamics/BodyNode.hpp"
#include "dart/dynamics/DegreeOfFreedom.hpp"
#include "dart/dynamics/RevoluteJoint.hpp"
#include "dart/experimental/JointViRiqnDrnea.hpp"

namespace dart {
namespace dynamics {

//==============================================================================
std::unique_ptr<common::Aspect> BodyNodeViRiqnDrnea::cloneAspect() const
{
  // TODO(JS): Not implemented
  return common::make_unique<BodyNodeViRiqnDrnea>();
}

//==============================================================================
JointViRiqnDrnea* BodyNodeViRiqnDrnea::getJointVi()
{
  auto* bodyNode = mComposite;
  auto* joint = bodyNode->getParentJoint();
  assert(joint->get<JointViRiqnDrnea>());

  return joint->get<JointViRiqnDrnea>();
}

//==============================================================================
const JointViRiqnDrnea* BodyNodeViRiqnDrnea::getJointVi() const
{
  return const_cast<const BodyNodeViRiqnDrnea*>(this)->getJointVi();
}

//==============================================================================
void BodyNodeViRiqnDrnea::setComposite(common::Composite* newComposite)
{
  Base::setComposite(newComposite);

  auto* bodyNode = mComposite;
  auto skeleton = bodyNode->getSkeleton();
  //  const auto numDofs = skeleton->getNumDofs();
  //  const auto timeStep = skeleton->getTimeStep();

  assert(skeleton);

  // TODO(JS): These should be updated when the structure of skeleton is
  // modified. We might want to consider adding signal to skeleton for this.

  auto* joint = bodyNode->getParentJoint();
  if (joint->is<RevoluteJoint>())
  {
    JointViRiqnDrnea* revVI = new RevoluteJointViRiqnDrnea();
    joint->set(revVI);
    assert(joint->get<JointViRiqnDrnea>());
  }
  else
  {
    dterr << "[BodyNodeViRiqnDrnea::setComposite] Attempting to "
          << "create VI aspect for unsupported joint type '" << joint->getType()
          << "'.\n";
    assert(false);
  }
}

//==============================================================================
void BodyNodeViRiqnDrnea::updateNextTransform()
{
  auto* bodyNode = mComposite;
  auto* joint = bodyNode->getParentJoint();
  auto* jointAspect = joint->get<JointViRiqnDrnea>();

  jointAspect->updateNextRelativeTransform();
}

//==============================================================================
void BodyNodeViRiqnDrnea::updateNextVelocity(double timeStep)
{
  auto* bodyNode = mComposite;
  auto* parentBodyNode = bodyNode->getParentBodyNode();
  auto* joint = bodyNode->getParentJoint();
  auto* jointAspect = joint->get<JointViRiqnDrnea>();

  if (parentBodyNode)
  {
    auto parentBodyNodeVi = parentBodyNode->get<BodyNodeViRiqnDrnea>();
    assert(parentBodyNodeVi);

    mWorldTransformDisplacement
        = joint->getRelativeTransform().inverse()
          * parentBodyNodeVi->mWorldTransformDisplacement
          * jointAspect->mNextRelativeTransform;
  }
  else
  {
    mWorldTransformDisplacement
        = joint->getRelativeTransform().inverse() * jointAspect->mNextRelativeTransform;
  }

  mPostAverageSpatialVelocity
      = math::logMap(mWorldTransformDisplacement) / timeStep;
}

//==============================================================================
const Eigen::Vector6d& BodyNodeViRiqnDrnea::getPrevMomentum() const
{
  if (mNeedPrevMomentumUpdate)
  {
    auto* bodyNode = mComposite;
    const auto timeStep = bodyNode->getSkeleton()->getTimeStep();
    const Eigen::Matrix6d& G = bodyNode->getInertia().getSpatialTensor();
    const Eigen::Vector6d& V = bodyNode->getSpatialVelocity();
    mPrevMomentum = math::dexp_inv_transpose(V * timeStep, G * V);

    mNeedPrevMomentumUpdate = false;
  }

  return mPrevMomentum;
}

//==============================================================================
static Eigen::Vector6d computeSpatialGravityForce(
    const Eigen::Isometry3d& T,
    const Eigen::Matrix6d& inertiaTensor,
    const Eigen::Vector3d& gravityAcceleration)
{
  const Eigen::Matrix6d& I = inertiaTensor;
  const Eigen::Vector3d& g = gravityAcceleration;

  return I * math::AdInvRLinear(T, g);
}

//==============================================================================
void BodyNodeViRiqnDrnea::evaluateDel(
    const Eigen::Vector3d& gravity, double timeStep)
{
  auto* bodyNode = mComposite;
  auto* joint = bodyNode->getParentJoint();
  auto* jointAspect = joint->get<JointViRiqnDrnea>();

  const Eigen::Matrix6d& G = bodyNode->getInertia().getSpatialTensor();

  mPostMomentum = math::dexp_inv_transpose(
      mPostAverageSpatialVelocity * timeStep,
      G * mPostAverageSpatialVelocity);

  const Eigen::Isometry3d expHPrevVelocity
      = math::expMap(timeStep * mPreAverageSpatialVelocity);

  mParentImpulse
      = mPostMomentum
        - math::dAdT(expHPrevVelocity, getPrevMomentum())
        - computeSpatialGravityForce(bodyNode->getTransform(), G, gravity)
              * timeStep;
  // TODO(JS): subtract external force * timeStep to parentImpulse

  for (auto i = 0u; i < bodyNode->getNumChildBodyNodes(); ++i)
  {
    auto* childBodyNode = bodyNode->getChildBodyNode(i);
    auto* childBodyNodeVi = childBodyNode->get<BodyNodeViRiqnDrnea>();

    mParentImpulse += math::dAdInvT(
        childBodyNode->getParentJoint()->getRelativeTransform(),
        childBodyNodeVi->mParentImpulse);
  }

  jointAspect->evaluateDel(mParentImpulse, timeStep);
}

//==============================================================================
void BodyNodeViRiqnDrnea::updateNextTransformDeriv()
{
}

//==============================================================================
void BodyNodeViRiqnDrnea::updateNextVelocityDeriv(double /*timeStep*/)
{
}

} // namespace dynamics
} // namespace dart