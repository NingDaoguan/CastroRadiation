// functions specific for SingleGroupSolver (SolverType 0)

#include <LO_BCTYPES.H>

#include "Radiation.H"
#include "RadSolve.H"

#include "Castro_F.H"

#undef BL_USE_ARLIM

#include "RAD_F.H"

#include <Using.H>

void Radiation::single_group_update(int level, int iteration, int ncycle)
{
  BL_PROFILE("Radiation::single_group_update");
  if (verbose && ParallelDescriptor::IOProcessor()) {
    cout << "Radiation implicit update, level " << level << "..." << endl;
  }

  bool has_dcoefs = (Radiation::SolverType == Radiation::SGFLDSolver &&
		     Radiation::Er_Lorentz_term);

  int group = 0;
  int fine_level = parent->finestLevel();

  // Allocation and initialization:

  Castro *castro = dynamic_cast<Castro*>(&parent->getLevel(level));
  const BoxArray& grids = castro->boxArray();
  Real delta_t          = parent->dtLevel(level);
  Real time = castro->get_state_data(Rad_Type).curTime();

  MultiFab& S_new = castro->get_new_data(State_Type);
  MultiFab& Er_new = castro->get_new_data(Rad_Type);

  MultiFab Er_old(grids, Er_new.nComp(), Er_new.nGrow());
  Er_old.copy(Er_new); // all components, including any first moments

  Tuple<MultiFab, BL_SPACEDIM> Ff_new;

  for (int idim = 0; idim < BL_SPACEDIM; idim++) {
      BoxArray edge_boxes(grids);
      edge_boxes.surroundingNodes(idim);
      Ff_new[idim].define(edge_boxes, 1, 0, Fab_allocate);
  }

  MultiFab Dterm;  
  if (has_dcoefs) {
    Dterm.define(grids, BL_SPACEDIM, 0, Fab_allocate);
  }

  MultiFab frhoem(grids,1,0);
  MultiFab frhoes(grids,1,0);

  MultiFab temp(grids,1,0);
  MultiFab fkp(grids,1,0);

  MultiFab& dflux_old = dflux[level];
  MultiFab dflux_new(grids,1,0);

  MultiFab Er_lim; // will only be allocated if needed

#ifdef _OPENMP
#pragma omp parallel
#endif
  for (MFIter mfi(frhoem,true); mfi.isValid(); ++mfi) {
      const Box& reg = mfi.tilebox();
      get_frhoe(frhoem[mfi], S_new[mfi], reg);
      frhoes[mfi].copy(frhoem[mfi],reg);
  }

  // Rosseland mean in grid interiors can be updated within the loop,
  // but ghost cell values are set once and never updated.

  MultiFab kappa_r(grids,1,1);    // note ghost cell, needs to be filled

  MultiFab velo;
  MultiFab dcfactor; // 2. * (1-eta) * kappa_p/kappa_r
  if (has_dcoefs) {
    velo.define(grids, BL_SPACEDIM, 1, Fab_allocate);
    dcfactor.define(grids, 1, 1, Fab_allocate);
    get_rosseland_v_dcf(kappa_r, velo, dcfactor, delta_t, c, castro);
  }
  else {
    get_rosseland(kappa_r, castro); // fills everywhere, incl ghost cells
  }

  MultiFab eta(grids,1,0);
  MultiFab etainv(grids,1,0);  // this is 1-eta, to avoid loss of accuracy

  Tuple<MultiFab, BL_SPACEDIM> lambda;

  for (int idim = 0; idim < BL_SPACEDIM; idim++) {
    BoxArray edge_boxes(grids);
    edge_boxes.surroundingNodes(idim);
    lambda[idim].define(edge_boxes, 1, 0, Fab_allocate);
  }

  if (update_limiter == 0) {
    scaledGradient(level, lambda, kappa_r, 0, Er_old, 0, limiter);
    // lambda now contains scaled gradient

    fluxLimiter(level, lambda, limiter);
    // lambda now contains flux limiter
  }
  else if (update_limiter < 0) {
    MultiFab& Er_lag = castro->get_old_data(Rad_Type);
    scaledGradient(level, lambda, kappa_r, 0, Er_lag, 0, limiter);
    fluxLimiter(level, lambda, limiter);
  }

  // Implicit update loop:

  RadBndry bd(grids, castro->Geom());

  getBndryData(bd, Er_new, time, level);

  bool have_Sanchez_Pomraning = false;
  int lo_bc[3]={0}, hi_bc[3]={0};
  for (int idim=0; idim<BL_SPACEDIM; idim++) {
    lo_bc[idim] = rad_bc.lo(idim);
    hi_bc[idim] = rad_bc.hi(idim);
    if (lo_bc[idim] == LO_SANCHEZ_POMRANING || 
	hi_bc[idim] == LO_SANCHEZ_POMRANING) {
      have_Sanchez_Pomraning = true;
    }
  }

  FluxRegister* flux_in  =
    (level < fine_level) ? &flux_trial[level+1] : NULL;
  FluxRegister* flux_out =
    (level > 0) ? &flux_trial[level]   : NULL;

  RadSolve solver(parent);
  solver.levelInit(level);
  solver.levelBndry(bd);

  Real relative, absolute;
  int it = 0;
  int use_conservative_form = (matter_update_type == 0) ? 1 : 0;
  int converged;
  Real underrel = 1.0;
  do {

    it++;

    // get new-time temperature and opacity from frhoes:
    temp.copy(frhoes);

    if (it == 1 || it <= update_planck + 1) {
      get_planck_and_temp(fkp, temp, S_new);
    }
    else {
      // eta is just scratch space here, but still want updated temp
      get_planck_and_temp(eta, temp, S_new);
    }

    // The test (it > update_planck) in compute_eta becomes true
    // when we have just updated fkp for the last time.  When this
    // test is true it means we will not be updating fkp again.

    compute_eta(eta, etainv, S_new, temp, fkp, Er_new,
		delta_t, c, underrel, it > update_planck);

    underrel *= underfac;

    if (it > 1 && it <= update_rosseland + 1) {
      // only updates cells in interior of fine level:
      update_rosseland_from_temp(kappa_r, temp, S_new, castro->Geom());
    }

    // solve linear system:

    solver.levelACoeffs(level, fkp, eta, etainv, c, delta_t, 1.0);

    if (update_limiter > 0 && it <= update_limiter + 1) {
      scaledGradient(level, lambda, kappa_r, 0, Er_new, 0, limiter);
      // lambda now contains scaled gradient
      fluxLimiter(level, lambda, limiter);
      // lambda now contains flux limiter
    }

    solver.levelBCoeffs(level, lambda, kappa_r, 0, c);

    if (have_Sanchez_Pomraning) {
      solver.levelSPas(level, lambda, 0, lo_bc, hi_bc);
    }

    if (has_dcoefs) {
      if (it>1) {
	update_dcf(dcfactor, etainv, fkp, kappa_r, castro->Geom());
      }
      solver.levelDCoeffs(level, lambda, velo, dcfactor);
    }

    {
      MultiFab rhs(grids,1,0);

      dflux_new.setVal(0.0); // used as work space in place of edot
      solver.levelRhs(level, rhs, temp,
		      fkp, eta, etainv, frhoem, frhoes,
		      dflux_old, Er_old, dflux_new,
		      delta_t, sigma, 0.0, 1.0,
		      NULL);

      // If there is a sync source from the next finer level,
      // reflux it in:
      deferred_sync(level, rhs, 0);

      solver.levelSolve(level, Er_new, 0, rhs, 0.01);

      dflux_new.setVal(0.0);

      solver.levelFlux(level, Ff_new, Er_new, 0);

      if (has_dcoefs) {
	solver.levelDterm(level, Dterm, Er_new, 0);
      }

      // Ff_new is now the complete new-time flux

      solver.levelFluxReg(level, flux_in, flux_out, Ff_new, 0);
    }

    // do energy update:

    if (use_conservative_form) {
      // we don't need these at the same time, so we share space:
      MultiFab& exch = temp;  // temp will be overwritten

      compute_exchange(exch, Er_new, fkp);

      if (has_dcoefs) {
	internal_energy_update(relative, absolute,
			       frhoes, frhoem, eta, etainv,
			       dflux_old, dflux_new,
			       exch, Dterm, delta_t);
      }
      else {
	internal_energy_update(relative, absolute,
			       frhoes, frhoem, eta, etainv,
			       dflux_old, dflux_new,
			       exch, delta_t);
      }
    }
    else {
      nonconservative_energy_update(relative, absolute,
                                    frhoes, frhoem, eta, etainv,
                                    Er_new, dflux_old, dflux_new,
                                    temp, fkp, S_new, delta_t);
      if (has_dcoefs) {
	BoxLib::Error("Radiation::single_group_update: must do conservative energy update when has_dcoefs is true");
      }
    }

    if (verbose > 1 && ParallelDescriptor::IOProcessor()) {
      int oldprec = cout.precision(20);
      if (use_conservative_form)
	cout << "Radiation Update:  Iteration " << it;
      else
	cout << "Nonconservative:   Iteration " << it;
      cout << ", Relative = " << relative << endl;
      cout << "                              "
	   << "  Absolute = " << absolute << endl;
      cout.precision(oldprec);
    }

    if (verbose > 2) {
       for (MFIter fi(frhoes); fi.isValid(); ++fi) {
          if (grids[fi.index()].contains(spot)) {
             cout << "frhoes: ";
             for (int m=0; m<1; ++m) cout << frhoes[0](spot,m) << " ";
             cout << endl;
             break;
          }
       }
    }

    converged = (relative <= reltol || absolute <= abstol || it >= maxiter);

    if (converged && !use_conservative_form) {
      use_conservative_form = 1;
      converged = 0;
    }

    // The following is a hack for problems with little fluid energy
    // change but where the limiter is important:
    //converged = (limiter == 0) ? converged : (it == 15);

  } while (!converged);

  if (verbose == 1 && ParallelDescriptor::IOProcessor()) {
    int oldprec = cout.precision(20);
    cout << "Radiation Update:  Iteration " << it
        << ", Relative = " << relative << endl;
    cout << "                              "
        << "  Absolute = " << absolute << endl;
    cout.precision(oldprec);
  }

  if (it == maxiter &&
      (relative > reltol && absolute > abstol)) {
    if (verbose > 0 && ParallelDescriptor::IOProcessor()) {
      cout << "Implicit Update Failed to Converge" << endl;
      BoxLib::Abort("You Lose");
    }
  }

  solver.levelClear();

  // update flux registers:
  if (flux_in) {
    for (OrientationIter face; face; ++face) {
      Orientation ori = face();
      flux_cons[level+1][ori].linComb(1.0, -1.0,
                                      (*flux_in)[ori], group, group, 1);
    }
  }

  if (flux_out) {
    for (OrientationIter face; face; ++face) {
      Orientation ori = face();
      flux_cons[level][ori].linComb(1.0, 1.0 / ncycle,
                                    (*flux_out)[ori], group, group, 1);
    }
  }

  if (level == fine_level && iteration == 1) {

    // We've now advanced the finest level, so we've just passed the
    // last time we might want to reflux from any existing flux_cons_old
    // on any level until the next sync.  Setting the stored delta_t
    // to 0.0 is a hack equivalent to marking each corresponding
    // register as "not to be used".

    for (int flev = level; flev > 0; flev--) {
      delta_t_old[flev-1] = 0.0;
    }

    // The following would only apply if a finer level has
    // recently been deleted.

    for (int flev = level+1; flev <= parent->maxLevel(); flev++) {
      if (flux_cons_old.defined(flev)) {
        delete flux_cons_old.remove(flev);
      }
    }
  }

  // update fluid energy based on frhoes:
  state_update(S_new, frhoes, temp);

  if (verbose > 2) {
     for (MFIter fi(frhoes); fi.isValid(); ++fi) {
        if (grids[fi.index()].contains(spot)) {
           cout << "frhoes: ";
           for (int m=0; m<1; ++m) cout << frhoes[0](spot,m) << " ";
           cout << endl;
           break;
        }
     }
  }

  // update dflux[level] (== dflux_old)
  MultiFab::Copy(dflux_old, dflux_new, 0, 0, 1, 0);

  if (plot_lambda) {
#ifdef _OPENMP
#pragma omp parallel
#endif
      for (MFIter mfi(plotvar[level],true); mfi.isValid(); ++mfi) {
	  const Box& bx = mfi.tilebox();
	  int scomp = 0;
	  BL_FORT_PROC_CALL(CA_FACE2CENTER, ca_face2center)
	      (bx.loVect(), bx.hiVect(),
	       scomp, icomp_lambda, nGroups, nGroups, nplotvar,
	       D_DECL(BL_TO_FORTRAN(lambda[0][mfi]),
		      BL_TO_FORTRAN(lambda[1][mfi]),
		      BL_TO_FORTRAN(lambda[2][mfi])),
	       BL_TO_FORTRAN(plotvar[level][mfi]));
      }
  }

  if (plot_flux) {
      solver.levelFluxFaceToCenter(level, Ff_new, plotvar[level], icomp_flux);
      if (comoving && plot_lab_flux) {
	  // xxxxxxxxxx
      }
  }

  if (plot_kappa_p) {
      MultiFab::Copy(plotvar[level], fkp, 0, icomp_kp, 1, 0);
  }

  if (plot_kappa_r) {
      MultiFab::Copy(plotvar[level], kappa_r, 0, icomp_kr, 1, 0);
  }

  if (verbose && ParallelDescriptor::IOProcessor()) {
    cout << "                                     done" << endl;
  }
}
