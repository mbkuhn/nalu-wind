// Copyright 2017 National Technology & Engineering Solutions of Sandia, LLC
// (NTESS), National Renewable Energy Laboratory, University of Texas Austin,
// Northwest Research Associates. Under the terms of Contract DE-NA0003525
// with NTESS, the U.S. Government retains certain rights in this software.
//
// This software is released under the BSD 3-clause license. See LICENSE file
// for more details.
//

#ifdef NALU_USES_TIOGA

#include "overset/TiogaSTKIface.h"
#include "overset/TiogaBlock.h"
#include "overset/TiogaRef.h"

#include "overset/OversetManagerTIOGA.h"
#include "overset/OversetInfo.h"
#include "utils/StkHelpers.h"
#include "ngp_utils/NgpFieldUtils.h"

#include "NaluEnv.h"
#include "Realm.h"
#include "master_element/MasterElement.h"
#include "master_element/MasterElementRepo.h"
#include "stk_util/parallel/ParallelReduce.hpp"
#include "stk_mesh/base/FieldParallel.hpp"
#include "stk_mesh/base/FieldBLAS.hpp"
#include "stk_mesh/base/SkinBoundary.hpp"

#include "yaml-cpp/yaml.h"

#include <iostream>
#include <cmath>
#include <algorithm>
#include <numeric>

#include "tioga.h"

namespace tioga_nalu {

TiogaSTKIface::TiogaSTKIface(
  sierra::nalu::OversetManagerTIOGA& oversetManager,
  const YAML::Node& node,
  const std::string& coordsName)
  : oversetManager_(oversetManager),
    meta_(*oversetManager.metaData_),
    bulk_(*oversetManager.bulkData_),
    tg_(TiogaRef::self().get()),
    coordsName_(coordsName)
{
  load(node);
}

TiogaSTKIface::~TiogaSTKIface() {}

void
TiogaSTKIface::load(const YAML::Node& node)
{
  const YAML::Node& oset_groups = node["mesh_group"];

  if (node["tioga_options"])
    tiogaOpts_.load(node["tioga_options"]);

  int num_meshes = oset_groups.size();
  blocks_.resize(num_meshes);

  int offset = 0;
  if (node["mesh_tag_offset"]) {
    offset = node["mesh_tag_offset"].as<int>();
  }
  for (int i = 0; i < num_meshes; i++) {
    blocks_[i].reset(new TiogaBlock(
      meta_, bulk_, tiogaOpts_, oset_groups[i], coordsName_, offset + i + 1));
  }

  sierra::nalu::NaluEnv::self().naluOutputP0()
    << "TIOGA: Using coordinates field: " << coordsName_ << std::endl;

  if (node["tioga_symmetry_direction"])
    sierra::nalu::NaluEnv::self().naluOutputP0()
      << "WARNING!! TiogaSTKIface: tioga_symmetry_direction is no longer "
         "supported. "
      << "Use tioga_options to specify options that control TIOGA behavior"
      << std::endl;
}

void
TiogaSTKIface::setup(stk::mesh::PartVector& bcPartVec)
{
  for (auto& tb : blocks_) {
    tb->setup(bcPartVec);
  }
}

void
TiogaSTKIface::initialize()
{
  tiogaOpts_.set_options(tg_);

  sierra::nalu::NaluEnv::self().naluOutputP0()
    << "TIOGA: Initializing overset mesh blocks: " << std::endl;
  for (auto& tb : blocks_) {
    tb->initialize();
  }
  sierra::nalu::NaluEnv::self().naluOutputP0()
    << "TIOGA: Initialized " << blocks_.size() << " overset blocks"
    << std::endl;
}

void
TiogaSTKIface::execute(const bool isDecoupled)
{
#if defined(KOKKOS_ENABLE_GPU)
  // Bail out early if this is a GPU build and is using non-decoupled solve
  if (!isDecoupled) {
    throw std::runtime_error(
      "Non-decoupled overset connectivity not available in NGP build");
  }
#endif

  register_mesh();

  // Determine overset connectivity
  tg_.profile();
  tg_.performConnectivity();
  if (tiogaOpts_.reduce_fringes())
    tg_.reduce_fringes();

  post_connectivity_work(isDecoupled);
}

void
TiogaSTKIface::register_mesh()
{
  reset_data_structures();

  // Synchronize fields to host during transition period
  pre_connectivity_sync();

  // Update the coordinates for TIOGA and register updates to the TIOGA mesh
  // block.
  for (auto& tb : blocks_) {
    tb->update_coords();
    tb->update_element_volumes();
    tb->adjust_cell_resolutions();
  }

  auto* nodeVol =
    meta_.get_field(stk::topology::NODE_RANK, "tioga_nodal_volume");
  stk::mesh::parallel_max(bulk_, {nodeVol});

  for (auto& tb : blocks_) {
    tb->adjust_node_resolutions();
    tb->register_block(tg_);
  }
}

void
TiogaSTKIface::post_connectivity_work(const bool isDecoupled)
{
  for (auto& tb : blocks_) {
    // Update IBLANK information at nodes and elements
    tb->update_iblanks(
      oversetManager_.holeNodes_, oversetManager_.fringeNodes_);
    tb->update_iblank_cell();

    // For each block determine donor elements that needs to be ghosted to other
    // MPI ranks
    if (!isDecoupled)
      tb->get_donor_info(tg_, elemsToGhost_);
  }

  // Synchronize IBLANK data for shared nodes
  sierra::nalu::ScalarIntFieldType* ibf =
    meta_.get_field<int>(stk::topology::NODE_RANK, "iblank");
  std::vector<const stk::mesh::FieldBase*> pvec{ibf};
  stk::mesh::copy_owned_to_shared(bulk_, pvec);

  post_connectivity_sync();

  if (!isDecoupled) {
    get_receptor_info();

    // Collect all elements to be ghosted and update ghosting so that the
    // elements are available when generating {fringeNode, donorElement} pairs
    // in the next step.
    update_ghosting();

    // Update overset fringe connectivity information for Constraint based
    // algorithm
    populate_overset_info();
  }
}

void
TiogaSTKIface::reset_data_structures()
{
  oversetManager_.reset_data_structures();
  elemsToGhost_.clear();
  donorIDs_.clear();
  receptorIDs_.clear();
}

void
TiogaSTKIface::update_ghosting()
{
  std::vector<stk::mesh::EntityKey> recvGhostsToRemove;

  size_t local[2] = {elemsToGhost_.size(), recvGhostsToRemove.size()};
  size_t global[2] = {0, 0};
  stk::all_reduce_sum(bulk_.parallel(), local, global, 2);

  if ((global[0] > 0) || (global[1] > 0)) {
    bulk_.modification_begin();
    if (oversetManager_.oversetGhosting_ != nullptr) {
      bulk_.destroy_ghosting(*oversetManager_.oversetGhosting_);
    }
    const std::string ghostName = "nalu_overset_ghosting";
    oversetManager_.oversetGhosting_ = &(bulk_.create_ghosting(ghostName));
    bulk_.change_ghosting(
      *(oversetManager_.oversetGhosting_), elemsToGhost_, recvGhostsToRemove);
    bulk_.modification_end();

    sierra::nalu::populate_ghost_comm_procs(
      bulk_, *(oversetManager_.oversetGhosting_),
      oversetManager_.ghostCommProcs_);

#if 1
    sierra::nalu::NaluEnv::self().naluOutputP0()
      << "TIOGA: Overset algorithm will ghost " << global[0] << " elements"
      << std::endl;
#endif
  }
#if 1
  else {
    sierra::nalu::NaluEnv::self().naluOutputP0()
      << "TIOGA: Overset ghosting unchanged for this timestep" << std::endl;
  }
#endif

  // Communicate coordinates field when populating oversetInfoVec
  if (oversetManager_.oversetGhosting_ != nullptr) {
    sierra::nalu::VectorFieldType* coords =
      meta_.get_field<double>(stk::topology::NODE_RANK, coordsName_);
    std::vector<const stk::mesh::FieldBase*> fVec = {coords};
    stk::mesh::communicate_field_data(*oversetManager_.oversetGhosting_, fVec);
  }
}

void
TiogaSTKIface::get_receptor_info()
{
  sierra::nalu::ScalarIntFieldType* ibf =
    meta_.get_field<int>(stk::topology::NODE_RANK, "iblank");

  std::vector<unsigned long> nodesToReset;

  // Ask TIOGA for the fringe points and their corresponding donor element
  // information
  std::vector<int> receptors;
  tg_.getReceptorInfo(receptors);

  // Process TIOGA receptors array and fill in the oversetInfoVec used for
  // subsequent Nalu computations.
  //
  // TIOGA returns a integer array that contains 3 entries per receptor node:
  //   - the local node index within the tioga mesh data array
  //   - the local mesh tag (block index) for that mesh during registration
  //   - the STK global ID for the donor element (can be 8-byte or 4-byte)
  //
  size_t ncount = receptors.size();
  stk::mesh::EntityId donorID = std::numeric_limits<stk::mesh::EntityId>::max();
#ifdef TIOGA_HAS_UINT64T
  // The donor ID is stored in 2 4-byte integer entries (2 + 2 = 4). See above
  // for description on what is returned for each receptor node.
  const int rec_offset = 4;
#else
  // The donor ID is stored in a single 4-byte integer entry (2 + 1 = 3)
  const int rec_offset = 3;
#endif
  for (size_t i = 0; i < ncount; i += rec_offset) {
    int nid = receptors[i];          // TiogaBlock node index
    int mtag = receptors[i + 1] - 1; // Block index
#ifdef TIOGA_HAS_UINT64T
    std::memcpy(&donorID, &receptors[i + 2], sizeof(uint64_t));
#else
    donorID = receptors[i + 2]; // STK Global ID of the donor element
#endif
    auto nodeID =
      blocks_[mtag]->node_id_map()[nid]; // STK Global ID of the fringe node
    stk::mesh::Entity node = bulk_.get_entity(stk::topology::NODE_RANK, nodeID);

    if (!bulk_.bucket(node).owned()) {
      // We have a shared node that is marked as fringe. Ensure that the owning
      // proc also has this marked as fringe.
      int ibval = *stk::mesh::field_data(*ibf, node);

      if (ibval > -1) {
        // Disagreement between owner and shared status of iblank. Communicate
        // to owner and other shared procs that it must be a fringe.
        std::vector<int> sprocs;
        bulk_.comm_shared_procs(bulk_.entity_key(node), sprocs);
        for (auto jproc : sprocs) {
          if (jproc == bulk_.parallel_rank())
            continue;

          nodesToReset.push_back(jproc);
          nodesToReset.push_back(nodeID);
          nodesToReset.push_back(donorID);
        }
      }
    }

    // Stash the IDs for populating OversetInfo
    donorIDs_.push_back(donorID);
    receptorIDs_.push_back(nodeID);
  }

  int numLocal = nodesToReset.size();
  int iproc = bulk_.parallel_rank();
  int nproc = bulk_.parallel_size();
  std::vector<int> nbPerProc(nproc);
  MPI_Allgather(
    &numLocal, 1, MPI_INT, nbPerProc.data(), 1, MPI_INT, bulk_.parallel());

  // Total number of entities across all procs
  int nTotalEntities = std::accumulate(nbPerProc.begin(), nbPerProc.end(), 0);

  // If no disagreements were detected then we are done here
  if (nTotalEntities < 1)
    return;

#if 1
  sierra::nalu::NaluEnv::self().naluOutputP0()
    << "TIOGA: Detected fringe/field mismatch on " << (nTotalEntities / 3)
    << " entities" << std::endl;
#endif

  // Prepare data structures for reconciliation
  std::vector<int> offsets(nproc + 1);
  std::vector<unsigned long> allEntities(nTotalEntities);

  offsets[0] = 0;
  for (int i = 1; i <= nproc; ++i) {
    offsets[i] = offsets[i - 1] + nbPerProc[i - 1];
  }

  MPI_Allgatherv(
    nodesToReset.data(), numLocal, MPI_UNSIGNED_LONG, allEntities.data(),
    nbPerProc.data(), offsets.data(), MPI_UNSIGNED_LONG, bulk_.parallel());

  for (int i = 0; i < nTotalEntities; i += 3) {
    int nodeProc = allEntities[i];
    stk::mesh::EntityId nodeID = allEntities[i + 1];
    stk::mesh::EntityId donorID = allEntities[i + 2];

    // Add the receptor donor pair to populate OversetInfo
    if (iproc == nodeProc) {
      receptorIDs_.push_back(nodeID);
      donorIDs_.push_back(donorID);
    }

    // Setup for ghosting
    stk::mesh::Entity elem =
      bulk_.get_entity(stk::topology::ELEM_RANK, donorID);
    if (
      bulk_.is_valid(elem) && (bulk_.parallel_owner_rank(elem) == iproc) &&
      (nodeProc != iproc)) {
      // Found the owning proc for this donor element. Request ghosting
      stk::mesh::EntityProc elem_proc(elem, nodeProc);
      elemsToGhost_.push_back(elem_proc);
    }
  }
}

void
TiogaSTKIface::populate_overset_info()
{
  auto& osetInfo = oversetManager_.oversetInfoVec_;
  int nDim = meta_.spatial_dimension();
  std::vector<double> elemCoords;
  std::unordered_set<stk::mesh::EntityId> seenIDs;

  // Ensure that the oversetInfoVec has been cleared out
  STK_ThrowAssert(osetInfo.size() == 0);

  sierra::nalu::VectorFieldType* coords =
    meta_.get_field<double>(stk::topology::NODE_RANK, coordsName_);

  size_t numReceptors = receptorIDs_.size();
  for (size_t i = 0; i < numReceptors; i++) {
    stk::mesh::EntityId nodeID = receptorIDs_[i];
    stk::mesh::EntityId donorID = donorIDs_[i];
    stk::mesh::Entity node = bulk_.get_entity(stk::topology::NODE_RANK, nodeID);
    stk::mesh::Entity elem =
      bulk_.get_entity(stk::topology::ELEM_RANK, donorID);

    // Track fringe nodes that have already been processed.
    //
    // This is necessary when handling fringe-field mismatch across processors,
    // multiple shared procs might indicate that the owner must reset their
    // status. This check ensures the fringe is processed only once.
    auto hasIt = seenIDs.find(nodeID);
    if (hasIt != seenIDs.end())
      continue;
    seenIDs.insert(nodeID);

#if 1
    // The donor element must have already been ghosted to the required MPI
    // rank, so validity check should always succeed.
    if (!bulk_.is_valid(elem))
      throw std::runtime_error(
        "Invalid element encountered in overset mesh connectivity");
#endif

    // At this point, we have all the necessary information to create an
    // OversetInfo instance for this {receptor node, donor element} pair.
    sierra::nalu::OversetInfo* oinfo =
      new sierra::nalu::OversetInfo(node, nDim);
    osetInfo.push_back(oinfo);

    // Store away the coordinates for this receptor node for later use
    const double* xyz = stk::mesh::field_data(*coords, node);
    for (int i = 0; i < nDim; i++) {
      oinfo->nodalCoords_[i] = xyz[i];
    }

    const stk::topology elemTopo = bulk_.bucket(elem).topology();
    const stk::mesh::Entity* enodes = bulk_.begin_nodes(elem);
    sierra::nalu::MasterElement* meSCS =
      sierra::nalu::MasterElementRepo::get_surface_master_element_on_host(
        elemTopo);
    int num_nodes = bulk_.num_nodes(elem);
    elemCoords.resize(nDim * num_nodes);

    for (int ni = 0; ni < num_nodes; ++ni) {
      stk::mesh::Entity enode = enodes[ni];
      const double* xyz = stk::mesh::field_data(*coords, enode);
      for (int j = 0; j < nDim; j++) {
        const int offset = j * num_nodes + ni;
        elemCoords[offset] = xyz[j];
      }
    }

    const double nearestDistance = meSCS->isInElement(
      elemCoords.data(), oinfo->nodalCoords_.data(),
      oinfo->isoParCoords_.data());

#if 0
    if (nearestDistance > (1.0 + 1.0e-8))
      sierra::nalu::NaluEnv::self().naluOutput()
        << "TIOGA WARNING: In pair (" << nodeID << ", " << donorID << "): "
        << "iso-parametric distance is greater than 1.0: " << nearestDistance
        << std::endl;
#endif

    oinfo->owningElement_ = elem;
    oinfo->meSCS_ = meSCS;
    oinfo->bestX_ = nearestDistance;
    oinfo->elemIsGhosted_ = bulk_.bucket(elem).owned() ? 0 : 1;
  }

#if 1
  // Debugging information
  size_t numFringeLocal = osetInfo.size();
  size_t numFringeGlobal = 0;
  stk::all_reduce_sum(bulk_.parallel(), &numFringeLocal, &numFringeGlobal, 1);

  sierra::nalu::NaluEnv::self().naluOutputP0()
    << "TIOGA: Num. receptor nodes = " << numFringeGlobal << std::endl;
#endif
}

void
TiogaSTKIface::overset_update_fields(
  const std::vector<sierra::nalu::OversetFieldData>& fields)
{
  constexpr int row_major = 0;
  int nComp = 0;
  for (auto& f : fields) {
    f.field_->sync_to_host();
    nComp += f.sizeRow_ * f.sizeCol_;
  }

  for (auto& tb : blocks_)
    tb->register_solution(tg_, fields, nComp);

  tg_.dataUpdate(nComp, row_major);

  for (auto& tb : blocks_)
    tb->update_solution(fields);

  for (auto& finfo : fields) {
    auto* fld = finfo.field_;
    fld->modify_on_host();
    fld->sync_to_device();
  }
}

int
TiogaSTKIface::register_solution(
  const std::vector<sierra::nalu::OversetFieldData>& fields)
{
  int nComp = 0;
  for (auto& f : fields) {
    f.field_->sync_to_host();
    nComp += f.sizeRow_ * f.sizeCol_;
  }

  for (auto& tb : blocks_)
    tb->register_solution(tg_, fields, nComp);

  return nComp;
}

void
TiogaSTKIface::update_solution(
  const std::vector<sierra::nalu::OversetFieldData>& fields)
{
  for (auto& tb : blocks_)
    tb->update_solution(fields);

  for (auto& finfo : fields) {
    auto* fld = finfo.field_;
    fld->modify_on_host();
    fld->sync_to_device();
  }
}

void
TiogaSTKIface::overset_update_field(
  stk::mesh::FieldBase* field,
  const int nrows,
  const int ncols,
  const bool doFinalSyncToDevice)
{
  constexpr int row_major = 0;
  sierra::nalu::OversetFieldData fdata{field, nrows, ncols};

  field->sync_to_host();

  for (auto& tb : blocks_)
    tb->register_solution(tg_, fdata);

  tg_.dataUpdate(nrows * ncols, row_major);

  for (auto& tb : blocks_)
    tb->update_solution(fdata);

  field->modify_on_host();
  if (doFinalSyncToDevice)
    field->sync_to_device();
}

void
TiogaSTKIface::pre_connectivity_sync()
{
  auto* coords = meta_.get_field<double>(stk::topology::NODE_RANK, coordsName_);
  auto* dualVol =
    meta_.get_field<double>(stk::topology::NODE_RANK, "dual_nodal_volume");
  auto* elemVol =
    meta_.get_field<double>(stk::topology::ELEMENT_RANK, "element_volume");

  coords->sync_to_host();
  dualVol->sync_to_host();
  elemVol->sync_to_host();

  // Needed for adjusting resolutions
  auto* tgNodalVol =
    meta_.get_field(stk::topology::NODE_RANK, "tioga_nodal_volume");
  stk::mesh::field_copy(*dualVol, *tgNodalVol);
}

void
TiogaSTKIface::post_connectivity_sync()
{
  // Push iblank fields to device
  {
    auto* ibnode = meta_.get_field<int>(stk::topology::NODE_RANK, "iblank");
    auto* ibcell =
      meta_.get_field<int>(stk::topology::ELEM_RANK, "iblank_cell");
    ibnode->modify_on_host();
    ibnode->sync_to_device();
    ibcell->modify_on_host();
    ibcell->sync_to_device();
  }

  // Create device version of the fringe/hole lists for reset rows
  const auto& fringes = oversetManager_.fringeNodes_;
  const auto& holes = oversetManager_.holeNodes_;
  auto& ngpFringes = oversetManager_.ngpFringeNodes_;
  auto& ngpHoles = oversetManager_.ngpHoleNodes_;

  ngpFringes =
    sierra::nalu::OversetManager::EntityList("ngp_fringe_list", fringes.size());
  ngpHoles =
    sierra::nalu::OversetManager::EntityList("ngp_hole_list", holes.size());

  auto h_fringes = Kokkos::create_mirror_view(ngpFringes);
  auto h_holes = Kokkos::create_mirror_view(ngpHoles);

  for (size_t i = 0; i < fringes.size(); ++i) {
    h_fringes[i] = fringes[i];
  }
  for (size_t i = 0; i < holes.size(); ++i) {
    h_holes[i] = holes[i];
  }
  Kokkos::deep_copy(ngpFringes, h_fringes);
  Kokkos::deep_copy(ngpHoles, h_holes);
}

} // namespace tioga_nalu

#endif // NALU_USES_TIOGA
