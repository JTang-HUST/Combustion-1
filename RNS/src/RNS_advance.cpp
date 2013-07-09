#include <winstd.H>

#include "RNS.H"
#include "RNS_F.H"

using std::string;

Real
RNS::advance (Real time,
	      Real dt,
	      int  iteration,
	      int  ncycle)
{
    for (int k = 0; k < NUM_STATE_TYPE; k++) 
    {
        state[k].allocOldData();
        state[k].swapTimeLevels(dt);
    }

    MultiFab& Unew = get_new_data(State_Type);
    MultiFab& Uold = get_old_data(State_Type);

    RNSFillType fill_boundary_type;

    if (level == 0)
    {
	fill_boundary_type = use_FillBoundary;
	MultiFab::Copy(Unew, Uold, 0, 0, NUM_STATE, 0);
    }
    else
    {
	fill_boundary_type = use_FillPatchIterator;
    }
    fill_boundary(Unew, time, fill_boundary_type);
    
    // do half-dt chemistry
    advance_chemistry(Unew, 0.5*dt);

    // Advance Advection & Diffusion
    advance_AD(Uold, Unew, time, dt); 

    // do another half-dt chemistry
    advance_chemistry(Unew, 0.5*dt);
    
    if (Unew.contains_nan(0,NUM_STATE,0))
    {
	for (int i=0; i<NUM_STATE; i++)
	{
	    if (Unew.contains_nan(i, 1, 0))
	    {
		std::cout << "RNS::advance: Testing component i for NaNs: " << i << std::endl;
                BoxLib::Abort("RNS::advance: Has NaNs in this component.");
	    }
	}
    }
    
    return dt;
}


void
RNS::fill_boundary(MultiFab& U, Real time, RNSFillType type)
{
    BL_ASSERT( level > 0 || type == use_FillBoundary );

    switch (type)
    {
    case use_FillPatchIterator:

	for (FillPatchIterator fpi(*this, U, NUM_GROW, time, State_Type, 0, NUM_STATE); 
	     fpi.isValid(); ++fpi) 
	{
	    U[fpi].copy(fpi());
	}

	break;

    case use_FillCoarsePatch:
    {

	const Box& domain_box = geom.Domain();
	BoxArray grids_g(grids);
	for (int ibox=0; ibox<grids_g.size(); ibox++)
	{
	    const Box b = BoxLib::grow(grids_g[ibox], NUM_GROW) & domain_box;
	    grids_g.set(ibox, b);
	}
	
	MultiFab Utmp(grids_g, NUM_STATE, 0);
	FillCoarsePatch(Utmp, 0, time, State_Type, 0, NUM_STATE);

	for (MFIter mfi(U); mfi.isValid(); ++mfi)
	{
	    int i = mfi.index();

	    const Box& vbox = grids[i];
	    const Box& gbox = Utmp[i].box();
	    const BoxArray& ba = BoxLib::boxComplement(gbox, vbox);
	    
	    for (int ibox=0; ibox<ba.size(); ibox++)
	    {
		U[i].copy(Utmp[i], ba[ibox]);
	    }
	}

	// no break; so it will go to next case and call FillBoundary

    }
    case use_FillBoundary:

	U.FillBoundary();
	geom.FillPeriodicBoundary(U, true);
	for (MFIter mfi(U); mfi.isValid(); ++mfi)
	{
	    setPhysBoundaryValues(U[mfi], 
				  State_Type,
				  time,
				  0,
				  0,
				  NUM_STATE);
	}

	break;
    }
}


void
RNS::dUdt(MultiFab& U, MultiFab& Uprime, Real time, RNSFillType fill_boundary_type, 
	  FluxRegister* fine, FluxRegister* current, Real dt)
{
    FArrayBox  flux[BL_SPACEDIM];
    MultiFab fluxes[BL_SPACEDIM];
    if (do_reflux && fine) 
    {
	for (int idim = 0; idim < BL_SPACEDIM; idim++) 
	{
	    BoxArray ba = U.boxArray();
	    ba.surroundingNodes(idim);
	    fluxes[idim].define(ba, NUM_STATE, 0, Fab_allocate);
	}
    }

    fill_boundary(U, time, fill_boundary_type);
    
    const Real *dx = geom.CellSize();

    for (MFIter mfi(Uprime); mfi.isValid(); ++mfi)
    {
	int i = mfi.index();
	const Box& bx = mfi.validbox();

	for (int idim = 0; idim < BL_SPACEDIM ; idim++) 
	{
	    flux[idim].resize(BoxLib::surroundingNodes(bx,idim),NUM_STATE);
	}

	BL_FORT_PROC_CALL(RNS_DUDT,rns_dudt)
	    (bx.loVect(), bx.hiVect(),
	     BL_TO_FORTRAN(U[i]),
	     BL_TO_FORTRAN(Uprime[i]),
	     D_DECL(BL_TO_FORTRAN(flux[0]), 
		    BL_TO_FORTRAN(flux[1]), 
		    BL_TO_FORTRAN(flux[2])), 
	     dx);

	if (do_reflux) 
	{
	    if (fine) 
	    {
		for (int idim = 0; idim < BL_SPACEDIM ; idim++) 
		{
		    fluxes[idim][i].copy(flux[idim]);
		}
	    }

	    if (current) 
	    {
		for (int idim = 0; idim < BL_SPACEDIM ; idim++) 
		{
		    current->FineAdd(flux[idim],area[idim][i],idim,i,0,0,NUM_STATE,dt);
		}
	    }
	}
    }

    if (do_reflux && fine) 
    {
	for (int idim = 0; idim < BL_SPACEDIM ; idim++) 
	{
	    fine->CrseInit(fluxes[idim],area[idim],idim,0,0,NUM_STATE,-dt);
	}
    }
}


// Compute U1 = U2 + c Uprime.
void 
RNS::update_rk(MultiFab& U1, const MultiFab& U2, Real c, const MultiFab& Uprime)
{
    for (MFIter mfi(U1); mfi.isValid(); ++mfi)
    {
	const int   i = mfi.index();
	const Box& bx = mfi.validbox();
	
	U1[i].copy(U2[i], bx);
	U1[i].saxpy(c, Uprime[i]);
    }
}

// Compute U = a Ua + b Ub + c Uprime.
void 
RNS::update_rk(MultiFab& U, Real a, const MultiFab& Ua, Real b, const MultiFab& Ub,
	       Real c, const MultiFab& Uprime)
{
    for (MFIter mfi(U); mfi.isValid(); ++mfi)
    {
	const int   i = mfi.index();

	U[i].setVal(0.0);
	U[i].saxpy(a, Ua[i]);
	U[i].saxpy(b, Ub[i]);
	U[i].saxpy(c, Uprime[i]);
    }    
}


void
RNS::post_update(MultiFab& U)
{
    for (MFIter mfi(U); mfi.isValid(); ++mfi)
    {
	const int   i = mfi.index();
	const Box& bx = mfi.validbox();
        const int* lo = bx.loVect();
        const int* hi = bx.hiVect();

	BL_FORT_PROC_CALL(RNS_ENFORCE_CONSISTENT_Y, rns_enforce_consistent_y)
	    (lo, hi, BL_TO_FORTRAN(U[i]));

	BL_FORT_PROC_CALL(RNS_COMPUTE_TEMP, rns_compute_temp)
	    (lo, hi, BL_TO_FORTRAN(U[i]));	
    }
}


void
RNS::advance_chemistry(MultiFab& U, Real dt)
{
    if (! chemSolve->isNull)
    {
	for (MFIter mfi(U); mfi.isValid(); ++mfi)
	{
	    const int   i = mfi.index();
	    const Box& bx = mfi.validbox();
	    const int* lo = bx.loVect();
	    const int* hi = bx.hiVect();
	    
	    BL_FORT_PROC_CALL(RNS_ADVCHEM, rns_advchem)
		(lo, hi, BL_TO_FORTRAN(U[i]), dt);
	}
    }

    post_update(U);
}


void
RNS::advance_AD(const MultiFab& Uold, MultiFab& Unew, Real time, Real dt)
{
    MultiFab Uprime(grids,NUM_STATE,0);

    if (RK_order == 2)
    {
	int finest_level = parent->finestLevel();
	
	//
	// Get pointers to Flux registers, or set pointer to zero if not there.
	//
	FluxRegister *fine    = 0;
	FluxRegister *current = 0;
        
	if (do_reflux && level < finest_level) {
	    fine = &getFluxReg(level+1);
	    fine->setVal(0.0);
	}
	if (do_reflux && level > 0) {
	    current = &getFluxReg(level);
	}

	RNSFillType fill_boundary_type = (level == 0) ? use_FillBoundary : use_FillCoarsePatch;
	
	// Step 1 of RK2
	dUdt(Unew, Uprime, time, fill_boundary_type);
	update_rk(Unew, Uold, 0.5*dt, Uprime); // Unew = Uold + 0.5*dt*Uprime
	post_update(Unew);
	
	// Step 2 of RK2
	dUdt(Unew, Uprime, time+0.5*dt, fill_boundary_type, fine, current, dt);
	update_rk(Unew, Uold, dt, Uprime); // Unew = Uold + dt*Uprime
	post_update(Unew);
    }
    else if (RK_order == 3)
    {
	BL_ASSERT(level==0);

	RNSFillType fill_boundary_type = use_FillBoundary;
	MultiFab Utmp(grids,NUM_STATE,NUM_GROW);

	// Step 1 of RK3
	dUdt(Unew, Uprime, time, fill_boundary_type);
	update_rk(Unew, Uold, dt, Uprime);
	post_update(Unew);	

	// Step 2 of RK3
	dUdt(Unew, Uprime, time+(1./3.)*dt, fill_boundary_type);
	update_rk(Utmp, 0.75, Uold, 0.25, Unew, 0.25*dt, Uprime);
	post_update(Utmp);	
	
	// Step 3 of RK3
	dUdt(Utmp, Uprime, time+(2./3.)*dt, fill_boundary_type);
	update_rk(Unew, 1./3., Uold, 2./3., Utmp, (2./3.)*dt, Uprime);
	post_update(Unew);	
    }
}
