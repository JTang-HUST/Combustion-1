/*
 * Multilevel SDC + AMR controller.
 *
 * When RNS is compiled with USE_SDCLIB=TRUE, the time-stepping is
 * done using the multi-level SDC (MLSDC) algorithm, with IMEX
 * sweepers on each level (hydrodynamics are explicit, chemistry is
 * implicit).  The MLSDC algorithm is implemented in C in SDCLib (in
 * SDCLib multi-level SDC is called multi-grid SDC).
 *
 * The interface between SDCLib and RNS is (mostly) contained in
 * SDCAmr (derived from Amr) and SDCAmrEncap.
 *
 * Note that in MLSDC, there is no concept of "sub-cycling" fine
 * levels.  As such, the "timeStep" method defined in SDCAmr is only
 * called on the coarsest level, and it essentially passes control to
 * SDCLib to advance the solution.
 *
 * SDCLib handles interpolation and restriction in time.  SDCLib also
 * takes care of allocating multifabs at each SDC node.  As such the
 * traditional Amr concepts of "old data", "new data", and defining an
 * "advance" routine no longer apply.  In order to reuse as much of
 * the existing Amr code as possible, SDCAmr puts the solution
 * obtained from SDCLib into the "new data" state (this means all the
 * logic to write plotfiles etc still works), but never defines "old
 * data" or any "mid data".
 *
 * Some notes:
 *
 * 1. Since we never use 'old data', any calls to fill patch iterator
 *    will always use 'new data', regardless of any 'time' information
 *    set on the state data.
 *
 * Known issues:
 *
 * 1. Currently the fine levels are very high order: we need to tweak
 *    the integration and interpolation matrices.
 *
 * 2. The SDC hierarchy is currently not depth limited.
 *
 * 3. We're using Gauss-Lobatto nodes, but Gauss-Radau would probably
 *    be better for chemistry.
 */

#include <SDCAmr.H>
#include <MultiFab.H>
#include <ParmParse.H>
#include <StateDescriptor.H>
#include <AmrLevel.H>
#include <Interpolater.H>
#include <FabArray.H>

#include "RNS.H"
#include "RNS_F.H"

#ifdef BL_USE_ARRAYVIEW
#include <ArrayView.H>
#endif

using namespace std;

BEGIN_EXTERN_C

/*
 * Spatial interpolation between MultiFabs.  Called by SDCLib.
 */
// xxxxx TODO: need to distinguish correction from solution, because they have
//             different physical boundary conditions
void mlsdc_amr_interpolate(void *Fp, void *Gp, sdc_state *state, void *ctxF, void *ctxG)
{
  BL_PROFILE("MLSDC_AMR_INTERPOLATE()");

  RNSEncap& F      = *((RNSEncap*) Fp);
  RNSEncap& G      = *((RNSEncap*) Gp);
  MultiFab& UF     = *F.U;
  MultiFab& UG     = *G.U;
  RNS&      levelF = *((RNS*) ctxF);
  RNS&      levelG = *((RNS*) ctxG);

  const IntVect         ratio = levelG.fineRatio();
  const DescriptorList& dl    = levelF.get_desc_lst();
  const Array<BCRec>&   bcs   = dl[0].getBCs();
  const int             ncomp = dl[0].nComp();
  Interpolater&         map   = *dl[0].interp();
  Array<BCRec>          bcr(ncomp);

  const Geometry& geomG = levelG.Geom();
  const Geometry& geomF = levelF.Geom();

  RNS_SETNAN(UF);

  // make a coarse version (UC) of the fine multifab (UF)
  BoxArray ba_C(UF.size());
  for (int i=0; i<ba_C.size(); i++)
    ba_C.set(i, map.CoarseBox(UF.fabbox(i), ratio));

  MultiFab UC(ba_C, ncomp, 0);
  RNS_SETNAN(UC);

  bool touch = false;
  bool touch_periodic = false;
  const Box& crse_domain_box = levelG.Domain();
  if (geomG.isAnyPeriodic()) {
      for (int i=0; i < ba_C.size(); i++) {
	  if (! crse_domain_box.contains(ba_C[i])) {
	      touch = true;
	      for (int idim=0; idim<BL_SPACEDIM; idim++) {
		  if (geomG.isPeriodic(i)   ||
		      ba_C[i].bigEnd(idim) > crse_domain_box.bigEnd(idim) ||
		      ba_C[i].smallEnd(idim) < crse_domain_box.smallEnd(idim) )
		  {
		      touch_periodic = true;
		      break;
		  }
	      }
	  }
	  if (touch_periodic) break;
      }
  }
  else {
      for (int i=0; i < ba_C.size(); i++) {
	  if (! crse_domain_box.contains(ba_C[i])) {
	      touch = true;
	      break;
	  }
      }
  }

  if (!touch) {
    // If level F does not touch physical boundaries, then AMR levels are
    // properly nested so that the valid rgions of UC are contained inside
    // the valid regions of UG.  So FabArray::copy is all we need.
    UC.copy(UG);
  }
  else if (touch_periodic) {
      // This case is more complicated because level F might touch only one
      // of the periodic boundaries.
      Box box_C = ba_C.minimalBox();
      int ng_C = box_C.bigEnd(0) - crse_domain_box.bigEnd(0);
      int ng_G = UG.nGrow();
      const BoxArray& ba_G = UG.boxArray();

      MultiFab* UG_safe;
      MultiFab UGG;
      if (ng_C > ng_G) {
	  UGG.define(ba_G, ncomp, ng_C, Fab_allocate);
	  MultiFab::Copy(UGG, UG, 0, 0, ncomp, 0);
	  UG_safe = &UGG;
      }
      else {
	  UG_safe = &UG;
      }

      // set periodic and physical boundaries
      levelG.fill_boundary(*UG_safe, state->t, RNS::set_PhysBoundary);

      // We cannot do FabArray::copy() directly on UG because it copies only form
      // valid regions.  So we need to make the ghost cells of UG valid.
      BoxArray ba_G2(UG.size());
      for (int i=0; i<ba_G2.size(); i++) {
	  ba_G2.set(i, BoxLib::grow(ba_G[i],ng_C));
      }

      MultiFab UG2(ba_G2, ncomp, 0);
      for (MFIter mfi(UG2); mfi.isValid(); ++mfi)
      {
	  int i = mfi.index();
	  UG2[i].copy((*UG_safe)[i]);  // Fab to Fab copy
      }

      UC.copy(UG2);

  }
  else {
    UC.copy(UG);
    levelG.fill_boundary(UC, state->t, RNS::set_PhysBoundary);
  }

  RNS_ASSERTNONAN(UC);

  // now that UF is completely contained within UC, cycle through each
  // FAB in UF and interpolate from the corresponding FAB in UC
// #ifdef _OPENMP
// #pragma omp parallel for
// #endif
  for (MFIter mfi(UF); mfi.isValid(); ++mfi) {
    BoxLib::setBC(UF[mfi].box(), levelF.Domain(), 0, 0, ncomp, bcs, bcr);
    Geometry fine_geom(UF[mfi].box());
    Geometry crse_geom(UC[mfi].box());

    map.interp(UC[mfi], 0, UF[mfi], 0, ncomp, UF[mfi].box(), ratio,
               crse_geom, fine_geom, bcr, 0, 0);
  }

  levelF.fill_boundary(UF, state->t, RNS::set_PhysBoundary);
  RNS_ASSERTNONAN(UF);
}

/*
 * Spatial restriction between MultiFabs.  Called by SDCLib.
 */
void mlsdc_amr_restrict(void *Fp, void *Gp, sdc_state *state, void *ctxF, void *ctxG)
{
  BL_PROFILE("MLSDC_AMR_RESTRICT()");

  RNSEncap& F      = *((RNSEncap*) Fp);
  RNSEncap& G      = *((RNSEncap*) Gp);
  MultiFab& UF     = *F.U;
  MultiFab& UG     = *G.U;
  RNS&      levelG = *((RNS*) ctxG);

  levelG.avgDown(UG, UF);
  if (state->kind == SDC_SOLUTION)
    levelG.fill_boundary(UG, state->t, RNS::use_FillBoundary);
}

END_EXTERN_C

/*
 * Take one SDC+AMR time step.
 *
 * This is only called on the coarsest SDC+AMR level.
 */
void SDCAmr::timeStep(int level, Real time,
		      int /* iteration */, int /* niter */,
		      Real stop_time)
{
  BL_PROFILE("SDCAmr::timeStep()");

  BL_ASSERT(level == 0);

  // build sdc hierarchy
  if (sweepers[0] == NULL) rebuild_mlsdc();

  // regrid
  int lev_top = min(finest_level, max_level-1);
  for (int i=level; i<=lev_top; i++) {
    const int post_regrid_flag = 1;
    const int old_finest = finest_level;

    regrid(i,time);
    amr_level[0].computeNewDt(finest_level, sub_cycle, n_cycle, ref_ratio,
  			      dt_min, dt_level, stop_time, post_regrid_flag);

    for (int k=i; k<=finest_level; k++) level_count[k] = 0;
    if (old_finest > finest_level) lev_top = min(finest_level, max_level-1);
  }

  rebuild_mlsdc();

  // echo
  double dt = dt_level[0];
  if (verbose > 0 && ParallelDescriptor::IOProcessor()) {
    cout << "MLSDC advancing with dt: " << dt << endl;
  }

  // set intial conditions and times
  for (int lev=0; lev<=finest_level; lev++) {
    MultiFab& Unew = getLevel(lev).get_new_data(0);
    RNSEncap& Q0   = *((RNSEncap*) mg.sweepers[lev]->nset->Q[0]);
    MultiFab& U0   = *Q0.U;
    MultiFab::Copy(U0, Unew, 0, 0, U0.nComp(), U0.nGrow());
    getLevel(lev).get_state_data(0).setTimeLevel(time+dt, dt, dt);
  }

  // fill fine boundaries using coarse data
  for (int lev=0; lev<=finest_level; lev++) {
    RNS&      rns  = *dynamic_cast<RNS*>(&getLevel(lev));
    RNSEncap& Q0   = *((RNSEncap*) mg.sweepers[lev]->nset->Q[0]);
    MultiFab& U0   = *Q0.U;
    rns.fill_boundary(U0, time, RNS::use_FillCoarsePatch);
  }

  // spread and iterate
  sdc_mg_spread(&mg, time, dt, 0);

  BL_PROFILE_VAR("SDCAmr::timeStep-iters", sdc_iters);

  for (int k=0; k<max_iters; k++) {
    sdc_mg_sweep(&mg, time, dt, (k==max_iters-1) ? SDC_MG_LAST_SWEEP : 0);

    if (verbose > 0) {
      for (int lev=0; lev<=finest_level; lev++) {
        int       nnodes = mg.sweepers[lev]->nset->nnodes;
	RNSEncap& R0     = *((RNSEncap*) mg.sweepers[lev]->nset->R[nnodes-2]);
        MultiFab& R      = *R0.U;
	double    r0     = R.norm0();
	double    r2     = R.norm2();

	// dzmq_send_mf(R, lev, 0, lev==finest_level);

	if (ParallelDescriptor::IOProcessor()) {
	  std::ios_base::fmtflags ff = cout.flags();
	  cout << "MLSDC iter: " << k << ", level: " << lev
	       << ", res norm0: " << scientific << r0 << ", res norm2: " << r2 << endl;
	  cout.flags(ff);
	}
      }
    }
  }

  BL_PROFILE_VAR_STOP(sdc_iters);

  // copy final solution from sdclib to new_data
  for (int lev=0; lev<=finest_level; lev++) {
    int       nnodes = mg.sweepers[lev]->nset->nnodes;
    MultiFab& Unew   = getLevel(lev).get_new_data(0);
    RNSEncap& Qend   = *((RNSEncap*) mg.sweepers[lev]->nset->Q[nnodes-1]);
    MultiFab& Uend   = *Qend.U;

    MultiFab::Copy(Unew, Uend, 0, 0, Uend.nComp(), 0);
  }

  level_steps[level]++;
  level_count[level]++;
}


/*
 * Build single SDC level.
 */
sdc_sweeper* SDCAmr::build_mlsdc_level(int lev)
{
  BL_PROFILE("SDCAmr::build_mlsdc_level()");

  int first_refinement_level, nnodes;

  if (finest_level - max_trefs > 1)
    first_refinement_level = finest_level - max_trefs;
  else
    first_refinement_level = 1;

  if (lev < first_refinement_level)
    nnodes = nnodes0;
  else
    nnodes = 1 + (nnodes0 - 1) * ((int) pow(trat, lev-first_refinement_level+1));

  sdc_nodes* nodes = sdc_nodes_create(nnodes, SDC_UNIFORM);
  sdc_imex*  imex  = sdc_imex_create(nodes, sdc_f1eval, sdc_f2eval, sdc_f2comp);

  // XXX: for fine levels, need to make the integration tables etc local only

  sdc_imex_setup(imex, NULL, NULL);
  sdc_hooks_add(imex->hooks, SDC_HOOK_POST_STEP, sdc_poststep_hook);
  sdc_nodes_destroy(nodes);
  return (sdc_sweeper*) imex;
}

/*
 * Rebuild MLSDC hierarchy.
 *
 * Note that XXX.
 */
void SDCAmr::rebuild_mlsdc()
{
  BL_PROFILE("SDCAmr::rebuild_mlsdc()");

  // reset previous and clear sweepers etc
  sdc_mg_reset(&mg);
  for (unsigned int lev=0; lev<=max_level; lev++) {
    if (sweepers[lev] != NULL) {
      sweepers[lev]->destroy(sweepers[lev]); sweepers[lev] = NULL;
      destroy_encap(lev);
    }
  }

  // rebuild
  for (int lev=0; lev<=finest_level; lev++) {
    encaps[lev] = build_encap(lev);
    sweepers[lev] = build_mlsdc_level(lev);
    sweepers[lev]->nset->ctx = &getLevel(lev);
    sweepers[lev]->nset->encap = encaps[lev];
    sdc_mg_add_level(&mg, sweepers[lev], mlsdc_amr_interpolate, mlsdc_amr_restrict);
  }
  sdc_mg_setup(&mg);
  sdc_mg_allocate(&mg);

  // XXX: for fine levels, need to make the interpolation matrices local only

  if (verbose > 0 && ParallelDescriptor::IOProcessor()) {
    cout << "Rebuilt MLSDC: " << mg.nlevels << ", nnodes: ";
    for (int lev=0; lev<=finest_level; lev++)
      cout << sweepers[lev]->nset->nnodes << " ";
    cout << endl;
  }
}

/*
 * Initialize SDC multigrid sweeper, set parameters.
 */
SDCAmr::SDCAmr ()
{
  ParmParse ppsdc("mlsdc");
  if (!ppsdc.query("max_iters", max_iters)) max_iters = 8;
  if (!ppsdc.query("max_trefs", max_trefs)) max_trefs = 3;
  if (!ppsdc.query("nnodes0",   nnodes0))   nnodes0 = 3;
  if (!ppsdc.query("trat",      trat))      trat = 2;

  // sdc_log_set_stdout(SDC_LOG_DEBUG);
  sdc_mg_build(&mg, max_level+1);
  sdc_hooks_add(mg.hooks, SDC_HOOK_POST_TRANS, sdc_poststep_hook);
  // sdc_hooks_add(mg.hooks, SDC_HOOK_POST_FAS,   sdc_postfas_hook);

  sweepers.resize(max_level+1);
  encaps.resize(max_level+1);

  for (int i=0; i<=max_level; i++) sweepers[i] = NULL;

  if (max_level > 0) {
      for (int i=0; i<=max_level; i++) {
	  if (blockingFactor(i) < 4) {
	      BoxLib::Abort("For AMR runs, set blocking_factor to at least 4.");
	  }
      }
  }
}

SDCAmr::~SDCAmr()
{
  sdc_mg_destroy(&mg);
}
