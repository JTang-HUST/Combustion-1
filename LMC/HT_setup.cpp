//
// $Id: HT_setup.cpp,v 1.1 2003-08-08 20:09:40 lijewski Exp $
//
// Note: define TEMPERATURE if you want variables T and rho*h, h = c_p*T,in the 
//       State_Type part of the state
//       whose component indices are Temp and RhoH
//       whose evoution equations are 
//       \pd (rho h)/\pd t + diver (\rho U h) = div k grad T
//       rho DT/dt = div k/c_p grad T
//       define RADIATION only if TEMPERATURE is also defined and the evolution equations
//       for T and rho*h are
//       \pd (rho h)/\pd t + diver (\rho U h) = div k grad T - div q_rad
//       rho DT/dt = div k/c_p grad T - 1/c_p div q_rad
//
//       The equation for temperature is used solely for computing div k grad T
//
//       Note: The reasons for using an auxiliary T equations are two fold:
//             1) solving the C-N difference equations for 
//                \pd (rho h)/\pd t + diver (\rho U h) = div k/c_p grad h
//                does not easily fit into our framework as of 10/31/96
//             2) the boundary condition for rho h at a wall is ill defined
//
// "Divu_Type" means S, where divergence U = S
// "Dsdt_Type" means pd S/pd t, where S is as above
// "Dqrad_Type" means diver q_rad, i.e., the divergence of the radiative heat flux
//
// see variableSetUp on how to use or not use these types in the state
//
#include <winstd.H>

#include <algorithm>
#include <cstdio>
#include <iomanip>

#include <HeatTransfer.H>
#include <RegType.H>
#include <ParmParse.H>
#include <ErrorList.H>
#include <PROB_F.H>
#include <DERIVE_F.H>
#include <FArrayBox.H>
#include <CoordSys.H>
#include <NAVIERSTOKES_F.H>
#include <HEATTRANSFER_F.H>
#include <SLABSTAT_HT_F.H>

#define DEF_LIMITS(fab,fabdat,fablo,fabhi)   \
const int* fablo = (fab).loVect();           \
const int* fabhi = (fab).hiVect();           \
Real* fabdat = (fab).dataPtr();
#define DEF_CLIMITS(fab,fabdat,fablo,fabhi)  \
const int* fablo = (fab).loVect();           \
const int* fabhi = (fab).hiVect();           \
const Real* fabdat = (fab).dataPtr();

static Box the_same_box (const Box& b)    { return b;                 }
static Box grow_box_by_one (const Box& b) { return BoxLib::grow(b,1); }

static bool do_group_bndry_fills = false;

//
// Components are  Interior, Inflow, Outflow, Symmetry, SlipWall, NoSlipWall.
//

static
int
norm_vel_bc[] =
{
//  INT_DIR, EXT_DIR, HOEXTRAP, REFLECT_ODD, EXT_DIR, EXT_DIR
    INT_DIR, EXT_DIR, FOEXTRAP, REFLECT_ODD, EXT_DIR, EXT_DIR
};

static
int
tang_vel_bc[] =
{
//  INT_DIR, EXT_DIR, HOEXTRAP, REFLECT_EVEN, HOEXTRAP, EXT_DIR
    INT_DIR, EXT_DIR, FOEXTRAP, REFLECT_EVEN, HOEXTRAP, EXT_DIR
};

static
int
scalar_bc[] =
{
    INT_DIR, EXT_DIR, FOEXTRAP, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN
};

//
// Here we use slipwall to impose neumann condition, not to represent a real wall
//
static
int
temp_bc[] =
{
    INT_DIR, EXT_DIR, FOEXTRAP, REFLECT_EVEN, REFLECT_EVEN, EXT_DIR
};

static
int
press_bc[] =
{
    INT_DIR, FOEXTRAP, FOEXTRAP, REFLECT_EVEN, FOEXTRAP, FOEXTRAP
};

static
int
rhoh_bc[] =
{
    INT_DIR, EXT_DIR, FOEXTRAP, REFLECT_EVEN, REFLECT_EVEN, EXT_DIR
};

static
int
divu_bc[] =
{
    INT_DIR, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN
};

static
int
dsdt_bc[] =
{
    INT_DIR, EXT_DIR, EXT_DIR, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN
};

static
int
reflect_bc[] =
{
    REFLECT_EVEN,REFLECT_EVEN,REFLECT_EVEN,REFLECT_EVEN,REFLECT_EVEN,REFLECT_EVEN
};

static
int
dqrad_bc[] =
{
    INT_DIR, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN
};

static
int
species_bc[] =
{
    INT_DIR, EXT_DIR, FOEXTRAP, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN
};

#if 0
static
RegType
project_bc[] =
{
    interior, inflow, outflow, refWall, refWall, refWall
};
#endif

static
void
set_x_vel_bc (BCRec&       bc,
              const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    bc.setLo(0,norm_vel_bc[lo_bc[0]]);
    bc.setHi(0,norm_vel_bc[hi_bc[0]]);
    bc.setLo(1,tang_vel_bc[lo_bc[1]]);
    bc.setHi(1,tang_vel_bc[hi_bc[1]]);
#if (BL_SPACEDIM == 3)
    bc.setLo(2,tang_vel_bc[lo_bc[2]]);
    bc.setHi(2,tang_vel_bc[hi_bc[2]]);
#endif
}

static
void
set_y_vel_bc (BCRec&       bc,
              const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    bc.setLo(0,tang_vel_bc[lo_bc[0]]);
    bc.setHi(0,tang_vel_bc[hi_bc[0]]);
    bc.setLo(1,norm_vel_bc[lo_bc[1]]);
    bc.setHi(1,norm_vel_bc[hi_bc[1]]);
#if (BL_SPACEDIM == 3)
    bc.setLo(2,tang_vel_bc[lo_bc[2]]);
    bc.setHi(2,tang_vel_bc[hi_bc[2]]);
#endif
}

#if (BL_SPACEDIM == 3)
static
void
set_z_vel_bc (BCRec&       bc,
              const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    bc.setLo(0,tang_vel_bc[lo_bc[0]]);
    bc.setHi(0,tang_vel_bc[hi_bc[0]]);
    bc.setLo(1,tang_vel_bc[lo_bc[1]]);
    bc.setHi(1,tang_vel_bc[hi_bc[1]]);
    bc.setLo(2,norm_vel_bc[lo_bc[2]]);
    bc.setHi(2,norm_vel_bc[hi_bc[2]]);
}
#endif

static
void
set_scalar_bc (BCRec&       bc,
               const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
	bc.setLo(i,scalar_bc[lo_bc[i]]);
	bc.setHi(i,scalar_bc[hi_bc[i]]);
    }
}

static
void
set_reflect_bc (BCRec&       bc,
                const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
	bc.setLo(i,reflect_bc[lo_bc[i]]);
	bc.setHi(i,reflect_bc[hi_bc[i]]);
    }
}

static
void
set_temp_bc (BCRec&       bc,
             const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
	bc.setLo(i,temp_bc[lo_bc[i]]);
	bc.setHi(i,temp_bc[hi_bc[i]]);
    }
}

static
void
set_pressure_bc (BCRec&       bc,
                 const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    int i;
    for (i = 0; i < BL_SPACEDIM; i++)
    {
        bc.setLo(i,press_bc[lo_bc[i]]);
        bc.setHi(i,press_bc[hi_bc[i]]);
    }
}

static
void
set_rhoh_bc (BCRec&       bc,
             const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
	bc.setLo(i,rhoh_bc[lo_bc[i]]);
	bc.setHi(i,rhoh_bc[hi_bc[i]]);
    }
}

static
void
set_divu_bc (BCRec&       bc,
             const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
	bc.setLo(i,divu_bc[lo_bc[i]]);
	bc.setHi(i,divu_bc[hi_bc[i]]);
    }
}

static
void
set_dsdt_bc (BCRec&       bc,
             const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
	bc.setLo(i,dsdt_bc[lo_bc[i]]);
	bc.setHi(i,dsdt_bc[hi_bc[i]]);
    }
}

static
void
set_dqrad_bc (BCRec&       bc,
              const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
	bc.setLo(i,dqrad_bc[lo_bc[i]]);
	bc.setHi(i,dqrad_bc[hi_bc[i]]);
    }
}

static
void
set_species_bc (BCRec&       bc,
                const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
	bc.setLo(i,species_bc[lo_bc[i]]);
	bc.setHi(i,species_bc[hi_bc[i]]);
    }
}

void
NavierStokes::variableSetUp ()
{
    BoxLib::Error("NavierStokes::variableSetUp(): should not be here");
}

typedef StateDescriptor::BndryFunc BndryFunc;

extern "C"
{
//
// Function called by BCRec for user-supplied boundary data.
//
typedef void (*ChemBndryFunc_FortBndryFunc)(Real* data, ARLIM_P(lo), ARLIM_P(hi),
					    const int* dom_lo, const int* dom_hi,
					    const Real* dx, const Real* grd_lo,
					    const Real* time, const int* bc,
					    const int* stateID);
}

class ChemBndryFunc
    :
    public BndryFunc
{
public:
    //
    // Bogus constructor.
    //
    ChemBndryFunc()
	:
        m_func(0),
        m_stateID(-1) {}
    //
    // Constructor.
    //
    ChemBndryFunc (ChemBndryFunc_FortBndryFunc  inFunc,
                   const std::string& stateName)
	:
        m_func(inFunc),
        m_stateName(stateName)
    {
	m_stateID = getStateID(m_stateName);
	BL_ASSERT(m_stateID >= 0);
    }
    //
    // Another Constructor which sets "regular" and "group" fill routines..
    //
    ChemBndryFunc (ChemBndryFunc_FortBndryFunc  inFunc,
                   const std::string&           stateName,
                   BndryFuncDefault             gFunc)
	:
        BndryFunc(gFunc,gFunc),
        m_func(inFunc),
        m_stateName(stateName)
    {
	m_stateID = getStateID(m_stateName);
	BL_ASSERT(m_stateID >= 0);
    }
    //
    // Destructor.
    //
    virtual ~ChemBndryFunc () {}
    
    virtual StateDescriptor::BndryFunc* clone () const
    {
        //
	// Bitwise copy ok here, no copy ctr required.
        //
	return new ChemBndryFunc(*this);
    }
    //
    // Fill boundary cells the "regular" way.
    // The other virtual function in BndryFunc will
    // give us the appropriate call for "group" fills.
    //
    virtual void operator () (Real* data, const int* lo, const int* hi,
			      const int* dom_lo, const int* dom_hi,
			      const Real* dx, const Real* grd_lo,
			      const Real* time, const int* bc) const
    {
	BL_ASSERT(m_func != 0);
	m_func(data,ARLIM(lo),ARLIM(hi),dom_lo,dom_hi,dx,grd_lo,time,bc,&m_stateID);
    }
    //
    // Access.
    //
    int getStateID () const              { return m_stateID;   }
    const std::string& getStateName () const { return m_stateName; }
    ChemBndryFunc_FortBndryFunc getBndryFunc () const  { return m_func;      }
    
protected:

    static int getStateID (const std::string& stateName)
    {
	const Array<std::string>& names = HeatTransfer::getChemSolve().speciesNames();
	for (int i=0; i<names.size(); i++)
	    if (names[i] == stateName)
		return i;
	return -1;
    }

private:

    ChemBndryFunc_FortBndryFunc m_func;
    std::string       m_stateName;
    int           m_stateID;
};

//
// Indices of fuel and oxidizer -- ParmParsed in & used in a couple places.
//
std::string HeatTransfer::fuelName = "CH4";
static std::string oxidizerName = "O2";
static std::string productName  = "CO2";

void
HeatTransfer::variableSetUp ()
{
    BL_ASSERT(desc_lst.size() == 0);

    int i;

    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
	phys_bc.setLo(dir,SlipWall);
	phys_bc.setHi(dir,SlipWall);
    }

    read_params();
    BCRec bc;
    //
    // Set state variable Id's (Density, velocities and Temp set already).
    //
    int counter = Density;
    int RhoH = -1;
    int FirstSpec = -1;
    int Trac = -1;
    int RhoRT = -1;
    FirstSpec = ++counter;
    nspecies = getChemSolve().numSpecies();
    counter += nspecies - 1;
    if (do_temp)
	RhoH = ++counter;
    Trac = ++counter;
    if (do_temp)
    {
	Temp = ++counter;
        //RhoRT = ++counter;
    }
    NUM_STATE = ++counter;
    NUM_SCALARS = NUM_STATE - Density;

    const Array<std::string>& names = getChemSolve().speciesNames();

    if (ParallelDescriptor::IOProcessor())
    {
        std::cout << nspecies << " Chemical species interpreted:\n { ";
        for (int i = 0; i < nspecies; i++)
            std::cout << names[i] << ' ' << ' ';
        std::cout << '}' << '\n' << '\n';
    }
    //
    // Send indices of fuel and oxidizer to fortran for setting prob data in common block
    //
    ParmParse pp("ns");
    pp.query("fuelName",fuelName);
    pp.query("oxidizerName",oxidizerName);
    pp.query("productName",productName);
    pp.query("do_group_bndry_fills",do_group_bndry_fills);
    //
    // Set scale of chemical components, used in ODE solves
    //
    std::string speciesScaleFile; pp.query("speciesScaleFile",speciesScaleFile);
    if (! speciesScaleFile.empty())
    {
        if (ParallelDescriptor::IOProcessor())
            std::cout << "  Setting scale values for chemical species\n\n";
        
        getChemSolve().set_species_Yscales(speciesScaleFile);
    }
    int ncycle_vode=15000; pp.query("ncycle_vode",ncycle_vode);
    int verbose_vode=0; pp.query("verbose_vode",verbose_vode);
    getChemSolve().set_max_vode_subcycles(ncycle_vode);
    if (verbose_vode!=0)
        getChemSolve().set_verbose_vode();

    int fuelID = getChemSolve().index(fuelName);
    int oxidID = getChemSolve().index(oxidizerName);
    int prodID = getChemSolve().index(productName);
    FORT_SET_PROB_SPEC(&fuelID, &oxidID, &prodID, &nspecies);
    //
    // Get a species to use as a flame tracker.
    //
    std::string flameTracName = fuelName;
    pp.query("flameTracName",flameTracName);    
    //
    // **************  DEFINE VELOCITY VARIABLES  ********************
    //
    desc_lst.addDescriptor(State_Type,IndexType::TheCellType(),StateDescriptor::Point,1,NUM_STATE,
			   &cell_cons_interp);

    Array<BCRec>       bcs(BL_SPACEDIM);
    Array<std::string> name(BL_SPACEDIM);

    set_x_vel_bc(bc,phys_bc);
    bcs[0]  = bc;
    name[0] = "x_velocity";
    desc_lst.setComponent(State_Type,Xvel,"x_velocity",bc,BndryFunc(FORT_XVELFILL));

    set_y_vel_bc(bc,phys_bc);
    bcs[1]  = bc;
    name[1] = "y_velocity";
    desc_lst.setComponent(State_Type,Yvel,"y_velocity",bc,BndryFunc(FORT_YVELFILL));

#if(BL_SPACEDIM==3)
    set_z_vel_bc(bc,phys_bc);
    bcs[2]  = bc;
    name[2] = "z_velocity";
    desc_lst.setComponent(State_Type,Zvel,"z_velocity",bc,BndryFunc(FORT_ZVELFILL));
#endif
    //
    // To enable "group" operations on filling velocities, we need to
    // overwrite the first component specifing how to do "regular"
    // and "group" fill operations.
    //
    if (do_group_bndry_fills)
    {
        desc_lst.setComponent(State_Type,
                              Xvel,
                              name,
                              bcs,
                              BndryFunc(FORT_XVELFILL,FORT_VELFILL));
    }
    //
    // **************  DEFINE SCALAR VARIABLES  ********************
    //
    // Set range of combination limit to include rho, rhoh and species, if they exist
    //
    int combinLimit_lo = static_cast<int>(Density);
    int combinLimit_hi = std::max(combinLimit_lo, RhoH);
    set_scalar_bc(bc,phys_bc);
    desc_lst.setComponent(State_Type,Density,"density",bc,BndryFunc(FORT_DENFILL),
                          &cell_cons_interp);
    if (do_temp)
    {
        //
	// **************  DEFINE RHO*H  ********************
        //
	set_rhoh_bc(bc,phys_bc);
	desc_lst.setComponent(State_Type,RhoH,"rhoh",bc,BndryFunc(FORT_RHOHFILL),
                              &cell_cons_interp);
	//
	// **************  DEFINE TEMPERATURE  ********************
        //
	set_temp_bc(bc,phys_bc);
	desc_lst.setComponent(State_Type,Temp,"temp",bc,BndryFunc(FORT_TEMPFILL));
	//
	// **************  DEFINE RhoRT  ********************
        //
        //set_scalar_bc(bc,phys_bc);
        //desc_lst.setComponent(State_Type,RhoRT,"RhoRT",bc,BndryFunc(FORT_ADVFILL));
    }
    //
    // ***************  DEFINE SPECIES **************************
    //
    bcs.resize(nspecies);
    name.resize(nspecies);

    set_species_bc(bc,phys_bc);

    for (i = 0; i < nspecies; i++)
    {
        bcs[i]  = bc;
        name[i] = "rho.Y(" + names[i] + ")";

        desc_lst.setComponent(State_Type,
                              FirstSpec+i,
                              name[i].c_str(),
                              bc,
                              ChemBndryFunc(FORT_CHEMFILL,names[i]),
                              &cell_cons_interp);
    }
    //
    // To enable "group" operations on filling species, we need to
    // overwrite the first component specifing how to do "regular"
    // and "group" fill operations.
    //
//    if (do_group_bndry_fills)
    if (false)
    {        
        desc_lst.setComponent(State_Type,
                              FirstSpec,
                              name,
                              bcs,
                              ChemBndryFunc(FORT_CHEMFILL,names[0],FORT_ALLCHEMFILL),
                              &cell_cons_interp);
    }
    //
    // ***************  DEFINE TRACER **************************
    //
    if (RhoRT > 0)
    {
        set_scalar_bc(bc,phys_bc);
    }
    else
    {
        //
        // Force Trac BCs to be REFLECT_EVEN for RhoRT ghost cells in UGRADP.
        //
        set_reflect_bc(bc,phys_bc);
    }
    desc_lst.setComponent(State_Type,Trac,"tracer",bc,BndryFunc(FORT_ADVFILL));

    advectionType.resize(NUM_STATE);
    diffusionType.resize(NUM_STATE);
    is_diffusive.resize(NUM_STATE);
    // assume everything is diffusive and then change it if it is not.
    for (i = 0; i < NUM_STATE; i++)
    {
	advectionType[i] = NonConservative;
	diffusionType[i] = RhoInverse_Laplacian_S;
	is_diffusive[i] = true;
    }

    if (do_mom_diff == 1)
      for (int d = 0; d < BL_SPACEDIM; d++)
        advectionType[Xvel+d] = Conservative;

    is_diffusive[Density]=false;

    // check to see if diffuse tracer
    if (Trac > 0)
    {
	advectionType[Trac] = NonConservative;
	diffusionType[Trac] = Laplacian_S;
        if (trac_diff_coef <= 0.0)
            is_diffusive[Trac] = false;
    }

    advectionType[Density] = Conservative;
    diffusionType[Density] = Laplacian_SoverRho;
    if (do_temp)
    {
	advectionType[Temp] = NonConservative;
	diffusionType[Temp] = RhoInverse_Laplacian_S;
	advectionType[RhoH] = Conservative;
	diffusionType[RhoH] = Laplacian_SoverRho;
    }
    for (int i = 0; i < nspecies; ++i)
    {
	advectionType[FirstSpec + i] = Conservative;
	diffusionType[FirstSpec + i] = Laplacian_SoverRho;
    }

    if (is_diffusive[Density])
	BoxLib::Abort("HeatTransfer::variableSetUp(): density cannot diffuse");
    //
    // ---- pressure
    //
    // the #if 1 makes this a simpler problem CAR
#if 1
    desc_lst.addDescriptor(Press_Type,IndexType::TheNodeType(),
                           StateDescriptor::Interval,1,1,
			   &node_bilinear_interp);

    set_pressure_bc(bc,phys_bc);
    desc_lst.setComponent(Press_Type,Pressure,"pressure",bc,BndryFunc(FORT_PRESFILL));
#else
    desc_lst.addDescriptor(Press_Type,IndexType::TheNodeType(),
                           StateDescriptor::Point,1,1,
			   &node_bilinear_interp,true);

    set_pressure_bc(bc,phys_bc);
    desc_lst.setComponent(Press_Type,Pressure,"pressure",bc,BndryFunc(FORT_PRESFILL));

    //
    // ---- time derivative of pressure
    //
    Dpdt_Type = desc_lst.size();
    desc_lst.addDescriptor(Dpdt_Type,IndexType::TheNodeType(),
                           StateDescriptor::Interval,1,1,
                           &node_bilinear_interp);
    set_pressure_bc(bc,phys_bc);
    desc_lst.setComponent(Dpdt_Type,Dpdt,"dpdt",bc,BndryFunc(FORT_PRESFILL));
#endif

    //
    // ---- right hand side of divergence constraint.
    //
    int ngrow;
    //
    // Descriptors for divu, dsdt, dqrad
    // you can leave these out or mix and match with the contraint
    // that dsdt can be used only if divu is used
    //
    // If everything going into divu is zero, don't need to make space for divu
    bool zero_divu = (do_temp && is_diffusive[Temp]) && !do_DO_radiation
	&& !do_OT_radiation && (have_spec && is_diffusive[FirstSpec]);
    //
    // If zerodivu != 0, then we're going to hardwire something into divu
    // (if zerodivu == 1, hardwire zero, so don't need it)
    //
    bool hard_zero_divu = zerodivu == 1;

    if (!(zero_divu || hard_zero_divu))
    {
        //
	// stick Divu_Type on the end of the descriptor list.
        //
	Divu_Type = desc_lst.size();
	ngrow = 1;
	desc_lst.addDescriptor(Divu_Type,IndexType::TheCellType(),StateDescriptor::Point,ngrow,1,
			       &cell_cons_interp);
	set_divu_bc(bc,phys_bc);
	desc_lst.setComponent(Divu_Type,Divu,"divu",bc,BndryFunc(FORT_DIVUFILL));
	//
	// Stick Dsdt_Type on the end of the descriptor list.
        //
	if (usedsdt == 1)
	{
	    Dsdt_Type = desc_lst.size();
	    
	    ngrow = 0;
	    desc_lst.addDescriptor(Dsdt_Type,IndexType::TheCellType(),StateDescriptor::Point,ngrow,1,
				   &cell_cons_interp);
	    set_dsdt_bc(bc,phys_bc);
	    desc_lst.setComponent(Dsdt_Type,Dsdt,"dsdt",bc,BndryFunc(FORT_DSDTFILL));
	}
    }
    
    if (do_DO_radiation)
    {
	BL_ASSERT(do_temp);
	//
	// Stick Dqrad_Type on the end of the descriptor list.
        //
	Dqrad_Type = desc_lst.size();
        //
	// Note: there must be a component Temp in State_Type if
	// you are going to use radiation.
        //
	ngrow = 0;
	desc_lst.addDescriptor(Dqrad_Type,IndexType::TheCellType(),StateDescriptor::Point,ngrow,1,
			       &cell_cons_interp);
	set_dqrad_bc(bc,phys_bc);
	desc_lst.setComponent(Dqrad_Type,Dqrad,"dqrad",bc,BndryFunc(FORT_DQRADFILL));
    }

    // Add in the fcncall tracer type quantity FIXME???
    FuncCount_Type = desc_lst.size();
    desc_lst.addDescriptor(FuncCount_Type, IndexType::TheCellType(),StateDescriptor::Point,0, 1, &cell_cons_interp);
    desc_lst.setComponent(FuncCount_Type, 0, "FuncCount", bc, BndryFunc(FORT_DQRADFILL));
    ydotSetUp();

    if (do_temp)
    {
        //
	// rho_temp
        //
	derive_lst.add("rho_temp",IndexType::TheCellType(),1,FORT_DERMPRHO,the_same_box);
	derive_lst.addComponent("rho_temp",desc_lst,State_Type,Density,1);
	derive_lst.addComponent("rho_temp",desc_lst,State_Type,Temp,1);
	//
	// enthalpy
        //
	derive_lst.add("enthalpy",IndexType::TheCellType(),1,FORT_DERDVRHO,the_same_box);
	derive_lst.addComponent("enthalpy",desc_lst,State_Type,Density,1);
	derive_lst.addComponent("enthalpy",desc_lst,State_Type,RhoH,1);
        //
	// rho*R*T
        //
	derive_lst.add("rhoRT",IndexType::TheCellType(),1,FORT_DERRHORT,the_same_box);
	derive_lst.addComponent("rhoRT",desc_lst,State_Type,Density,1);
	derive_lst.addComponent("rhoRT",desc_lst,State_Type,Temp,1);

        for (i = 0; i < nspecies; i++)
        {
            const int comp = FirstSpec + i;
            derive_lst.addComponent("rhoRT",desc_lst,State_Type,comp,1);
        }
    }
    //
    // Species mass fractions.
    //
    for (i = 0; i < nspecies; i++)
      {
	const std::string name = "Y("+names[i]+")";
	derive_lst.add(name,IndexType::TheCellType(),1,FORT_DERDVRHO,the_same_box);
	derive_lst.addComponent(name,desc_lst,State_Type,Density,1);
	derive_lst.addComponent(name,desc_lst,State_Type,FirstSpec + i,1);
      }


    //
    // Species mole fractions
    //
    Array<std::string> var_names_molefrac(nspecies);
    for (i = 0; i < nspecies; i++)
      var_names_molefrac[i] = "X("+names[i]+")";
    derive_lst.add("molefrac",IndexType::TheCellType(),nspecies,
		   var_names_molefrac,FORT_DERMOLEFRAC,the_same_box);
    derive_lst.addComponent("molefrac",desc_lst,State_Type,Density,1);
    derive_lst.addComponent("molefrac",desc_lst,State_Type,FirstSpec,nspecies);

    //
    // Species concentrations
    //
    Array<std::string> var_names_conc(nspecies);
    for (i = 0; i < nspecies; i++)
      var_names_conc[i] = "C("+names[i]+")";
    derive_lst.add("concentration",IndexType::TheCellType(),nspecies,
		   var_names_conc,FORT_DERCONCENTRATION,the_same_box);
    derive_lst.addComponent("concentration",desc_lst,State_Type,Density,1);
    derive_lst.addComponent("concentration",desc_lst,State_Type,Temp,1);
    derive_lst.addComponent("concentration",desc_lst,State_Type,
			    FirstSpec,nspecies);

    if (nspecies > 0)
    {
        //
	// rho-sum rhoY.
        //
	derive_lst.add("rhominsumrhoY",IndexType::TheCellType(),1,FORT_DERRHOMINUSSUMRHOY,the_same_box);
	derive_lst.addComponent("rhominsumrhoY",desc_lst,State_Type,Density,1);
	for (i = 0; i < nspecies; i++)
	{
	    const int comp = FirstSpec + i;
	    derive_lst.addComponent("rhominsumrhoY",desc_lst,State_Type,comp,1);
	}
    }
    
    if (have_ydot)
    {
        //
	// Sum Ydot.
        //
	derive_lst.add("sumYdot",IndexType::TheCellType(),1,FORT_DERSUMYDOT,the_same_box);
	for (i = 0; i < nspecies; i++)
	{
	    derive_lst.addComponent("sumYdot",desc_lst,Ydot_Type,i,1);
	}
    }
    //
    // **************  DEFINE DERIVED QUANTITIES ********************
    //
    // Divergence of velocity field.
    //
    derive_lst.add("diveru",IndexType::TheCellType(),1,FORT_DERMGDIVU,grow_box_by_one);
    derive_lst.addComponent("diveru",desc_lst,State_Type,Xvel,BL_SPACEDIM);
    //
    // average pressure
    //
    derive_lst.add("avg_pressure",IndexType::TheCellType(),1,FORT_DERAVGPRES,
                   the_same_box);
    derive_lst.addComponent("avg_pressure",desc_lst,Press_Type,Pressure,1);
    //
    // Pressure gradient in X direction.
    //
    derive_lst.add("gradpx",IndexType::TheCellType(),1,FORT_DERGRDPX,the_same_box);
    derive_lst.addComponent("gradpx",desc_lst,Press_Type,Pressure,1);
    //
    // Pressure gradient in Y direction.
    //
    derive_lst.add("gradpy",IndexType::TheCellType(),1,FORT_DERGRDPY,the_same_box);
    derive_lst.addComponent("gradpy",desc_lst,Press_Type,Pressure,1);

#if (BL_SPACEDIM == 3)
    //
    // Pressure gradient in Z direction.
    //
    derive_lst.add("gradpz",IndexType::TheCellType(),1,FORT_DERGRDPZ,the_same_box);
    derive_lst.addComponent("gradpz",desc_lst,Press_Type,Pressure,1);
#endif
    //
    // Magnitude of vorticity.
    //
    derive_lst.add("mag_vort",IndexType::TheCellType(),1,FORT_DERMGVORT,grow_box_by_one);
    derive_lst.addComponent("mag_vort",desc_lst,State_Type,Xvel,BL_SPACEDIM);
    //
    // **************  DEFINE ERROR ESTIMATION QUANTITIES  *************
    //
    const int nGrowErr = 1;
    //err_list.add("tracer", nGrowErr, ErrorRec::Special, FORT_ADVERROR);

    if (do_temp)
	err_list.add("temp", nGrowErr, ErrorRec::Special, FORT_TEMPERROR);
    
    err_list.add("mag_vort", nGrowErr, ErrorRec::Special, FORT_MVERROR);
    err_list.add("tracer", nGrowErr, ErrorRec::Special, FORT_MVERROR);

    //
    // Tag region of interesting chemistry.
    //
    const int idx = getChemSolve().index(flameTracName);
    if (idx >= 0)
    {
        if (ParallelDescriptor::IOProcessor())
            std::cout << "Flame tracer will be " << flameTracName << '\n';
        const std::string name = "Y("+flameTracName+")";
        err_list.add(name,nGrowErr,ErrorRec::Special,FORT_FLAMETRACERROR);
    }

    //
    // Set up the running statistics
    //
    if (do_running_statistics)
    {
#if (BL_SPACEDIM == 2)
        const int nNonSpec = 8;
        const int nSpec = names.size();
        const int nBasicStats = 24 + 14 + 4 * nSpec;

        Array<std::string> slabStatVars(nNonSpec + nSpec);
        slabStatVars[0]="density";           slabStatVars[1]="x_velocity";
        slabStatVars[2]="y_velocity";        slabStatVars[3]="tracer";
        slabStatVars[4]="avg_pressure";      slabStatVars[5]="temp";
        slabStatVars[6]="rhoh";              slabStatVars[7]="rhoRT";

        for (int n = 0; n < names.size(); ++n)
            slabStatVars[n+nNonSpec] = "rho.Y(" + names[n] + ")";
#endif
#if (BL_SPACEDIM == 3)
        const int nNonSpec = 9;
        const int nSpec = names.size();
        const int nBasicStats = 35 + 17 + 5 * nSpec;

        Array<std::string> slabStatVars(nNonSpec + nSpec);
        slabStatVars[0]="density";           slabStatVars[1]="x_velocity";
        slabStatVars[2]="y_velocity";        slabStatVars[3]="z_velocity";
        slabStatVars[4]="tracer";            slabStatVars[5]="avg_pressure";
        slabStatVars[6]="temp";              slabStatVars[7]="rhoh";
        slabStatVars[8]="rhoRT";

        for (int n = 0; n < names.size(); ++n)
            slabStatVars[n+nNonSpec] = "rho.Y(" + names[n] + ")";
#endif
        if (advectionType[Trac] == NonConservative)
        {
            AmrLevel::get_slabstat_lst().add("basicStats",
                                             nBasicStats,
                                             slabStatVars,
                                             0,
                                             FORT_HT_BASICSTATS_NCTRAC);
        }
        else if (advectionType[Trac] == Conservative)
        {
            AmrLevel::get_slabstat_lst().add("basicStats",
                                             nBasicStats,
                                             slabStatVars,
                                             0,
                                             FORT_HT_BASICSTATS_NCTRAC);
        }
    }



#if 0
    {
        ParmParse pp1;
        std::string hack_data_inp; pp1.query("hack_data_inp",hack_data_inp);
        if (!(hack_data_inp.empty()))
        {
            std::string hack_chem_inp; pp.query("chemfile",hack_chem_inp);
            pp1.query("hack_chem_inp",hack_chem_inp);
            ifstream is(hack_chem_inp.c_str(),ios::in);
            bool ok = true;
            bool readingNames = false;
            std::string buf;
            Array<std::string> oldNames;
            while (ok)
            {
                is >> buf;
                if (readingNames)
                {
                    const int len = oldNames.size();
                    oldNames.resize(len+1);
                    oldNames[len] = buf;
                }
                if (buf == "SPECIES") readingNames = true;
                if (readingNames && buf == "END")
                {
                    oldNames.resize(oldNames.size()-1);
                    ok = false;
                }
            }
            is.close();

            const int NoldNames = oldNames.size();
            Array<int> mapOldToNew(NoldNames);
            for (int i=0; i<NoldNames; ++i)
                mapOldToNew[i] = getChemSolve().index(oldNames[i]); // defaults to -1

            ifstream is1(hack_data_inp.c_str(),ios::in);
            FArrayBox t_fab(is1);
            is1.close();

            Array<int> hack_boxlo(BL_SPACEDIM);
            pp1.getarr("hack_boxlo",hack_boxlo,0,BL_SPACEDIM);
            Array<int> hack_boxhi(BL_SPACEDIM);
            pp1.getarr("hack_boxhi",hack_boxhi,0,BL_SPACEDIM);

            const Box hbox_1(IntVect(hack_boxlo.dataPtr()),
                             IntVect(hack_boxhi.dataPtr()),
                             IndexType::TheCellType());
            ParmParse ppamr("amr");
            Array<int> rat(1,1);
            ppamr.getarr("ref_ratio",rat,0,1); // get ref ratio from 0 to 1
            IntVect ratio(D_DECL(rat[0],rat[0],rat[0]));
            const Box hbox_0 = ::coarsen(hbox_1,ratio);
            const Geometry geom_1(hbox_1);
            const Geometry geom_0(hbox_0);
            FArrayBox vol_1, vol_0;
            static_cast<CoordSys>(geom_1).GetVolume(vol_1,hbox_1);
            static_cast<CoordSys>(geom_0).GetVolume(vol_0,hbox_0);

            FArrayBox t_fab_0(hbox_0,t_fab.nComp());
            NavierStokes::avgDown_doit(t_fab,t_fab_0,vol_1,vol_0,1,0,
                                       hbox_0,0,t_fab.nComp(),ratio);

            const int nComp_hack = t_fab.nComp();
            FORT_HACK(hack_boxlo.dataPtr(),hack_boxhi.dataPtr(),
                      t_fab.dataPtr(),ARLIM(t_fab.loVect()),ARLIM(t_fab.hiVect()),
                      t_fab_0.dataPtr(),ARLIM(t_fab_0.loVect()),ARLIM(t_fab_0.hiVect()),
                      &nComp_hack,mapOldToNew.dataPtr(),&NoldNames,rat.dataPtr());
        }
    }
#endif
}

#if 0
//
// For testing ...
//
void
HeatTransfer::initData ()
{
      NavierStokes::initData();
}
#endif

class HTBld
    :
    public LevelBld
{
    virtual void variableSetUp ();
    virtual void variableCleanUp ();
    virtual AmrLevel *operator() ();
    virtual AmrLevel *operator() (Amr&            papa,
                                  int             lev,
                                  const Geometry& level_geom,
                                  const BoxArray& ba,
                                  Real            time);
};

HTBld HTbld;

LevelBld*
getLevelBld ()
{
    return &HTbld;
}

void
HTBld::variableSetUp ()
{
    HeatTransfer::variableSetUp();
}

void
HTBld::variableCleanUp ()
{
    HeatTransfer::variableCleanUp();
}

AmrLevel*
HTBld::operator() ()
{
    return new HeatTransfer;
}

AmrLevel*
HTBld::operator() (Amr&            papa,
                   int             lev,
                   const Geometry& level_geom,
                   const BoxArray& ba,
                   Real            time)
{
    return new HeatTransfer(papa, lev, level_geom, ba, time);
}

static
int
ydot_bc[] =
{
    INT_DIR, EXT_DIR, FOEXTRAP, REFLECT_EVEN, REFLECT_EVEN, REFLECT_EVEN
};

static
void
set_ydot_bc (BCRec&       bc,
             const BCRec& phys_bc)
{
    const int* lo_bc = phys_bc.lo();
    const int* hi_bc = phys_bc.hi();

    for (int i = 0; i < BL_SPACEDIM; i++)
    {
	bc.setLo(i,ydot_bc[lo_bc[i]]);
	bc.setHi(i,ydot_bc[hi_bc[i]]);
    }
}

void
HeatTransfer::ydotSetUp()
{
    Ydot_Type       = desc_lst.size();
    const int ngrow = 1;
    const int nydot = getChemSolve().numSpecies();

    if (ParallelDescriptor::IOProcessor())
	std::cout << "Ydot_Type, nydot = " << Ydot_Type << ' ' << nydot << '\n';

    desc_lst.addDescriptor(Ydot_Type,IndexType::TheCellType(),
			   StateDescriptor::Point,ngrow,nydot,
			   &lincc_interp);
	
    const StateDescriptor& d_cell = desc_lst[State_Type];
    const Array<std::string>& names   = getChemSolve().speciesNames();

    BCRec bc;	
    set_ydot_bc(bc,phys_bc);
    for (int i = 0; i < nydot; i++)
    {
	const std::string name = "d[Y("+names[i]+")]/dt";
	desc_lst.setComponent(Ydot_Type, i, name.c_str(), bc,
			      BndryFunc(FORT_YDOTFILL), &lincc_interp, 0, nydot-1);
    }
	
    have_ydot = 1;
}
