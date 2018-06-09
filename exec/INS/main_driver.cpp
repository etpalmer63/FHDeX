

#include "common_functions.H"
#include "gmres_functions.H"

#include "common_functions_F.H"
#include "common_namespace.H"
#include "common_namespace_declarations.H"

#include "gmres_functions_F.H"
#include "gmres_namespace.H"
#include "gmres_namespace_declarations.H"

#include "INS_functions.H"

using namespace common;
using namespace gmres;

// argv contains the name of the inputs file entered at the command line
void main_driver(const char* argv)
{

    // store the current time so we can later compute total run time.
    Real strt_time = ParallelDescriptor::second();

    std::string inputs_file = argv;

    // read in parameters from inputs file into F90 modules
    // we use "+1" because of amrex_string_c_to_f expects a null char termination
    read_common_namelist(inputs_file.c_str(),inputs_file.size()+1);
    read_gmres_namelist(inputs_file.c_str(),inputs_file.size()+1);

    // copy contents of F90 modules to C++ namespaces
    InitializeCommonNamespace();
    InitializeGmresNamespace();

    // is the problem periodic?
    Vector<int> is_periodic(AMREX_SPACEDIM,0);  // set to 0 (not periodic) by default
    for (int i=0; i<AMREX_SPACEDIM; ++i) {
        if (bc_lo[i] == 0 && bc_hi[i] == 0) {
            is_periodic[i] = 1;
        }
    }

    // make BoxArray and Geometry
    BoxArray ba;
    Geometry geom;
    {
        IntVect dom_lo(AMREX_D_DECL(           0,            0,            0));
        IntVect dom_hi(AMREX_D_DECL(n_cells[0]-1, n_cells[1]-1, n_cells[2]-1));
        Box domain(dom_lo, dom_hi);

        // Initialize the boxarray "ba" from the single box "bx"
        ba.define(domain);

        // Break up boxarray "ba" into chunks no larger than "max_grid_size" along a direction
        // note we are converting "Vector<int> max_grid_size" to an IntVect
        ba.maxSize(IntVect(max_grid_size));

        RealBox real_box({AMREX_D_DECL(0,0,0)},
                         {AMREX_D_DECL( 1.0, 1.0, 1.0)});
        // This defines a Geometry object
        geom.define(domain,&real_box,CoordSys::cartesian,is_periodic.data());
    }
  
    // how boxes are distrubuted among MPI processes
    DistributionMapping dmap(ba);

    // total density
    MultiFab rhotot(ba, dmap, 1, 1);

    // divergence
    MultiFab div(ba, dmap, 1, 1);

    // potential
    MultiFab phi(ba, dmap, 1, 1);

    // beta cell centred
    MultiFab betaCC(ba, dmap, 1, 1);

    // Nodal velocity for interpolations
    std::array< MultiFab, AMREX_SPACEDIM > umacNodal;
    AMREX_D_TERM(umacNodal[0].define(convert(ba,IntVect{AMREX_D_DECL(1, 1, 1)}), dmap, 1, 1);,
                 umacNodal[1].define(convert(ba,IntVect{AMREX_D_DECL(1, 1, 1)}), dmap, 1, 1);,
                 umacNodal[2].define(convert(ba,IntVect{AMREX_D_DECL(1, 1, 1)}), dmap, 1, 1););

    // gamma cell centred
    MultiFab gammaCC(ba, dmap, 1, 1);

    //Print() << nodal_flag_xy << "\n";
    //while(true);

    // beta on nodes in 2d
    // beta on edges in 3d
    std::array< MultiFab, NUM_EDGE > betaEdge;

#if (AMREX_SPACEDIM == 2)
    betaEdge[0].define(convert(ba,nodal_flag), dmap, 1, 1);
    betaEdge[0].setVal(1.);
#elif (AMREX_SPACEDIM == 3)
    betaEdge[0].define(convert(ba,nodal_flag_xy), dmap, 1, 1);
    betaEdge[1].define(convert(ba,nodal_flag_xz), dmap, 1, 1);
    betaEdge[2].define(convert(ba,nodal_flag_yz), dmap, 1, 1);
    betaEdge[0].setVal(1.);  
    betaEdge[1].setVal(1.);
    betaEdge[2].setVal(1.);
#endif

    //Nodal beta. If running in 2D, betaEdge is already nodal.

#if (AMREX_SPACEDIM == 3)
    MultiFab betaNodal(convert(ba,IntVect{AMREX_D_DECL(1, 1, 1)}), dmap, 1, 1);
#endif

    //Replace with proper initialiser
    phi.setVal(100.);

    betaCC.setVal(1.);
    gammaCC.setVal(0);

    // temporary placeholder for potential gradients on cell faces
    std::array< MultiFab, AMREX_SPACEDIM > umacT;
    AMREX_D_TERM(umacT[0].define(convert(ba,nodal_flag_x), dmap, 1, 1);,
                 umacT[1].define(convert(ba,nodal_flag_y), dmap, 1, 1);,
                 umacT[2].define(convert(ba,nodal_flag_z), dmap, 1, 1););

    // set density to 1
    rhotot.setVal(1.);

    // staggered real coordinates
    std::array< MultiFab, AMREX_SPACEDIM > RealFaceCoords;
    AMREX_D_TERM(RealFaceCoords[0].define(convert(ba,nodal_flag_x), dmap, AMREX_SPACEDIM, 1);,
                 RealFaceCoords[1].define(convert(ba,nodal_flag_y), dmap, AMREX_SPACEDIM, 1);,
                 RealFaceCoords[2].define(convert(ba,nodal_flag_z), dmap, AMREX_SPACEDIM, 1););

    // staggered velocities
    std::array< MultiFab, AMREX_SPACEDIM > umac;
    AMREX_D_TERM(umac[0].define(convert(ba,nodal_flag_x), dmap, 1, 1);,
                 umac[1].define(convert(ba,nodal_flag_y), dmap, 1, 1);,
                 umac[2].define(convert(ba,nodal_flag_z), dmap, 1, 1););

    // staggered source terms
    std::array< MultiFab, AMREX_SPACEDIM > source;
    AMREX_D_TERM(source[0].define(convert(ba,nodal_flag_x), dmap, 1, 1);,
                 source[1].define(convert(ba,nodal_flag_y), dmap, 1, 1);,
                 source[2].define(convert(ba,nodal_flag_z), dmap, 1, 1););

    // staggered temporary holder for calculating source terms - This may not be necesssary, review later.
    std::array< MultiFab, AMREX_SPACEDIM > sourceTemp;
    AMREX_D_TERM(sourceTemp[0].define(convert(ba,nodal_flag_x), dmap, 1, 1);,
                 sourceTemp[1].define(convert(ba,nodal_flag_y), dmap, 1, 1);,
                 sourceTemp[2].define(convert(ba,nodal_flag_z), dmap, 1, 1););

    // alpha arrays
    std::array< MultiFab, AMREX_SPACEDIM > alpha;
    AMREX_D_TERM(alpha[0].define(convert(ba,nodal_flag_x), dmap, 1, 1);
                 alpha[0].setVal(0);,
                 alpha[1].define(convert(ba,nodal_flag_y), dmap, 1, 1);
                 alpha[1].setVal(0);,
                 alpha[2].define(convert(ba,nodal_flag_z), dmap, 1, 1);
                 alpha[2].setVal(0););

    // For testing timestepping
    std::array< MultiFab, AMREX_SPACEDIM > umacNew;
    AMREX_D_TERM(umacNew[0].define(convert(ba,nodal_flag_x), dmap, 1, 1);,
                 umacNew[1].define(convert(ba,nodal_flag_y), dmap, 1, 1);,
                 umacNew[2].define(convert(ba,nodal_flag_z), dmap, 1, 1););

    // For testing StagApplyOp
    std::array< MultiFab, AMREX_SPACEDIM > umacOut;
    AMREX_D_TERM(umacOut[0].define(convert(ba,nodal_flag_x), dmap, 1, 1);,
                 umacOut[1].define(convert(ba,nodal_flag_y), dmap, 1, 1);,
                 umacOut[2].define(convert(ba,nodal_flag_z), dmap, 1, 1););


    // ***REPLACE THIS WITH A FUNCTION THAT SETS THE INITIAL VELOCITY***
    // ***SETTING THESE TO DUMMY VALUES FOR NOW***
    //AMREX_D_TERM(umac[0].setVal(1.);,
    //             umac[1].setVal(0);,
    //             umac[2].setVal(0););

    const RealBox& realDomain = geom.ProbDomain();


	int dm = 0;
	for ( MFIter mfi(rhotot); mfi.isValid(); ++mfi )
    {
        const Box& bx = mfi.validbox();

        AMREX_D_TERM(dm=0; init_vel(BL_TO_FORTRAN_BOX(bx),
                                    BL_TO_FORTRAN_ANYD(umac[0][mfi]), geom.CellSize(),
                                    geom.ProbLo(), geom.ProbHi() ,&dm, ZFILL(realDomain.lo()), ZFILL(realDomain.hi()));,
                     dm=1; init_vel(BL_TO_FORTRAN_BOX(bx),
            			            BL_TO_FORTRAN_ANYD(umac[1][mfi]), geom.CellSize(),
            			            geom.ProbLo(), geom.ProbHi() ,&dm, ZFILL(realDomain.lo()), ZFILL(realDomain.hi()));,
                     dm=2; init_vel(BL_TO_FORTRAN_BOX(bx),
                                    BL_TO_FORTRAN_ANYD(umac[2][mfi]), geom.CellSize(),
                                    geom.ProbLo(), geom.ProbHi() ,&dm, ZFILL(realDomain.lo()), ZFILL(realDomain.hi())););
    }

    AMREX_D_TERM(
    MultiFab::Copy(umacNew[0], umac[0], 0, 0, 1, 0);,
    MultiFab::Copy(umacNew[1], umac[1], 0, 0, 1, 0);,
    MultiFab::Copy(umacNew[2], umac[2], 0, 0, 1, 0););

    // compute the time step
    const Real* dx = geom.CellSize();
    Real dt = 50*0.5*dx[0]*dx[0] / (2.0*AMREX_SPACEDIM);
    
    Print() << "Step size: " << dt << "\n";
        

    int step = 0;
    Real time = 0.;

 
    //Particles!
    FhdParticleContainer particles(geom, dmap, ba);

    //Find coordinates of cell faces. Used for interpolating fields to particle locations
    FindFaceCoords(RealFaceCoords, geom); //May not be necessary to pass Geometry?


    particles.InitParticles();

    // write out initial state
    WritePlotFile(step,time,geom,rhotot,umac,div,particles);


    //Time stepping loop
   for(step=1;step<=max_step;++step)
    {
        AMREX_D_TERM(
        umac[0].FillBoundary(geom.periodicity());,
        umac[1].FillBoundary(geom.periodicity());,
        umac[2].FillBoundary(geom.periodicity()););

        eulerStep(betaCC, gammaCC, 
                 betaEdge,
                 umac, umacOut, umacNew, alpha, geom, &dt);


        AMREX_D_TERM(
        MultiFab::Copy(umac[0], umacNew[0], 0, 0, 1, 0);,
        MultiFab::Copy(umac[1], umacNew[1], 0, 0, 1, 0);,
        MultiFab::Copy(umac[2], umacNew[2], 0, 0, 1, 0););
        
#if (AMREX_SPACEDIM == 2)
        particles.updateParticles(dt, dx, umac, umacNodal, RealFaceCoords, betaCC, betaEdge[0], rhotot, source, sourceTemp);
#endif

#if (AMREX_SPACEDIM == 3)
        particles.updateParticles(dt, dx, umac, umacNodal, RealFaceCoords, betaCC, betaNodal, rhotot, source, sourceTemp);
#endif
        
        amrex::Print() << "Advanced step " << step << "\n";

        time = time + dt;

        if (plot_int > 0 && step%plot_int == 0)
        {
            // write out rhotot and umac to a plotfile
            WritePlotFile(step,time,geom,rhotot,umac,div,particles);
        }


    }


    //Compute divergence, last arguement is flag for increment
    
    //ComputeDiv(div,umac,geom,0);

    //Compute gradient
    //ComputeGrad(phi,umacT,geom);


    // Call the timer again and compute the maximum difference between the start time 
    // and stop time over all processors
    Real stop_time = ParallelDescriptor::second() - strt_time;
    ParallelDescriptor::ReduceRealMax(stop_time);
    amrex::Print() << "Run time = " << stop_time << std::endl;
}
