
// Copyright (c) 2012, 2013 Pierre MOULON.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef OPENMVG_SFM_ROBUST_H
#define OPENMVG_SFM_ROBUST_H

#include "openMVG/numeric/numeric.h"

#include "openMVG/multiview/solver_resection_kernel.hpp"
#include "openMVG/multiview/solver_resection_p3p.hpp"
#include "openMVG/multiview/solver_essential_kernel.hpp"
#include "openMVG/multiview/projection.hpp"
#include "openMVG/multiview/triangulation.hpp"

using namespace openMVG;

#include "openMVG/robust_estimation/robust_estimator_ACRansac.hpp"
#include "openMVG/robust_estimation/robust_estimator_ACRansacKernelAdaptator.hpp"

using namespace openMVG::robust;

static const size_t ACRANSAC_ITER = 4096;

namespace openMVG{
namespace SfMRobust{

/// Estimate the essential matrix from point matches and K matrices.
bool robustEssential(const Mat3 & K1, const Mat3 & K2,
  const Mat & x1, const Mat & x2,
  Mat3 * pE, std::vector<size_t> * pvec_inliers,
  const std::pair<size_t, size_t> & size_ima1,
  const std::pair<size_t, size_t> & size_ima2,
  double * errorMax,
  double precision = std::numeric_limits<double>::infinity())
{
  assert(pvec_inliers != NULL);
  assert(pE != NULL);

  // Use the 5 point solver to estimate E
  typedef openMVG::essential::kernel::FivePointKernel SolverType;
  // Define the AContrario adaptor
  typedef ACKernelAdaptorEssential<
      SolverType,
      openMVG::fundamental::kernel::EpipolarDistanceError,
      UnnormalizerT,
      Mat3>
      KernelType;

  KernelType kernel(x1, size_ima1.first, size_ima1.second,
                    x2, size_ima2.first, size_ima2.second, K1, K2);

  // Robustly estimation of the Essential matrix and it's precision
  std::pair<double,double> ACRansacOut = ACRANSAC(kernel, *pvec_inliers,
    ACRANSAC_ITER, pE, precision, true);
  *errorMax = ACRansacOut.first;

  return pvec_inliers->size() > 2.5 * SolverType::MINIMUM_SAMPLES;
}

/// Estimate the best possible Rotation/Translation from E
/// Four are possible, keep the one with most of the point in front.
bool estimate_Rt_fromE(const Mat3 & K1, const Mat3 & K2,
  const Mat & x1, const Mat & x2,
  const Mat3 & E, const std::vector<size_t> & vec_inliers,
  Mat3 * R, Vec3 * t)
{
  bool bOk = false;

  // Accumulator to find the best solution
  std::vector<size_t> f(4, 0);

  std::vector<Mat3> Es; // Essential,
  std::vector<Mat3> Rs;  // Rotation matrix.
  std::vector<Vec3> ts;  // Translation matrix.

  Es.push_back(E);
  // Recover best rotation and translation from E.
  MotionFromEssential(E, &Rs, &ts);

  //-> Test the 4 solutions will all the point
  assert(Rs.size() == 4);
  assert(ts.size() == 4);

  Mat34 P1, P2;
  Mat3 R1 = Mat3::Identity();
  Vec3 t1 = Vec3::Zero();
  P_From_KRt(K1, R1, t1, &P1);

  for (int i = 0; i < 4; ++i) {
    const Mat3 &R2 = Rs[i];
    const Vec3 &t2 = ts[i];
    P_From_KRt(K2, R2, t2, &P2);
    Vec3 X;

    for (size_t k = 0; k < vec_inliers.size(); ++k) {
      const Vec2 & x1_ = x1.col(vec_inliers[k]),
        & x2_ = x2.col(vec_inliers[k]);
      TriangulateDLT(P1, x1_, P2, x2_, &X);
      // Test if point is front to the two cameras.
      if (Depth(R1, t1, X) > 0 && Depth(R2, t2, X) > 0) {
          ++f[i];
      }
    }
  }
  // Check the solution :
  std::cout << std::endl << "bundlerHelp::estimate_Rt_fromE" << std::endl;
  std::cout << "\t Number of points in front of both cameras:"
    << f[0] << " " << f[1] << " " << f[2] << " " << f[3] << std::endl;
  std::vector<size_t>::iterator iter = max_element(f.begin(), f.end());
  if(*iter != 0)  {
    size_t index = std::distance(f.begin(),iter);
    (*R) = Rs[index];
    (*t) = ts[index];
    bOk = true;
  }
  else  {
    std::cerr << std::endl << "/!\\There is no right solution,"
      <<" probably intermediate results are not correct or no points"
      <<" in front of both cameras" << std::endl;
    bOk = false;
  }
  return bOk;
}

/// Triangulate a set of points between two view
void triangulate2View_Vector(const Mat34 & P1,
  const Mat34 & P2,
  const std::vector<SIOPointFeature> & vec_feat1,
  const std::vector<SIOPointFeature> & vec_feat2,
  const std::vector<IndMatch>  & vec_index,
  std::vector<Vec3> * pvec_3dPoint,
  std::vector<double> * pvec_residual)
{
  assert(pvec_3dPoint);
  assert(pvec_residual);

  pvec_3dPoint->reserve(vec_index.size());
  pvec_residual->reserve(vec_index.size());
  double dMin = std::numeric_limits<double>::max(),
         dMax = std::numeric_limits<double>::min();
  for (size_t i=0; i < vec_index.size(); ++i)
  {
    //Get corresponding point and triangulate it
    const SIOPointFeature & imaA = vec_feat1[vec_index[i]._i];
    const SIOPointFeature & imaB = vec_feat2[vec_index[i]._j];

    Vec2 x1 = imaA.coords().cast<double>(),
         x2 = imaB.coords().cast<double>();

    Vec3 X_euclidean = Vec3::Zero();
    TriangulateDLT(P1, x1, P2, x2, &X_euclidean);
    //RefineTriangulation(P1, x1, P2, x2, &X_euclidean);
    double dResidual2D = ( (x1-Project(P1, X_euclidean)).norm() +
      (x2-Project(P2, X_euclidean)).norm() ) /2.0;

    dMin = std::min(dMin, dResidual2D);
    dMax = std::max(dMax, dResidual2D);

    // store 3DPoint and associated residual
    pvec_3dPoint->push_back(X_euclidean);
    pvec_residual->push_back(dResidual2D);
  }
  std::cout << std::endl
    << "bundlerHelper::triangulate2View_Vector" << std::endl
    << "\t-- Residual min max -- " << dMin <<"\t" << dMax << std::endl;
}

/// Return the angle (degree) between two camera ray
double angleBetweenRay(
  const Mat3 & K1, const Mat3 & R1, const Vec3 & t1,
  const Mat3 &K2, const Mat3 & R2, const Vec3 & t2,
  const Vec2 & x1, const Vec2 & x2)
{
  Vec3 C1 = -R1.inverse()*t1;
  Vec3 C2 = -R2.inverse()*t2;

  Vec3 ray1 = R1.transpose()*(K1.inverse()*Vec3(x1(0),x1(1),1)).normalized();
  Vec3 ray2 = R2.transpose()*(K2.inverse()*Vec3(x2(0),x2(1),1)).normalized();
  ray1.normalize();
  ray2.normalize();
  // Subtract camera center
  ray1 = ray1-C1;
  ray2 = ray2-C2;

  double mag = ray1.norm() * ray2.norm();

  double dotAngle = ray1.dot(ray2);
#define RAD2DEG(r) ((r) * (180.0 / M_PI))

  return RAD2DEG(acos(clamp(dotAngle/mag, -1.0+1.e-8, 1.0-1.e-8)));
}

/// Compute the robust resection of the 3D<->2D correspondences.
bool robustResection(
  const std::pair<size_t,size_t> & imageSize,
  const Mat & pt2D,
  const Mat & pt3D,
  std::vector<size_t> * pvec_inliers,
  const Mat3 * K = NULL,
  Mat34 * P = NULL,
  double * maxError = NULL)
{
  double dPrecision = std::numeric_limits<double>::infinity();
  size_t MINIMUM_SAMPLES = 0;
  // Classic resection
  if (K == NULL)
  {
    typedef openMVG::resection::kernel::SixPointResectionSolver SolverType;
    MINIMUM_SAMPLES = SolverType::MINIMUM_SAMPLES;

    typedef ACKernelAdaptorResection<
      SolverType, SolverType, UnnormalizerResection, Mat34>
      KernelType;

    KernelType kernel(pt2D, imageSize.first, imageSize.second, pt3D);
    // Robustly estimation of the Projection matrix and it's precision
    std::pair<double,double> ACRansacOut = ACRANSAC(kernel, *pvec_inliers,
      ACRANSAC_ITER, P, dPrecision, true);
    *maxError = ACRansacOut.first;

  }
  else
  {
    // If K is available use the Epnp solver
    //typedef openMVG::euclidean_resection::kernel::EpnpSolver SolverType;
    typedef openMVG::euclidean_resection::P3PSolver SolverType;
    MINIMUM_SAMPLES = SolverType::MINIMUM_SAMPLES;

    typedef ACKernelAdaptorResection_K<
      SolverType,  SolverType,  UnnormalizerResection, Mat34>  KernelType;

    KernelType kernel(pt2D, pt3D, *K);
    // Robustly estimation of the Projection matrix and it's precision
    std::pair<double,double> ACRansacOut = ACRANSAC(kernel, *pvec_inliers,
      ACRANSAC_ITER, P, dPrecision, true);
    *maxError = ACRansacOut.first;
  }

  // Test if the found model is valid
  if (pvec_inliers->size() > 2.5 * MINIMUM_SAMPLES)
  {
    // Re-estimate the model from the inlier data
    // and/or LM to Refine f R|t
    return true;
  }
  else{
    P = NULL;
    return false;
  }
}

} // namespace SfMRobust
} // namespace openMVG

#endif // OPENMVG_SFM_ROBUST_H

