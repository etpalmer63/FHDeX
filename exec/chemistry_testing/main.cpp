#include <AMReX_PlotFileUtil.H>
#include <AMReX_ParmParse.H>
#include <AMReX_Vector.H>

#include "myfunc.H"
#include "chemistry_functions.H"
#include "chemistry_namespace_declarations.H"
#include "common_functions.H"
#include "common_namespace_declarations.H"

using namespace amrex;


int main (int argc, char* argv[])
{
    amrex::Initialize(argc,argv);
    main_main(argv[1]);

    amrex::Finalize();
    return 0;
}

void main_main(const char* argv)
{
    std::string inputs_file = argv;
    
    // read in parameters from inputs file into F90 modules
    // we use "+1" because of amrex_string_c_to_f expects a null char termination
    
    read_common_namelist(inputs_file.c_str(),inputs_file.size()+1);

    // copy contents of F90 modules to C++ namespaces
    InitializeCommonNamespace();
    
    amrex::Print() << "n_cells_x =  " << n_cells[0] << "\n";
    amrex::Print() << "n_cells_y =  " << n_cells[1] << "\n";
#if AMREX_SPACEDIM==3
    amrex::Print() << "n_cells_z =  " << n_cells[2] << "\n";
#endif
    
    amrex::Print() << "max_step = " << max_step << "\n";   
    
    amrex::Print() << "dt = " << fixed_dt << "\n";   
     
    amrex::Print() << "plot_int =  " << plot_int << "\n";

    // print problem type
    amrex::Print() << "prob_type = " << prob_type << "\n";
    
    // print number of species
    amrex::Print() << "nspecies  = " << nspecies << "\n";

    // print mass of each species    
    for (int n=0; n<nspecies; n++)
    {
        amrex::Print() << "molmass_" << n << " = " << molmass[n] << "\n";
    }

    // initialize chemistry namespace
    InitializeChemistryNamespace();
    // print reaction type
    amrex::Print() << "reaction type = " << reaction_type << "\n";

    // print number of reactions
    amrex::Print() << "nreaction = " << nreaction << "\n";

    
    // print reaction rates k's
    amrex::Print() << "Rate constants are:" << "\n";
    for (int m=0; m<nreaction; m++) amrex::Print() << rate_const[m] << " ";
    amrex::Print() << "\n";
    
    // print stoichiometry coeffs for the reactants
    amrex::Print() << "Stoich Coeffs Reactants:" << "\n";
    for (int m=0;m<nreaction;m++)
    {
        for (int n=0;n<nspecies;n++)
        {
            amrex::Print() << stoich_coeffs_R[m][n] << " ";
        }
        amrex::Print() << "\n";
    }

    // print stoichiometry coeffs for the products
    amrex::Print() << "Stoich Coeffs Products:" << "\n";
    for (int m=0;m<nreaction;m++)
    {
        for (int n=0;n<nspecies;n++)
        {
            amrex::Print() << stoich_coeffs_P[m][n] << " ";
        }
        amrex::Print() << "\n";
    }

    // **********************************
    // SIMULATION SETUP

    // make BoxArray and Geometry
    // ba will contain a list of boxes that cover the domain
    // geom contains information such as the physical domain size,
    //               number of points in the domain, and periodicity
    BoxArray ba;
    Geometry geom;

    // AMREX_D_DECL means "do the first X of these, where X is the dimensionality of the simulation"
    IntVect dom_lo(AMREX_D_DECL(       0,        0,        0));
    IntVect dom_hi(AMREX_D_DECL(n_cells[0]-1, n_cells[1]-1, n_cells[2]-1));

    // Make a single box that is the entire domain
    Box domain(dom_lo, dom_hi);

    // Initialize the boxarray "ba" from the single box "domain"
    ba.define(domain);

    // Break up boxarray "ba" into chunks no larger than "max_grid_size" along a direction
    ba.maxSize(IntVect(max_grid_size));

    // This defines the physical box, [0,1] in each direction.
    RealBox real_box({AMREX_D_DECL( prob_lo[0], prob_lo[1], prob_lo[2])},
                     {AMREX_D_DECL( prob_hi[0], prob_hi[1], prob_hi[2])});

    // periodic in all direction
    Array<int,AMREX_SPACEDIM> is_periodic{AMREX_D_DECL(1,1,1)};

    // This defines a Geometry object
    geom.define(domain, real_box, CoordSys::cartesian, is_periodic);

    // extract dx from the geometry object
    GpuArray<Real,AMREX_SPACEDIM> dx = geom.CellSizeArray();

    // Nghost = number of ghost cells for each array
    int Nghost = 1;

    // How Boxes are distrubuted among MPI processes
    DistributionMapping dm(ba);

    // we allocate two rho multifabs; one will store the old state, the other the new.
    MultiFab rho_old(ba, dm, nspecies, Nghost);
    MultiFab rho_new(ba, dm, nspecies, Nghost);
    
    // allocate Omega MultiFab 
    MultiFab Omega(ba, dm, nspecies, Nghost);    
    
    // time = starting time in the simulation
    amrex::Real time = 0.0;
    amrex::Real dt = fixed_dt;
    
    // for now I fix the cell volume as a sanity check
    amrex::Real dV = 1000.;
    //amrex::Real dV = dx[0]*dx[1];
    //amrex::Print() << "dV = " << dV; "\n";
    // **********************************
    // INITIALIZE DATA

    // loop over boxes
    for (MFIter mfi(rho_old); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.validbox();

        const Array4<Real>& rhoOld = rho_old.array(mfi);

        amrex::ParallelFor(bx, nspecies, [=] AMREX_GPU_DEVICE(int i, int j, int k, int n)
        {
            rhoOld(i,j,k,n) = rho0*rhobar[n];
        });
    }
    
    // vector to store the name of the components.
    // NOTE: its size must be equal to the number of components
    Vector<std::string> var_names(nspecies);
    for (int n=0; n<nspecies; n++) var_names[n] = "spec" + std::to_string(n+1); 
    
    // Write a plotfile of the initial data if plot_int > 0
    if (plot_int > 0)
    {
        int step = 0;
        const std::string& pltfile = amrex::Concatenate("plt",step,5);
        WriteSingleLevelPlotfile(pltfile, rho_old, var_names, geom, time, 0);
    }

    // mean and variance computed numerically at t0 for SSA

    amrex::Print() << "Stats ";
    amrex::Print()  << 0 << " ";
    for (int n=0; n<nspecies; n++)
    {
        amrex::Print()  << ComputeSpatialMean(rho_old,n)*(Runiv/k_B)/molmass[n] << " ";
    }
    for (int n=0; n<nspecies; n++)
    {
        amrex::Print()  << ComputeSpatialVariance(rho_old,n)*((Runiv/k_B)/molmass[n])*((Runiv/k_B)/molmass[n]) << " ";
    }
    amrex::Print() << "\n";
    

    for (int step = 1; step <= max_step; ++step)
    {
        // fill periodic ghost cells
        rho_old.FillBoundary(geom.periodicity());
        
        // only need to compute Omega if prob_type=2
        if (prob_type==2)
        {
            Omega.FillBoundary(geom.periodicity());
            // compute Omega
            compute_Omega(rho_old,Omega);
        }
        
        // loop over boxes
        for ( MFIter mfi(rho_old); mfi.isValid(); ++mfi )
        {
            const Box& bx = mfi.validbox();

            const Array4<Real>& rhoOld = rho_old.array(mfi);
            const Array4<Real>& rhoNew = rho_new.array(mfi);

            const Array4<Real>& OmegaArr = Omega.array(mfi);
            
            amrex::ParallelForRNG(bx, [=] AMREX_GPU_DEVICE (int i, int j, int k, RandomEngine const& engine) noexcept
            {
                // cell-based routines
                if (prob_type==1)
                {
                    amrex::Real n_old[MAX_SPECIES];
                    amrex::Real n_new[MAX_SPECIES];
                    for (int n=0; n<nspecies; n++) n_old[n] = rhoOld(i,j,k,n)*(Runiv/k_B)/molmass[n];
                
                    switch(reaction_type){
                        case 0: // deterministic case
                            advance_reaction_det_cell(n_old,n_new,dt);
                            break;
                        case 1: // CLE case
                            advance_reaction_CLE_cell(n_old,n_new,dt,dV,engine);
                            break;
                        case 2: // SSA case
                            advance_reaction_SSA_cell(n_old,n_new,dt,dV,engine);
                            break;
                    }

                    for (int n=0; n<nspecies; n++) rhoNew(i,j,k,n) = n_new[n]*(k_B/Runiv)*molmass[n];
                }
                // MultiFab-based routine
                else
                {
                    // just deterministic case for now
                    for (int n=0; n<nspecies; n++) rhoNew(i,j,k,n) = rhoOld(i,j,k,n) + dt*OmegaArr(i,j,k,n);
                }
            });

        }
        // update time
        time = time + dt;

        // copy new solution into old solution
        MultiFab::Copy(rho_old, rho_new, 0, 0, nspecies, 0);

        // Tell the I/O Processor to write out which step we're doing
        amrex::Print() << "Advanced step " << step << "\n";

        // print out mean and variance of both species in one single line at each time step 
        amrex::Print()  << "Stats ";
        amrex::Print()  << dt*step << " ";
        for (int n=0; n<nspecies; n++)
        {
            amrex::Print()  << ComputeSpatialMean(rho_new,n)*(Runiv/k_B)/molmass[n] << " ";
        }
        for (int n=0; n<nspecies; n++)
        {
            amrex::Print()  << ComputeSpatialVariance(rho_new,n)*((Runiv/k_B)/molmass[n])*((Runiv/k_B)/molmass[n]) << " ";
        }
        amrex::Print() << "\n";

        // Write a plotfile of the current data (plot_int was defined in the inputs file)
        if (plot_int > 0 && step%plot_int == 0)
        {
            const std::string& pltfile = amrex::Concatenate("plt",step,5);
            WriteSingleLevelPlotfile(pltfile, rho_new, var_names, geom, time, step);
        }
    }


    return;
}
