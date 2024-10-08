/*------------------------------------------------------------------------*/
/*  Copyright 2017 National Renewable Energy Laboratory.                  */
/*  This software is released under the license detailed                  */
/*  in the file, LICENSE, which is located in the top-level Nalu          */
/*  directory structure                                                   */
/*------------------------------------------------------------------------*/

#include "node_kernels/BLTGammaM2015NodeKernel.h"
#include "Realm.h"
#include "SolutionOptions.h"
#include "SimdInterface.h"
#include "utils/StkHelpers.h"
#include "NaluEnv.h"

#include "stk_mesh/base/MetaData.hpp"
#include "stk_mesh/base/Types.hpp"

namespace sierra {
namespace nalu {

BLTGammaM2015NodeKernel::BLTGammaM2015NodeKernel(
  const stk::mesh::MetaData& meta)
  : NGPNodeKernel<BLTGammaM2015NodeKernel>(),
    tkeID_(get_field_ordinal(meta, "turbulent_ke")),
    sdrID_(get_field_ordinal(meta, "specific_dissipation_rate")),
    densityID_(get_field_ordinal(meta, "density")),
    viscID_(get_field_ordinal(meta, "viscosity")),
    dudxID_(get_field_ordinal(meta, "dudx")),
    minDID_(get_field_ordinal(meta, "minimum_distance_to_wall")),
    dwalldistdxID_(get_field_ordinal(meta, "dwalldistdx")),
    dnDotVdxID_(get_field_ordinal(meta, "dnDotVdx")),
    dualNodalVolumeID_(get_field_ordinal(meta, "dual_nodal_volume")),
    gamintID_(get_field_ordinal(meta, "gamma_transition")),
    nDim_(meta.spatial_dimension())
{
}

void
BLTGammaM2015NodeKernel::setup(Realm& realm)
{
  const auto& fieldMgr = realm.ngp_field_manager();

  tke_ = fieldMgr.get_field<double>(tkeID_);
  sdr_ = fieldMgr.get_field<double>(sdrID_);
  density_ = fieldMgr.get_field<double>(densityID_);
  visc_ = fieldMgr.get_field<double>(viscID_);
  dudx_ = fieldMgr.get_field<double>(dudxID_);
  minD_ = fieldMgr.get_field<double>(minDID_);
  dwalldistdx_ = fieldMgr.get_field<double>(dwalldistdxID_);
  dnDotVdx_ = fieldMgr.get_field<double>(dnDotVdxID_);
  dualNodalVolume_ = fieldMgr.get_field<double>(dualNodalVolumeID_);
  gamint_ = fieldMgr.get_field<double>(gamintID_);

  fsti_ = realm.get_turb_model_constant(TM_fsti);
}

KOKKOS_FUNCTION
double
BLTGammaM2015NodeKernel::FPG(const double& lamda0L)
{
  using DblType = NodeKernelTraits::DblType;

  DblType out; // this is the result of this calculation
  DblType CPG1 = 14.68;
  DblType CPG2 = -7.34;
  DblType CPG3 = 0.0;

  DblType CPG1_lim = 1.5;
  DblType CPG2_lim = 3.0;

  if (lamda0L >= 0.0) {
    out = stk::math::min(1.0 + CPG1 * lamda0L, CPG1_lim);
  } else {
    out = stk::math::min(
      1.0 + CPG2 * lamda0L + CPG3 * stk::math::min(lamda0L + 0.0681, 0.0),
      CPG2_lim);
  }

  out = stk::math::max(out, 0.0);

  return out;
}

KOKKOS_FUNCTION
void
BLTGammaM2015NodeKernel::execute(
  NodeKernelTraits::LhsType& lhs,
  NodeKernelTraits::RhsType& rhs,
  const stk::mesh::FastMeshIndex& node)
{
  using DblType = NodeKernelTraits::DblType;

  const DblType tke = tke_.get(node, 0);
  const DblType sdr = sdr_.get(node, 0);
  const DblType gamint = gamint_.get(node, 0);

  const DblType density = density_.get(node, 0);
  const DblType visc = visc_.get(node, 0);
  const DblType minD = minD_.get(node, 0);
  const DblType dVol = dualNodalVolume_.get(node, 0);

  DblType dvnn = 0.0;
  DblType TuL = 0.0;
  DblType lamda0L = 0.0;

  DblType sijMag = 0.0;
  DblType vortMag = 0.0;

  // constants for the source terms
  const DblType flength = 100.0;
  const DblType caTwo = 0.06;
  const DblType ceTwo = 50.0;

  // constants for the local-correlations
  const DblType Ctu1 = 100.;
  const DblType Ctu2 = 1000.;
  const DblType Ctu3 = 1.0;

  for (int i = 0; i < nDim_; ++i) {
    dvnn += dwalldistdx_.get(node, i) * dnDotVdx_.get(node, i);
    for (int j = 0; j < nDim_; ++j) {
      const double duidxj = dudx_.get(node, nDim_ * i + j);
      const double dujdxi = dudx_.get(node, nDim_ * j + i);

      const double rateOfStrain = 0.5 * (duidxj + dujdxi);
      const double vortTensor = 0.5 * (duidxj - dujdxi);
      sijMag += rateOfStrain * rateOfStrain;
      vortMag += vortTensor * vortTensor;
    }
  }

  sijMag = stk::math::sqrt(2.0 * sijMag);
  vortMag = stk::math::sqrt(2.0 * vortMag);

  if (fsti_ > 0.0) {
    TuL = fsti_; // const. Tu from yaml
  } else {
    TuL = stk::math::min(
      100.0 * stk::math::sqrt(2.0 / 3.0 * tke) / sdr / (minD + 1.0e-10),
      100.0); // local Tu
  }

  lamda0L = -7.57e-3 * dvnn * minD * minD * density / visc + 0.0128;
  lamda0L = stk::math::min(stk::math::max(lamda0L, -1.0), 1.0);

  const DblType Re0c = Ctu1 + Ctu2 * stk::math::exp(-Ctu3 * TuL * FPG(lamda0L));
  const DblType Rev = density * minD * minD * sijMag / visc;
  const DblType fonset1 = Rev / 2.2 / Re0c;
  const DblType fonset2 = stk::math::min(fonset1, 2.0);
  const DblType rt = density * tke / sdr / visc;
  const DblType fonset3 =
    stk::math::max(1.0 - stk::math::pow(rt / 3.5, 3), 0.0);
  const DblType fonset = stk::math::max(fonset2 - fonset3, 0.0);
  const DblType fturb = stk::math::exp(-rt * rt * rt * rt / 16.0);

  const DblType Pgamma =
    flength * density * sijMag * fonset * gamint * (1.0 - gamint);
  const DblType Dgamma =
    caTwo * density * vortMag * fturb * gamint * (ceTwo * gamint - 1.0);

  // Exact Jacobian
  // const DblType PgammaDir =
  //    flength * density * sijMag * fonset * (1.0 - 2.0 * gamint);
  // const DblType DgammaDir =
  //    caTwo * density * vortMag * fturb * (2.0 * ceTwo * gamint - 1.0);
  //
  //  rhs(0) += (Pgamma - Dgamma) * dVol;
  //  lhs(0, 0) += (DgammaDir - PgammaDir) * dVol;

  // Jacobian with the Positivity in the implicit operator
  const DblType PgammaDir =
    flength * density * sijMag * fonset * (1.0 - gamint);
  const DblType PgammaDirP = -flength * density * sijMag * fonset;

  const DblType DgammaDir =
    caTwo * density * vortMag * fturb * (ceTwo * gamint - 1.0);
  const DblType DgammaDirP = caTwo * density * vortMag * fturb * ceTwo;

  const DblType gamma_pos1 = stk::math::max(DgammaDir - PgammaDir, 0.0);
  const DblType gamma_pos2 = stk::math::max(DgammaDirP - PgammaDirP, 0.0);

  rhs(0) += (Pgamma - Dgamma) * dVol;
  lhs(0, 0) += (gamma_pos1 + gamma_pos2 * gamint) * dVol;
  //
}

} // namespace nalu
} // namespace sierra
