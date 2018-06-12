#include "FhdParticleContainer.H"
#include "particle_functions_F.H"

using namespace amrex;

FhdParticleContainer::FhdParticleContainer(const Geometry            & geom,
                              const DistributionMapping & dmap,
                              const BoxArray            & ba)
    : ParticleContainer<8, 2+BL_SPACEDIM> (geom, dmap, ba)
{}

void FhdParticleContainer::InitParticles()
{
    const int lev = 0;
    const Geometry& geom = Geom(lev); //Linking to geom given to constructor?
    const Real* dx  = geom.CellSize();
    
    std::mt19937 mt(0451);
    std::uniform_real_distribution<double> unit(0, 1.0);
    std::uniform_real_distribution<double> dist(-0.0001, 0.0001);

    int boxes = 0;

    for (MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi) 
    {

        const Box& validBox = mfi.validbox();

        const int* lovect = validBox.loVect();
        const int* hivect = validBox.hiVect();

        boxes++;

        //Print() << "Box: " << boxes << "\n";
        //Print() << "LoCoord: (" << lovect[0]*dx[0] << ", " << lovect[1]*dx[1] << ", " << lovect[2]*dx[2] << ")." << "\n";

        ParticleType p;

        const int grid_id = mfi.index();
        auto& particle_grid = GetParticles(lev)[std::make_pair(grid_id,0)];


//Place one particle in the centre of each cell
/*        for (IntVect iv = validBox.smallEnd(); iv <= validBox.bigEnd(); validBox.next(iv))
        {
            p.id() = ParticleType::NextID();
            p.cpu() = ParallelDescriptor::MyProc();

           // Print() << "Iterator: (" << iv[0] << ", " << iv[1] << ", " << iv[2] << ")." << "\n";

            p.pos(0) = (iv[0] + 0.5)*dx[0];
            p.pos(1) = (iv[1] + 0.5)*dx[1];
#if (BL_SPACEDIM == 3)
            p.pos(2) = (iv[2] + 0.5)*dx[2];
#endif
            p.rdata(0) = dist(mt);
            p.rdata(1) = dist(mt);
            p.rdata(2) = dist(mt);
#if (BL_SPACEDIM == 3)
            p.rdata(3) = dist(mt);
#endif

            particle_grid.push_back(p);

        }*/

        //Place 1 particles (per box?) randomly in the domain
        for(int i = 0; i<1; i++)
        {
            p.id() = ParticleType::NextID();
            p.cpu() = ParallelDescriptor::MyProc();

            p.pos(0) = 0.5;
            p.pos(1) = 0.6;
#if (BL_SPACEDIM == 3)
            p.pos(2) = 0.1;
#endif
            //Remove properties that aren't being used when we're done coding the rest of the algorithm, must match fortran struct defined in particle_functions.F90
            //Also, number of real and int particle properties is set in class definition.

            p.rdata(0) = 1; //mass
 

            p.rdata(1) = 1; //radius
            p.rdata(2) = 6*3.14159265359*p.rdata(1)/p.rdata(0); //acceleration factor (replace with amrex c++ constant for pi...)

            //Particle velocity is always 3D

            p.rdata(3) = 0; //particle xVel
            p.rdata(4) = 0; //particle yVel
            p.rdata(5) = 10; //particle zVel

            p.rdata(6) = dist(mt); //angular velocity 1
            p.rdata(7) = dist(mt); //angular velocity 2


            p.idata(0) = 0; //cell list index - for reverse lookup, depending on how we implement particle cell tracking
            p.idata(1) = 0; //species

            p.idata(2) = 0; //cell index i
            p.idata(3) = 0; //cell index j

#if (BL_SPACEDIM == 3)
            p.idata(4) = 0; //cell index k
#endif
            particle_grid.push_back(p);
        }
    }
    Redistribute(); //Not necessary?
}


//Computes drag on particles, updates particle velocities, updates particle positions, updates source Multifab for velocity change in fluid
void FhdParticleContainer::updateParticles(const Real dt, const Real* dx, const std::array<MultiFab, AMREX_SPACEDIM>& umac,
                                           std::array<MultiFab, AMREX_SPACEDIM>& umacNodal,
                                           const std::array<MultiFab, AMREX_SPACEDIM>& RealFaceCoords,
                                           const MultiFab& betaCC, //Not necessary but may use later
                                           MultiFab& betaNodal, //Not necessary but may use later
                                           const MultiFab& rho, //Not necessary but may use later
                                           std::array<MultiFab, AMREX_SPACEDIM>& source,
                                           std::array<MultiFab, AMREX_SPACEDIM>& sourceTemp)
{
    const int lev = 0;
    const RealBox& realDomain = Geom(lev).ProbDomain();

    //Arg1: Source multifab to be shifted. Arg2: destination multiFab. Arg3: A cell centred multifab for reference (can probably change this).
    FindNodalValues(umac[0], umacNodal[0], betaCC);
    FindNodalValues(umac[1], umacNodal[1], betaCC);

#if (AMREX_SPACEDIM == 3)
    FindNodalValues(umac[2], umacNodal[2], betaCC);
    FindNodalValues(betaCC, betaNodal, betaCC);
#endif
   
    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti) 
    {
        AoS& parts = pti.GetArrayOfStructs();
        int Np = parts.size();

        const Box& validBox = pti.validbox();

        update_particles(parts.data(), &Np, &dt, ZFILL(dx), 
                         ARLIM_3D(validBox.loVect()), ARLIM_3D(validBox.hiVect()),
                         ZFILL(realDomain.lo()), ZFILL(realDomain.hi()),
                         BL_TO_FORTRAN_3D(umacNodal[0][pti]),
                         BL_TO_FORTRAN_3D(umac[1][pti]),
#if (AMREX_SPACEDIM == 3)
                         BL_TO_FORTRAN_3D(umac[2][pti]),
#endif
                         BL_TO_FORTRAN_3D(RealFaceCoords[0][pti]),
                         BL_TO_FORTRAN_3D(RealFaceCoords[1][pti]),
#if (AMREX_SPACEDIM == 3)
                         BL_TO_FORTRAN_3D(RealFaceCoords[2][pti]),
#endif
                         BL_TO_FORTRAN_3D(betaCC[pti]),
                         BL_TO_FORTRAN_3D(rho[pti]),

                         BL_TO_FORTRAN_3D(sourceTemp[0][pti]),
                         BL_TO_FORTRAN_3D(sourceTemp[1][pti])
#if (AMREX_SPACEDIM == 3)
                         , BL_TO_FORTRAN_3D(sourceTemp[2][pti])
#endif
                        );
    }

    Redistribute();

    sourceTemp[0].SumBoundary(Geom(lev).periodicity());
    sourceTemp[1].SumBoundary(Geom(lev).periodicity());
#if (AMREX_SPACEDIM == 3)
    sourceTemp[2].SumBoundary(Geom(lev).periodicity());
#endif
    MultiFab::Add(source[0],sourceTemp[0],0,0,source[0].nComp(),source[0].nGrow());
    MultiFab::Add(source[1],sourceTemp[1],0,0,source[1].nComp(),source[1].nGrow());
#if (AMREX_SPACEDIM == 3)
    MultiFab::Add(source[2],sourceTemp[2],0,0,source[2].nComp(),source[2].nGrow());
#endif
    source[0].FillBoundary(Geom(lev).periodicity());
    source[1].FillBoundary(Geom(lev).periodicity());
#if (AMREX_SPACEDIM == 3)
    source[2].FillBoundary(Geom(lev).periodicity());
#endif



}

void FhdParticleContainer::WriteParticlesAscii(int n)
{
    const std::string& pltfile = amrex::Concatenate("particles", n, 5);
    WriteAsciiFile(pltfile);
}


