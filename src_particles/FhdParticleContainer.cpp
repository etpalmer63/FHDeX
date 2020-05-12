#include <AMReX.H>
#include <AMReX_AmrParGDB.H>
#include <AMReX_ParmParse.H>
#include <AMReX_Particles.H>
#include <AMReX_NeighborParticles.H>

#include <AMReX_VisMF.H>  // amrex::VisMF::Write(MultiFab)

#include <common_functions.H>
#include <particle_functions_K.H>
#include <FhdParticleContainer.H>
#include <ib_functions_F.H>

#include <iostream>

#include <AMReX_Geometry.H>
#include <AMReX_BoxArray.H>
#include <AMReX_DistributionMapping.H>
#include <AMReX_Utility.H>
#include <AMReX_MultiFab.H>
#include <AMReX_Box.H>
#include <iostream>
#include <fstream>


using namespace amrex;


bool FhdParticleContainer::use_neighbor_list  {true};
bool FhdParticleContainer::sort_neighbor_list {false};


FhdParticleContainer::FhdParticleContainer(const Geometry & geom,
                                           const DistributionMapping & dmap,
                                           const BoxArray & ba,
                                           int n_nbhd)
    : IBMarkerContainerBase<FHD_realData, FHD_intData>(geom, dmap, ba, n_nbhd), n_list(0)
{
    BL_PROFILE_VAR("FhdParticleContainer()",FhdParticleContainer);
    
    InitInternals(n_nbhd);
    nghost = n_nbhd;
 
    double domx, domy, domz;

    domx = (prob_hi[0] - prob_lo[0]);
    domy = (prob_hi[1] - prob_lo[1]);
    domz = (prob_hi[2] - prob_lo[2]);

    if (radialdist_int > 0 || cartdist_int > 0) {
        if (binSize == 0. || searchDist == 0.) {
            Abort("Must set binSize and searchDist if computing g(r)");
        }
    }
    
    if (searchDist > 0.5*domx || searchDist > 0.5*domy || searchDist > 0.5*domz) {
        Abort("searchDist is greater than half the domain length");
    }

    if (radialdist_int > 0 || cartdist_int > 0) {

        // create enough bins to look within a sphere with radius equal to "half" of the domain
        totalBins = (int)floor((searchDist)/((double)binSize)) - 1;

        Print() << "Bin size for pair correlation function: " << binSize << std::endl;
        Print() << "Number of pair correlation bins: " << totalBins << std::endl;
    
        // compute the volume of each bin
        binVolRadial = new Real[totalBins]();
        for(int i=0;i<totalBins;i++) {
            binVolRadial[i]= (4.0/3.0)*3.14159265359*(pow((i+1)*binSize,3) - pow((i)*binSize,3));
        }

        binVolCartesian =  2.*binSize * 2.*searchDist * 2.*searchDist;

        // how many snapshots
        radialStatsCount = 0;
        cartesianStatsCount = 0;
    }


    // storage for mean nearest neighbour counts
    if (radialdist_int > 0) {
        nearestN    = new Real[nspecies*nspecies + 1]();
        for(int i=0;i<nspecies*nspecies + 1;i++)
        {
            nearestN[i] = 0;
        }
    }
    
    // storage for mean radial distribution
    if (radialdist_int > 0) {
        meanRadialDistribution    = new Real[totalBins]();
        meanRadialDistribution_pp = new Real[totalBins]();
        meanRadialDistribution_pm = new Real[totalBins]();
        meanRadialDistribution_mm = new Real[totalBins]();
    }
        
    // storage for mean Cartesian distributions
    if (cartdist_int > 0) {
        meanXDistribution    = new Real[totalBins]();
        meanXDistribution_pp = new Real[totalBins]();
        meanXDistribution_pm = new Real[totalBins]();
        meanXDistribution_mm = new Real[totalBins]();
    
        meanYDistribution    = new Real[totalBins]();
        meanYDistribution_pp = new Real[totalBins]();
        meanYDistribution_pm = new Real[totalBins]();
        meanYDistribution_mm = new Real[totalBins]();

        meanZDistribution    = new Real[totalBins]();
        meanZDistribution_pp = new Real[totalBins]();
        meanZDistribution_pm = new Real[totalBins]();
        meanZDistribution_mm = new Real[totalBins]();
    }

    //Remove files that we will be appending to.
    remove("diffusionEst");
    remove("conductivityEst");
    remove("currentEst");
    remove("nearestNeighbour");

    Real dr = threepmRange/threepmBins;

    threepmVals[0] = 0;
    threepmMin[0] = 0;
    threepmMax[0] = 0;
    threepmPoints[0] = 0;


    for(int i=1;i<threepmBins;i++)
    {
       threepmPoints[i] = i*dr;

       threepmMax[i] = 0;
       threepmMin[i] = 10000000;
    }

}




void FhdParticleContainer::DoRFD(const Real dt, const Real* dxFluid, const Real* dxE, const Geometry geomF,
                                 const std::array<MultiFab, AMREX_SPACEDIM>& umac, const std::array<MultiFab, AMREX_SPACEDIM>& efield,
                                 const std::array<MultiFab, AMREX_SPACEDIM>& RealFaceCoords,
                                 const MultiFab& cellCenters,
                                 std::array<MultiFab, AMREX_SPACEDIM>& source,
                                 std::array<MultiFab, AMREX_SPACEDIM>& sourceTemp,
                                 const paramPlane* paramPlaneList, const int paramPlaneCount, int sw)
{
    BL_PROFILE_VAR("DoRFD()",DoRFD);
    
    UpdateCellVectors();

    const int lev = 0;
    const Real* dx = Geom(lev).CellSize();
    const Real* plo = Geom(lev).ProbLo();
    const Real* phi = Geom(lev).ProbHi();

#ifdef _OPENMP
#pragma omp parallel
#endif

    source[0].setVal(0.0);
    source[1].setVal(0.0);
#if (AMREX_SPACEDIM == 3)
    source[2].setVal(0.0);
#endif

    sourceTemp[0].setVal(0.0);
    sourceTemp[1].setVal(0.0);
#if (AMREX_SPACEDIM == 3)
    sourceTemp[2].setVal(0.0);
#endif


    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();
        
        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& particles = particle_tile.GetArrayOfStructs();
        const int np = particles.numParticles();
        
        //Print() << "FHD\n"; 
        do_rfd(particles.data(), &np,
                         ARLIM_3D(tile_box.loVect()),
                         ARLIM_3D(tile_box.hiVect()),
                         m_vector_ptrs[grid_id].dataPtr(),
                         m_vector_size[grid_id].dataPtr(),
                         ARLIM_3D(m_vector_ptrs[grid_id].loVect()),
                         ARLIM_3D(m_vector_ptrs[grid_id].hiVect()),
                         ZFILL(plo), ZFILL(phi), ZFILL(dx), &dt, ZFILL(geomF.ProbLo()), ZFILL(dxFluid), ZFILL(dxE),
                         BL_TO_FORTRAN_3D(umac[0][pti]),
                         BL_TO_FORTRAN_3D(umac[1][pti]),
#if (AMREX_SPACEDIM == 3)
                         BL_TO_FORTRAN_3D(umac[2][pti]),
#endif
                         BL_TO_FORTRAN_3D(efield[0][pti]),
                         BL_TO_FORTRAN_3D(efield[1][pti]),
#if (AMREX_SPACEDIM == 3)
                         BL_TO_FORTRAN_3D(efield[2][pti]),
#endif
                         BL_TO_FORTRAN_3D(RealFaceCoords[0][pti]),
                         BL_TO_FORTRAN_3D(RealFaceCoords[1][pti]),
#if (AMREX_SPACEDIM == 3)
                         BL_TO_FORTRAN_3D(RealFaceCoords[2][pti]),
#endif
                         BL_TO_FORTRAN_3D(cellCenters[pti]),
                         BL_TO_FORTRAN_3D(sourceTemp[0][pti]),
                         BL_TO_FORTRAN_3D(sourceTemp[1][pti])
#if (AMREX_SPACEDIM == 3)
                         , BL_TO_FORTRAN_3D(sourceTemp[2][pti])
#endif
                         , paramPlaneList, &paramPlaneCount, &sw
                         );


        // resize particle vectors after call to move_particles
//        for (IntVect iv = tile_box.smallEnd(); iv <= tile_box.bigEnd(); tile_box.next(iv))
//        {
//            const auto new_size = m_vector_size[grid_id](iv);
//            auto& pvec = m_cell_vectors[grid_id](iv);
//            pvec.resize(new_size);
//        }
    }
}

void FhdParticleContainer::computeForcesNL(const MultiFab& charge, const MultiFab& coords, const Real* dx) {

    BL_PROFILE_VAR("computeForcesNL()",computeForcesNL);

    Real rcount = 0;
    const int lev = 0;
   
    buildNeighborList(CHECK_PAIR{});
    
#ifdef _OPENMP
#pragma omp parallel
#endif

   for (FhdParIter pti(*this, lev, MFItInfo().SetDynamic(false)); pti.isValid(); ++pti)
   {     
        PairIndex index(pti.index(), pti.LocalTileIndex());
        AoS& particles = pti.GetArrayOfStructs();
        int Np = pti.numParticles();
        int Nn = pti.numNeighborParticles();
        int size = neighbor_list[lev][index].size();

        const Box& tile_box  = pti.tilebox();

        if (sr_tog==1)
        {
            compute_forces_nl(particles, Np, Nn,
                              m_neighbor_list[lev][index], rcount);            
        }
        if (es_tog==3)
        {
            compute_p3m_sr_correction_nl(particles, Np, Nn,
                                         m_neighbor_list[lev][index], dx, rcount);
        }
    }

    if(sr_tog==1) 
    {
            ParallelDescriptor::ReduceRealSum(rcount);

            Print() << rcount/2 << " close range interactions.\n";
    }
}

void FhdParticleContainer::MoveIons(const Real dt, const Real* dxFluid, const Real* dxE, const Geometry geomF,
                                    const std::array<MultiFab, AMREX_SPACEDIM>& umac, const std::array<MultiFab, AMREX_SPACEDIM>& efield,
                                    const std::array<MultiFab, AMREX_SPACEDIM>& RealFaceCoords,
                                    std::array<MultiFab, AMREX_SPACEDIM>& source,
                                    std::array<MultiFab, AMREX_SPACEDIM>& sourceTemp,
                                    const MultiFab& mobility,
                                    const paramPlane* paramPlaneList, const int paramPlaneCount, int sw)
{
    BL_PROFILE_VAR("MoveIons()",MoveIons);
    
    UpdateCellVectors();

    const int lev = 0;
    const Real* dx = Geom(lev).CellSize();
    const Real* plo = Geom(lev).ProbLo();
    const Real* phi = Geom(lev).ProbHi();

    double kinetic = 0;

    int        np_tile = 0 ,       np_proc = 0 ; // particle count
    Real rejected_tile = 0., rejected_proc = 0.; // rejected moves in midpoint scheme
    Real    moves_tile = 0.,    moves_proc = 0.; // total moves in midpoint scheme
    Real maxspeed_tile = 0., maxspeed_proc = 0.; // max speed
    Real  maxdist_tile = 0.,  maxdist_proc = 0.; // max displacement (fraction of radius)
    Real diffinst_tile = 0., diffinst_proc = 0.; // average diffusion coefficient

#ifdef _OPENMP
#pragma omp parallel
#endif

    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();
        
        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& particles = particle_tile.GetArrayOfStructs();
        np_tile = particles.numParticles();

        move_ions_fhd(particles.data(), &np_tile,
                      &rejected_tile, &moves_tile, &maxspeed_tile,
                      &maxdist_tile, &diffinst_tile,
                      ARLIM_3D(tile_box.loVect()),
                      ARLIM_3D(tile_box.hiVect()),
                      m_vector_ptrs[grid_id].dataPtr(),
                      m_vector_size[grid_id].dataPtr(),
                      ARLIM_3D(m_vector_ptrs[grid_id].loVect()),
                      ARLIM_3D(m_vector_ptrs[grid_id].hiVect()),
                      ZFILL(plo), ZFILL(phi), ZFILL(dx), &dt,
                      ZFILL(geomF.ProbLo()), ZFILL(dxFluid), ZFILL(dxE),
                      BL_TO_FORTRAN_3D(umac[0][pti]),
                      BL_TO_FORTRAN_3D(umac[1][pti]),
#if (AMREX_SPACEDIM == 3)
                      BL_TO_FORTRAN_3D(umac[2][pti]),
#endif
                      BL_TO_FORTRAN_3D(efield[0][pti]),
                      BL_TO_FORTRAN_3D(efield[1][pti]),
#if (AMREX_SPACEDIM == 3)
                      BL_TO_FORTRAN_3D(efield[2][pti]),
#endif
                      BL_TO_FORTRAN_3D(RealFaceCoords[0][pti]),
                      BL_TO_FORTRAN_3D(RealFaceCoords[1][pti]),
#if (AMREX_SPACEDIM == 3)
                      BL_TO_FORTRAN_3D(RealFaceCoords[2][pti]),
#endif
                      BL_TO_FORTRAN_3D(sourceTemp[0][pti]),
                      BL_TO_FORTRAN_3D(sourceTemp[1][pti]),
#if (AMREX_SPACEDIM == 3)
                      BL_TO_FORTRAN_3D(sourceTemp[2][pti]),
#endif
                      BL_TO_FORTRAN_3D(mobility[pti]),
                      paramPlaneList, &paramPlaneCount, &kinetic, &sw
            );

        // gather statistics
        np_proc       += np_tile;
        rejected_proc += rejected_tile;
        moves_proc    += moves_tile;
        maxspeed_proc = std::max(maxspeed_proc, maxspeed_tile);
        maxdist_proc  = std::max(maxdist_proc, maxdist_tile);
        diffinst_proc += diffinst_tile;

        // resize particle vectors after call to move_particles
        for (IntVect iv = tile_box.smallEnd(); iv <= tile_box.bigEnd(); tile_box.next(iv))
        {
            const auto new_size = m_vector_size[grid_id](iv);
            long imap = tile_box.index(iv);
            auto& pvec = m_cell_vectors[grid_id][imap];
            pvec.resize(new_size);
        }
    }

    // gather statistics
    ParallelDescriptor::ReduceIntSum(np_proc);
    ParallelDescriptor::ReduceRealSum(rejected_proc);
    ParallelDescriptor::ReduceRealSum(moves_proc);
    ParallelDescriptor::ReduceRealMax(maxspeed_proc);
    ParallelDescriptor::ReduceRealMax(maxdist_proc);
    ParallelDescriptor::ReduceRealSum(diffinst_proc);

    // write out global diagnostics
    if (ParallelDescriptor::IOProcessor()) {
        Print() << "I see " << np_proc << " particles\n";
        if (move_tog == 2) {
            Print() << "Fraction of midpoint moves rejected: " << rejected_proc/moves_proc << "\n";
        }
        Print() <<"Maximum observed speed: " << sqrt(maxspeed_proc) << "\n";
        Print() <<"Maximum observed displacement (fraction of radius): " << maxdist_proc << "\n";
        Print() <<"Average diffusion coefficient: " << diffinst_proc/np_proc << "\n";
    }
    
}

void FhdParticleContainer::MoveIons2(const Real dt, const Real* dxFluid, const Real* dxE, const Geometry geomF,
                                    const std::array<MultiFab, AMREX_SPACEDIM>& umac, const std::array<MultiFab, AMREX_SPACEDIM>& efield,
                                    const std::array<MultiFab, AMREX_SPACEDIM>& RealFaceCoords,
                                    std::array<MultiFab, AMREX_SPACEDIM>& source,
                                    std::array<MultiFab, AMREX_SPACEDIM>& sourceTemp,
                                    const MultiFab& mobility,
                                    const paramPlane* paramPlaneList, const int paramPlaneCount, int sw)
{
    BL_PROFILE_VAR("MoveIons()",MoveIons);
    
    UpdateCellVectors();

    const int lev = 0;
    const Real* dx = Geom(lev).CellSize();
    const Real* plo = Geom(lev).ProbLo();
    const Real* phi = Geom(lev).ProbHi();

    double kinetic = 0;

    int        np_tile = 0 ,       np_proc = 0 ; // particle count
    Real rejected_tile = 0., rejected_proc = 0.; // rejected moves in midpoint scheme
    Real    moves_tile = 0.,    moves_proc = 0.; // total moves in midpoint scheme
    Real maxspeed_tile = 0., maxspeed_proc = 0.; // max speed
    Real  maxdist_tile = 0.,  maxdist_proc = 0.; // max displacement (fraction of radius)
    Real diffinst_tile = 0., diffinst_proc = 0.; // average diffusion coefficient


    InterpolateMarkers(0, dx, umac, RealFaceCoords);



#ifdef _OPENMP
#pragma omp parallel
#endif

    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();
        
        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& particles = particle_tile.GetArrayOfStructs();
        np_tile = particles.numParticles();

        move_ions_fhd(particles.data(), &np_tile,
                      &rejected_tile, &moves_tile, &maxspeed_tile,
                      &maxdist_tile, &diffinst_tile,
                      ARLIM_3D(tile_box.loVect()),
                      ARLIM_3D(tile_box.hiVect()),
                      m_vector_ptrs[grid_id].dataPtr(),
                      m_vector_size[grid_id].dataPtr(),
                      ARLIM_3D(m_vector_ptrs[grid_id].loVect()),
                      ARLIM_3D(m_vector_ptrs[grid_id].hiVect()),
                      ZFILL(plo), ZFILL(phi), ZFILL(dx), &dt,
                      ZFILL(geomF.ProbLo()), ZFILL(dxFluid), ZFILL(dxE),
                      BL_TO_FORTRAN_3D(umac[0][pti]),
                      BL_TO_FORTRAN_3D(umac[1][pti]),
#if (AMREX_SPACEDIM == 3)
                      BL_TO_FORTRAN_3D(umac[2][pti]),
#endif
                      BL_TO_FORTRAN_3D(efield[0][pti]),
                      BL_TO_FORTRAN_3D(efield[1][pti]),
#if (AMREX_SPACEDIM == 3)
                      BL_TO_FORTRAN_3D(efield[2][pti]),
#endif
                      BL_TO_FORTRAN_3D(RealFaceCoords[0][pti]),
                      BL_TO_FORTRAN_3D(RealFaceCoords[1][pti]),
#if (AMREX_SPACEDIM == 3)
                      BL_TO_FORTRAN_3D(RealFaceCoords[2][pti]),
#endif
                      BL_TO_FORTRAN_3D(sourceTemp[0][pti]),
                      BL_TO_FORTRAN_3D(sourceTemp[1][pti]),
#if (AMREX_SPACEDIM == 3)
                      BL_TO_FORTRAN_3D(sourceTemp[2][pti]),
#endif
                      BL_TO_FORTRAN_3D(mobility[pti]),
                      paramPlaneList, &paramPlaneCount, &kinetic, &sw
            );

        // gather statistics
        np_proc       += np_tile;
        rejected_proc += rejected_tile;
        moves_proc    += moves_tile;
        maxspeed_proc = std::max(maxspeed_proc, maxspeed_tile);
        maxdist_proc  = std::max(maxdist_proc, maxdist_tile);
        diffinst_proc += diffinst_tile;

        // resize particle vectors after call to move_particles
        for (IntVect iv = tile_box.smallEnd(); iv <= tile_box.bigEnd(); tile_box.next(iv))
        {
            const auto new_size = m_vector_size[grid_id](iv);
            long imap = tile_box.index(iv);
            auto& pvec = m_cell_vectors[grid_id][imap];
            pvec.resize(new_size);
        }
    }

    // gather statistics
    ParallelDescriptor::ReduceIntSum(np_proc);
    ParallelDescriptor::ReduceRealSum(rejected_proc);
    ParallelDescriptor::ReduceRealSum(moves_proc);
    ParallelDescriptor::ReduceRealMax(maxspeed_proc);
    ParallelDescriptor::ReduceRealMax(maxdist_proc);
    ParallelDescriptor::ReduceRealSum(diffinst_proc);

    // write out global diagnostics
    if (ParallelDescriptor::IOProcessor()) {
        Print() << "I see " << np_proc << " particles\n";
        if (move_tog == 2) {
            Print() << "Fraction of midpoint moves rejected: " << rejected_proc/moves_proc << "\n";
        }
        Print() <<"Maximum observed speed: " << sqrt(maxspeed_proc) << "\n";
        Print() <<"Maximum observed displacement (fraction of radius): " << maxdist_proc << "\n";
        Print() <<"Average diffusion coefficient: " << diffinst_proc/np_proc << "\n";
    }
    
}

void FhdParticleContainer::SpreadIons(const Real dt, const Real* dxFluid, const Real* dxE, const Geometry geomF,
                                      const std::array<MultiFab, AMREX_SPACEDIM>& umac,
                                      const std::array<MultiFab, AMREX_SPACEDIM>& efield,
                                      const MultiFab& charge,
                                      const std::array<MultiFab, AMREX_SPACEDIM>& RealFaceCoords,
                                      const MultiFab& cellCenters,
                                      std::array<MultiFab, AMREX_SPACEDIM>& source,
                                      std::array<MultiFab, AMREX_SPACEDIM>& sourceTemp,
                                      const paramPlane* paramPlaneList, const int paramPlaneCount, int sw)
{
    BL_PROFILE_VAR("SpreadIons()",SpreadIons);

    const int lev = 0;
    const Real* dx = Geom(lev).CellSize();
    const Real* plo = Geom(lev).ProbLo();
    const Real* phi = Geom(lev).ProbHi();

    double potential = 0;

#ifdef _OPENMP
#pragma omp parallel
#endif


    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();
        
        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& particles = particle_tile.GetArrayOfStructs();
        const int np = particles.numParticles();
        
        //Print() << "FHD\n"; 
        spread_ions_fhd(particles.data(), &np,
                         ARLIM_3D(tile_box.loVect()),
                         ARLIM_3D(tile_box.hiVect()),
                         m_vector_ptrs[grid_id].dataPtr(),
                         m_vector_size[grid_id].dataPtr(),
                         ARLIM_3D(m_vector_ptrs[grid_id].loVect()),
                         ARLIM_3D(m_vector_ptrs[grid_id].hiVect()),
                         ZFILL(plo), ZFILL(phi), ZFILL(dx), &dt, ZFILL(geomF.ProbLo()), ZFILL(dxFluid), ZFILL(dxE),
                         BL_TO_FORTRAN_3D(umac[0][pti]),
                         BL_TO_FORTRAN_3D(umac[1][pti]),
#if (AMREX_SPACEDIM == 3)
                         BL_TO_FORTRAN_3D(umac[2][pti]),
#endif
                         BL_TO_FORTRAN_3D(efield[0][pti]),
                         BL_TO_FORTRAN_3D(efield[1][pti]),
#if (AMREX_SPACEDIM == 3)
                         BL_TO_FORTRAN_3D(efield[2][pti]),
#endif
                         BL_TO_FORTRAN_3D(charge[pti]),
                         BL_TO_FORTRAN_3D(RealFaceCoords[0][pti]),
                         BL_TO_FORTRAN_3D(RealFaceCoords[1][pti]),
#if (AMREX_SPACEDIM == 3)
                         BL_TO_FORTRAN_3D(RealFaceCoords[2][pti]),
#endif
                         BL_TO_FORTRAN_3D(cellCenters[pti]),
                         BL_TO_FORTRAN_3D(sourceTemp[0][pti]),
                         BL_TO_FORTRAN_3D(sourceTemp[1][pti])
#if (AMREX_SPACEDIM == 3)
                         , BL_TO_FORTRAN_3D(sourceTemp[2][pti])
#endif
                         , paramPlaneList, &paramPlaneCount, &potential, &sw
                         );
    }

    for (int i=0; i<AMREX_SPACEDIM; ++i) {
        MultiFabPhysBCDomainStress(sourceTemp[i], geomF, i);
        MultiFabPhysBCMacStress(sourceTemp[i], geomF, i);
    }
        
    sourceTemp[0].SumBoundary(geomF.periodicity());
    sourceTemp[1].SumBoundary(geomF.periodicity());
#if (AMREX_SPACEDIM == 3)
    sourceTemp[2].SumBoundary(geomF.periodicity());
#endif

    MultiFab::Add(source[0],sourceTemp[0],0,0,source[0].nComp(),source[0].nGrow());
    MultiFab::Add(source[1],sourceTemp[1],0,0,source[1].nComp(),source[1].nGrow());
#if (AMREX_SPACEDIM == 3)
    MultiFab::Add(source[2],sourceTemp[2],0,0,source[2].nComp(),source[2].nGrow());
#endif

    source[0].FillBoundary(geomF.periodicity());
    source[1].FillBoundary(geomF.periodicity());
#if (AMREX_SPACEDIM == 3)
    source[2].FillBoundary(geomF.periodicity());
#endif

}

//void FhdParticleContainer::SyncMembrane(double* spec3xPos, double* spec3yPos, double* spec3zPos, double* spec3xForce, double* spec3yForce, double* spec3zForce, const int length, const int step, const species* particleInfo)
//{
//    

//    const int lev = 0;
//    double temp;


//#ifdef _OPENMP
//#pragma omp parallel
//#endif


//    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti)
//    {
//        const int grid_id = pti.index();
//        const int tile_id = pti.LocalTileIndex();
//        const Box& tile_box  = pti.tilebox();
//        
//        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
//        auto& particles = particle_tile.GetArrayOfStructs();
//        const int np = particles.numParticles();

//        sync_particles(spec3xPos, spec3yPos, spec3zPos, spec3xForce, spec3yForce, spec3zForce, particles.data(), &np, &length);                    

//    }

//    //I'm sure there is an array version of this but this will do for now.
//    for(int i=0;i<length;i++)
//    {
//        temp = spec3xPos[i];
//        ParallelDescriptor::ReduceRealSum(temp);
//        spec3xPos[i] = temp;

//        temp = spec3yPos[i];
//        ParallelDescriptor::ReduceRealSum(temp);
//        spec3yPos[i] = temp;

//        temp = spec3zPos[i];
//        ParallelDescriptor::ReduceRealSum(temp);
//        spec3zPos[i] = temp;

//        spec3xForce[i] = 0;
//        spec3yForce[i] = 0;
//        spec3zForce[i] = 0;
//    }

//    if(ParallelDescriptor::MyProc() == 0)
//    {

//        user_force_calc(spec3xPos, spec3yPos, spec3zPos, spec3xForce, spec3yForce, spec3zForce, &length, &step, particleInfo);

//    }

//    for(int i=0;i<length;i++)
//    {
//        temp = spec3xForce[i];
//        ParallelDescriptor::ReduceRealSum(temp);
//        spec3xForce[i] = temp;

//        temp = spec3yForce[i];
//        ParallelDescriptor::ReduceRealSum(temp);
//        spec3yForce[i] = temp;

//        temp = spec3zForce[i];
//        ParallelDescriptor::ReduceRealSum(temp);
//        spec3zForce[i] = temp;

//    }

//    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti)
//    {
//        const int grid_id = pti.index();
//        const int tile_id = pti.LocalTileIndex();
//        const Box& tile_box  = pti.tilebox();
//        
//        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
//        auto& particles = particle_tile.GetArrayOfStructs();
//        const int np = particles.numParticles();

//        force_particles(spec3xPos, spec3yPos, spec3zPos, spec3xForce, spec3yForce, spec3zForce, particles.data(), &np, &length);                    

//    }
//}

void FhdParticleContainer::RadialDistribution(long totalParticles, const int step, const species* particleInfo)
{
    // timer for profiling
    BL_PROFILE_VAR("RadialDistribution()",RadialDistribution);
        
    const int lev = 0;
    int bin;
    Real domx, domy, domz, totalDist, temp;

    domx = (prob_hi[0] - prob_lo[0]);
    domy = (prob_hi[1] - prob_lo[1]);
    domz = (prob_hi[2] - prob_lo[2]);

    Gpu::ManagedDeviceVector<Real> posx(totalParticles, 0.0);
    Gpu::ManagedDeviceVector<Real> posy(totalParticles, 0.0);
    Gpu::ManagedDeviceVector<Real> posz(totalParticles, 0.0);
    Gpu::ManagedDeviceVector<int> species(totalParticles, 0.0);
    Gpu::ManagedDeviceVector<Real> charge(totalParticles, 0.0);

    Print() << "Calculating radial distribution\n";

    // collect particle positions onto one processor
    PullDown(0, posx.dataPtr(), -1, totalParticles);
    PullDown(0, posy.dataPtr(), -2, totalParticles);
    PullDown(0, posz.dataPtr(), -3, totalParticles);
    PullDown(0, charge.dataPtr(), 27, totalParticles);
    PullDownInt(0, species.dataPtr(), 4, totalParticles);
    
    // outer radial extent
    totalDist = totalBins*binSize;

    // this is the bin "hit count"
    Gpu::ManagedDeviceVector<Real> radDist(totalBins, 0.0);
    Gpu::ManagedDeviceVector<Real> radDist_pp(totalBins, 0.0);
    Gpu::ManagedDeviceVector<Real> radDist_pm(totalBins, 0.0);
    Gpu::ManagedDeviceVector<Real> radDist_mm(totalBins, 0.0);

    Gpu::ManagedDeviceVector<Real> nearest(totalParticles*(nspecies+1), 0.0);
    Gpu::ManagedDeviceVector<Real> nn(nspecies*nspecies+1, 0.0);

    for(int k = 0; k < (nspecies+1)*totalParticles; k++)
    {
        nearest[k] = 0;
    }

    for(int k = 0; k < nspecies*nspecies+1; k++)
    {
        nn[k] = 0;
    }
    
#ifdef _OPENMP
#pragma omp parallel
#endif
    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti) {

        PairIndex index(pti.index(), pti.LocalTileIndex());
        const int np = this->GetParticles(lev)[index].numRealParticles();
        auto& plev = this->GetParticles(lev);
        auto& ptile = plev[index];
        auto& aos   = ptile.GetArrayOfStructs();
        ParticleType* particles = aos().dataPtr();

        auto posxPtr = posx.dataPtr();
        auto posyPtr = posy.dataPtr();
        auto poszPtr = posz.dataPtr();
        auto speciesPtr = species.dataPtr();
        auto chargePtr = charge.dataPtr();

        auto radDistPtr = radDist.dataPtr();
        auto radDist_ppPtr = radDist_pp.dataPtr();
        auto radDist_pmPtr = radDist_pm.dataPtr();
        auto radDist_mmPtr = radDist_mm.dataPtr();

        auto nearestPtr = nearest.dataPtr();

        // loop over particles
        AMREX_FOR_1D( np, i,
        {

            ParticleType & part = particles[i];

            int iilo = (part.pos(0)-searchDist <= prob_lo[0]) ? -1 : 0;
            int iihi = (part.pos(0)+searchDist >= prob_hi[0]) ?  1 : 0;

            int jjlo = (part.pos(1)-searchDist <= prob_lo[1]) ? -1 : 0;
            int jjhi = (part.pos(1)+searchDist >= prob_hi[1]) ?  1 : 0;

            int kklo = (part.pos(2)-searchDist <= prob_lo[2]) ? -1 : 0;
            int kkhi = (part.pos(2)+searchDist >= prob_hi[2]) ?  1 : 0;
                
            int id = part.id() - 1;
             
            // loop over other particles
            for(int j = 0; j < totalParticles; j++) {
                
                // assume triply periodic, check the domain and the 8 periodic images
                for(int ii = iilo; ii <= iihi; ii++) {
                for(int jj = jjlo; jj <= jjhi; jj++) {
                for(int kk = kklo; kk <= kkhi; kk++) {

                    // get distance between particles
                    Real dx = part.pos(0)-posxPtr[j] - ii*domx;
                    Real dy = part.pos(1)-posyPtr[j] - jj*domy;
                    Real dz = part.pos(2)-poszPtr[j] - kk*domz;

                    int jSpec = speciesPtr[j]-1;
                    Real rad = sqrt(dx*dx + dy*dy + dz*dz);

                    if((nearestPtr[id*(nspecies+1) + jSpec] == 0 || nearestPtr[id*(nspecies+1) + jSpec] > rad) && rad != 0)
                    { 
                        nearestPtr[id*(nspecies +1)+ jSpec] = rad;
//                        
                    }

                    if((nearestPtr[id*(nspecies+1) + nspecies] == 0 || nearestPtr[id*(nspecies+1) + nspecies] > rad) && rad != 0)
                    { 
                        nearestPtr[id*(nspecies +1) + nspecies] = rad;
//                        Print() << "Particle " << i << " species " << jSpec << ", " << nearest[i*nspecies + jSpec] << "\n";
                    }

                    // if particles are close enough, increment the bin
                    if(rad < totalDist && rad > 0.) {

                        bin = static_cast<int>(rad/binSize);
                        radDistPtr[bin]++;

                        if (part.rdata(FHD_realData::q) > 0) {
                            if (chargePtr[j] > 0) {
                                radDist_ppPtr[bin]++;
                            }
                            else if (chargePtr[j] < 0) {
                                radDist_pmPtr[bin]++;
                            }
                        }
                        else if (part.rdata(FHD_realData::q) < 0) {
                            if (chargePtr[j] > 0) {
                                radDist_pmPtr[bin]++;
                            }
                            else if (chargePtr[j] < 0) {
                                radDist_mmPtr[bin]++;
                            }
                        }
                    }
                }
                }
                }                
            }  // loop over j (total particles)
        }); // loop over i (np; local particles)
    }

    // compute total number density
    Real n0_total = 0.;
    for (int i=0; i<nspecies; ++i) {
        n0_total += particleInfo[i].n0;
    }
        
    // collect the hit count
    ParallelDescriptor::ReduceRealSum(radDist   .dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(radDist_pp.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(radDist_pm.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(radDist_mm.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(nearest.dataPtr(),(nspecies+1)*totalParticles);

    int specCount[nspecies];

    for(int i=0;i<nspecies;i++)
    {
        specCount[i] = 0;
    }

    for(int i=0;i<totalParticles;i++)
    {
        int iSpec = species[i]-1;

        specCount[iSpec]++;

        for(int j=0;j<nspecies;j++)
        {
            nn[(iSpec)*nspecies + j] = nn[(iSpec)*nspecies + j] + nearest[i*(nspecies+1)+j];

            //std::cout << (iSpec)*nspecies + j  << ", " << nearest[i*(nspecies+1)+j] << "\n";

        }

        nn[nspecies*nspecies] = nn[nspecies*nspecies] + nearest[i*(nspecies+1)+nspecies];
    }

    for(int i=0;i<nspecies;i++)
    {
        for(int j=0;j<nspecies;j++)
        {           

            if(i==j)
            {
                nn[i*nspecies+j] = nn[i*nspecies+j]/(specCount[i]-1);

            }else
            {
                nn[i*nspecies+j] = nn[i*nspecies+j]/specCount[i];

            }

            //Print() << nn[i*nspecies+j] << "\n";

        }
    }

    nn[nspecies*nspecies] = nn[nspecies*nspecies]/(totalParticles - 1);

    //Print() << nn[nspecies*nspecies+1] << "\n";
        
            
    // normalize by 1 / (number density * bin volume * total particle count)
    for(int i=0;i<totalBins;i++) {
        radDist   [i] *= 1./(n0_total*binVolRadial[i]*(double)totalParticles);
        radDist_pp[i] *= 1./(n0_total*binVolRadial[i]*(double)totalParticles);
        radDist_pm[i] *= 1./(n0_total*binVolRadial[i]*(double)totalParticles);
        radDist_mm[i] *= 1./(n0_total*binVolRadial[i]*(double)totalParticles);
    }

    // increment number of snapshots
    radialStatsCount++;
    int stepsminusone = radialStatsCount - 1;
    Real stepsinv = 1.0/(double)radialStatsCount;

    // update the mean radial distribution
    for(int i=0;i<totalBins;i++) {
        meanRadialDistribution   [i] = (meanRadialDistribution   [i]*stepsminusone + radDist   [i])*stepsinv;
        meanRadialDistribution_pp[i] = (meanRadialDistribution_pp[i]*stepsminusone + radDist_pp[i])*stepsinv;
        meanRadialDistribution_pm[i] = (meanRadialDistribution_pm[i]*stepsminusone + radDist_pm[i])*stepsinv;
        meanRadialDistribution_mm[i] = (meanRadialDistribution_mm[i]*stepsminusone + radDist_mm[i])*stepsinv;
    }

    for(int i=0;i<(nspecies*nspecies + 1);i++) {

    //Print() << i << ", " << nn[i]<< "\n";
        nearestN[i] = (nearestN[i]*stepsminusone + nn[i])*stepsinv;

    }

    // output mean radial distribution g(r) based on plot_int
    if (plot_int > 0 && step%plot_int == 0) {
            
        if(ParallelDescriptor::MyProc() == 0) {

            std::string filename = Concatenate("radialDistribution",step,9);;
            std::ofstream ofs(filename, std::ofstream::out);

            // normalize by
            for(int i=0;i<totalBins;i++) {
                ofs << (i+0.5)*binSize << " "
                    << meanRadialDistribution   [i] << " "
                    << meanRadialDistribution_pp[i] << " "
                    << meanRadialDistribution_pm[i] << " "
                    << meanRadialDistribution_mm[i] << std::endl;
            }
            ofs.close();
        }
    }


    if (plot_int > 0 && step%plot_int == 0) {
            
        if(ParallelDescriptor::MyProc() == 0) {

            std::string filename = "nearestNeighbour";
            std::ofstream ofs(filename, std::ofstream::app);

            for(int i=0;i<nspecies*nspecies+1;i++) {
                ofs << nearestN[i] << " ";
            }
            ofs << std::endl;
            ofs.close();
        }
    }

    // reset the radial distribution at n_steps_skip (if n_steps_skip > 0)
    // OR
    // reset the radial distribution every |n_steps_skip| (if n_steps_skip < 0)
    if ((n_steps_skip > 0 && step == n_steps_skip) ||
        (n_steps_skip < 0 && step%n_steps_skip == 0) ) {
                    
        Print() << "Resetting radial distribution collection.\n";

        radialStatsCount = 0;

        for(int i=0;i<totalBins;i++) {
            meanRadialDistribution   [i] = 0;
            meanRadialDistribution_pp[i] = 0;
            meanRadialDistribution_pm[i] = 0;
            meanRadialDistribution_mm[i] = 0;
        }

        for(int i=0;i<nspecies*nspecies;i++) {
            nearestN[i] = 0;
        }
    }    
}

void FhdParticleContainer::CartesianDistribution(long totalParticles, const int step, const species* particleInfo)
{        
    BL_PROFILE_VAR("CartesianDistribution()",CartesianDistribution);
    
    const int lev = 0;
    int bin;
    double domx, domy, domz, totalDist, temp;

    domx = (prob_hi[0] - prob_lo[0]);
    domy = (prob_hi[1] - prob_lo[1]);
    domz = (prob_hi[2] - prob_lo[2]);

    Real posx[totalParticles];
    Real posy[totalParticles];
    Real posz[totalParticles];
    
    Real charge[totalParticles];

    Print() << "Calculating Cartesian distribution\n";

    // collect particle positions onto one processor
    PullDown(0, posx, -1, totalParticles);
    PullDown(0, posy, -2, totalParticles);
    PullDown(0, posz, -3, totalParticles);
    PullDown(0, charge, 27, totalParticles);

    // outer extent
    totalDist = totalBins*binSize;

    // this is the bin "hit count"
    RealVector XDist   (totalBins, 0.);
    RealVector XDist_pp(totalBins, 0.);
    RealVector XDist_pm(totalBins, 0.);
    RealVector XDist_mm(totalBins, 0.);
    RealVector YDist   (totalBins, 0.);
    RealVector YDist_pp(totalBins, 0.);
    RealVector YDist_pm(totalBins, 0.);
    RealVector YDist_mm(totalBins, 0.);
    RealVector ZDist   (totalBins, 0.);
    RealVector ZDist_pp(totalBins, 0.);
    RealVector ZDist_pm(totalBins, 0.);
    RealVector ZDist_mm(totalBins, 0.);

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti) {
            
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();

        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& particles = particle_tile.GetArrayOfStructs();
        const int np = particles.numParticles();
        
        // loop over particles
        for (int i = 0; i < np; ++i) {
            
            ParticleType & part = particles[i];

            int iilo = (part.pos(0)-searchDist <= prob_lo[0]) ? -1 : 0;
            int iihi = (part.pos(0)+searchDist >= prob_hi[0]) ?  1 : 0;

            int jjlo = (part.pos(1)-searchDist <= prob_lo[1]) ? -1 : 0;
            int jjhi = (part.pos(1)+searchDist >= prob_hi[1]) ?  1 : 0;

            int kklo = (part.pos(2)-searchDist <= prob_lo[2]) ? -1 : 0;
            int kkhi = (part.pos(2)+searchDist >= prob_hi[2]) ?  1 : 0;
            
            double dist, dx, dy, dz;
            // loop over other particles
            for(int j = 0; j < totalParticles; j++)
            {
                // assume triply periodic, check the domain and the 8 periodic images
                for(int ii = iilo; ii <= iihi; ii++)
                {
                for(int jj = jjlo; jj <= jjhi; jj++)
                {
                for(int kk = kklo; kk <= kkhi; kk++)
                {
                    // get distance between particles
                    dx = abs(part.pos(0)-posx[j] - ii*domx);
                    dy = abs(part.pos(1)-posy[j] - jj*domy);
                    dz = abs(part.pos(2)-posz[j] - kk*domz);

                    dist = sqrt(dx*dx + dy*dy + dz*dz);

                    // if particles are close enough, increment the bin
                    if (dist > 0.) {
                        if(dx < totalDist && dy < searchDist && dz < searchDist) {
                            
                            bin = (int)floor(dx/binSize);
                            XDist[bin]++;
                            
                            if (part.rdata(FHD_realData::q) > 0) {
                                if (charge[j] > 0) {
                                    XDist_pp[bin]++;
                                }
                                else if (charge[j] < 0) {
                                    XDist_pm[bin]++;
                                }
                            }
                            else if (part.rdata(FHD_realData::q) < 0) {
                                if (charge[j] > 0) {
                                    XDist_pm[bin]++;
                                }
                                else if (charge[j] < 0) {
                                    XDist_mm[bin]++;
                                }
                            }
                            
                        }
                        if(dy < totalDist && dx < searchDist && dz < searchDist) {
                            
                            bin = (int)floor(dy/binSize);
                            YDist[bin]++;
                            
                            if (part.rdata(FHD_realData::q) > 0) {
                                if (charge[j] > 0) {
                                    YDist_pp[bin]++;
                                }
                                else if (charge[j] < 0) {
                                    YDist_pm[bin]++;
                                }
                            }
                            else if (part.rdata(FHD_realData::q) < 0) {
                                if (charge[j] > 0) {
                                    YDist_pm[bin]++;
                                }
                                else if (charge[j] < 0) {
                                    YDist_mm[bin]++;
                                }
                            }
                            
                        }
                        if(dz < totalDist && dx < searchDist && dy < searchDist) {
                            
                            bin = (int)floor(dz/binSize);                            
                            ZDist[bin]++;
                            
                            if (part.rdata(FHD_realData::q) > 0) {
                                if (charge[j] > 0) {
                                    ZDist_pp[bin]++;
                                }
                                else if (charge[j] < 0) {
                                    ZDist_pm[bin]++;
                                }
                            }
                            else if (part.rdata(FHD_realData::q) < 0) {
                                if (charge[j] > 0) {
                                    ZDist_pm[bin]++;
                                }
                                else if (charge[j] < 0) {
                                    ZDist_mm[bin]++;
                                }
                            }
                        }
                    }
                }
                }
                }                
            }
        }
    }

    // compute total number density
    double n0_total = 0.;
    for (int i=0; i<nspecies; ++i) {
        n0_total += particleInfo[i].n0;
    }
 
    // collect the hit count
    ParallelDescriptor::ReduceRealSum(XDist   .dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(XDist_pp.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(XDist_pm.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(XDist_mm.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(YDist   .dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(YDist_pp.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(YDist_pm.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(YDist_mm.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(ZDist   .dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(ZDist_pp.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(ZDist_pm.dataPtr(),totalBins);
    ParallelDescriptor::ReduceRealSum(ZDist_mm.dataPtr(),totalBins);

    // normalize by 1 / (number density * bin volume * total particle count)
    for(int i=0;i<totalBins;i++) {
        XDist   [i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        XDist_pp[i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        XDist_pm[i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        XDist_mm[i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        YDist   [i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        YDist_pp[i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        YDist_pm[i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        YDist_mm[i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        ZDist   [i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        ZDist_pp[i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        ZDist_pm[i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
        ZDist_mm[i] *= 1./(n0_total*binVolCartesian*(double)totalParticles);
    }

    // increment number of snapshots
    cartesianStatsCount++;
    int stepsminusone = cartesianStatsCount - 1;
    double stepsinv = 1.0/(double)cartesianStatsCount;

    // update the mean Cartesian distribution
    for(int i=0;i<totalBins;i++) {
        meanXDistribution   [i] = (meanXDistribution   [i]*stepsminusone + XDist   [i])*stepsinv;
        meanXDistribution_pp[i] = (meanXDistribution_pp[i]*stepsminusone + XDist_pp[i])*stepsinv;
        meanXDistribution_pm[i] = (meanXDistribution_pm[i]*stepsminusone + XDist_pm[i])*stepsinv;
        meanXDistribution_mm[i] = (meanXDistribution_mm[i]*stepsminusone + XDist_mm[i])*stepsinv;
        meanYDistribution   [i] = (meanYDistribution   [i]*stepsminusone + YDist   [i])*stepsinv;
        meanYDistribution_pp[i] = (meanYDistribution_pp[i]*stepsminusone + YDist_pp[i])*stepsinv;
        meanYDistribution_pm[i] = (meanYDistribution_pm[i]*stepsminusone + YDist_pm[i])*stepsinv;
        meanYDistribution_mm[i] = (meanYDistribution_mm[i]*stepsminusone + YDist_mm[i])*stepsinv;
        meanZDistribution   [i] = (meanZDistribution   [i]*stepsminusone + ZDist   [i])*stepsinv;
        meanZDistribution_pp[i] = (meanZDistribution_pp[i]*stepsminusone + ZDist_pp[i])*stepsinv;
        meanZDistribution_pm[i] = (meanZDistribution_pm[i]*stepsminusone + ZDist_pm[i])*stepsinv;
        meanZDistribution_mm[i] = (meanZDistribution_mm[i]*stepsminusone + ZDist_mm[i])*stepsinv;
    }

    // output mean Cartesian distribution g(x), g(y), g(z) based on plot_int
    if (plot_int > 0 && step%plot_int == 0) {
            
        if(ParallelDescriptor::MyProc() == 0) {

            std::string filename = Concatenate("cartesianDistribution",step,9);;
            std::ofstream ofs(filename, std::ofstream::out);

            for(int i=0;i<totalBins;i++) {
                ofs << (i+0.5)*binSize << " "
                    << meanXDistribution   [i] << " "
                    << meanXDistribution_pp[i] << " "
                    << meanXDistribution_pm[i] << " "
                    << meanXDistribution_mm[i] << " "
                    << meanYDistribution   [i] << " "
                    << meanYDistribution_pp[i] << " "
                    << meanYDistribution_pm[i] << " "
                    << meanYDistribution_mm[i] << " "
                    << meanZDistribution   [i] << " "
                    << meanZDistribution_pp[i] << " "
                    << meanZDistribution_pm[i] << " "
                    << meanZDistribution_mm[i] << " " << std::endl;
            }
            ofs.close();
        }
    }

    // reset the Cartesian distribution at n_steps_skip (if n_steps_skip > 0)
    // OR
    // reset the Cartesian distribution every |n_steps_skip| (if n_steps_skip < 0)
    if ((n_steps_skip > 0 && step == n_steps_skip) ||
        (n_steps_skip < 0 && step%n_steps_skip == 0) ) {
                    
        Print() << "Resetting Cartesian distribution collection.\n";

        cartesianStatsCount = 0;

        for(int i=0;i<totalBins;i++) {
            meanXDistribution   [i] = 0;
            meanXDistribution_pp[i] = 0;
            meanXDistribution_pm[i] = 0;
            meanXDistribution_mm[i] = 0;
            meanYDistribution   [i] = 0;
            meanYDistribution_pp[i] = 0;
            meanYDistribution_pm[i] = 0;
            meanYDistribution_mm[i] = 0;
            meanZDistribution   [i] = 0;
            meanZDistribution_pp[i] = 0;
            meanZDistribution_pm[i] = 0;
            meanZDistribution_mm[i] = 0;
        }
    }
}

void FhdParticleContainer::collectFields(const Real dt, const Real* dxPotential, 
                                         const MultiFab& RealCenterCoords, const Geometry geomP, MultiFab& charge, MultiFab& chargeTemp,
                                         MultiFab& mass, MultiFab& massTemp)
{
    BL_PROFILE_VAR("collectFields()",collectFields);
    
    const int lev = 0;
    const Real* dx = Geom(lev).CellSize();
    const Real* plo = Geom(lev).ProbLo();
    const Real* phi = Geom(lev).ProbHi();

#ifdef _OPENMP
#pragma omp parallel
#endif

    charge.setVal(0.0);
    chargeTemp.setVal(0.0);

    mass.setVal(0.0);
    massTemp.setVal(0.0);

    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();
        
        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& particles = particle_tile.GetArrayOfStructs();
        const int np = particles.numParticles();
        
        collect_charge(particles.data(), &np,
                         ARLIM_3D(tile_box.loVect()),
                         ARLIM_3D(tile_box.hiVect()),
                         m_vector_ptrs[grid_id].dataPtr(),
                         m_vector_size[grid_id].dataPtr(),
                         ARLIM_3D(m_vector_ptrs[grid_id].loVect()),
                         ARLIM_3D(m_vector_ptrs[grid_id].hiVect()),
                         ZFILL(plo), ZFILL(phi), ZFILL(dx), &dt, ZFILL(geomP.ProbLo()), ZFILL(dxPotential),
                         BL_TO_FORTRAN_3D(RealCenterCoords[pti]),
                         BL_TO_FORTRAN_3D(chargeTemp[pti]));

    }

    MultiFabPhysBCCharge(chargeTemp, geomP);

    chargeTemp.SumBoundary(geomP.periodicity());
    //massTemp.SumBoundary(geomP.periodicity());

    MultiFab::Add(charge,chargeTemp,0,0,charge.nComp(),charge.nGrow());
    //MultiFab::Add(mass,massTemp,0,0,mass.nComp(),mass.nGrow());

    charge.FillBoundary(geomP.periodicity());
}



void FhdParticleContainer::InitCollisionCells(MultiFab& collisionPairs,
                                              MultiFab& collisionFactor, 
                                              MultiFab& cellVols, const species particleInfo, const Real delt)
{
    BL_PROFILE_VAR("InitCollisionCells()",InitCollisionCells);

    UpdateCellVectors();
    const int lev = 0;

    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti) 
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();
        
        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& particles = particle_tile.GetArrayOfStructs();
        const int Np = particles.numParticles();

        init_cells(particles.data(),
                         ARLIM_3D(tile_box.loVect()), 
                         ARLIM_3D(tile_box.hiVect()),
                         m_vector_ptrs[grid_id].dataPtr(),
                         m_vector_size[grid_id].dataPtr(),
                         ARLIM_3D(m_vector_ptrs[grid_id].loVect()),
                         ARLIM_3D(m_vector_ptrs[grid_id].hiVect()),
                         BL_TO_FORTRAN_3D(collisionPairs[pti]),
                         BL_TO_FORTRAN_3D(collisionFactor[pti]),
                         BL_TO_FORTRAN_3D(cellVols[pti]),&Np,&particleInfo.Neff,&particleInfo.cp,&particleInfo.d,&delt
                        );
    }
}

void FhdParticleContainer::CollideParticles(MultiFab& collisionPairs,
                                            MultiFab& collisionFactor, 
                                            MultiFab& cellVols, const species particleInfo, const Real delt)
{
    BL_PROFILE_VAR("CollideParticles()",CollideParticles);
    
    const int lev = 0;

    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti) 
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();
        
        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& particles = particle_tile.GetArrayOfStructs();
        const int Np = particles.numParticles();

        collide_cells(particles.data(),
                         ARLIM_3D(tile_box.loVect()), 
                         ARLIM_3D(tile_box.hiVect()),
                         m_vector_ptrs[grid_id].dataPtr(),
                         m_vector_size[grid_id].dataPtr(),
                         ARLIM_3D(m_vector_ptrs[grid_id].loVect()),
                         ARLIM_3D(m_vector_ptrs[grid_id].hiVect()),
                         BL_TO_FORTRAN_3D(collisionPairs[pti]),
                         BL_TO_FORTRAN_3D(collisionFactor[pti]),
                         BL_TO_FORTRAN_3D(cellVols[pti]),&Np,&particleInfo.Neff,&particleInfo.cp,&particleInfo.d,&delt
                        );
    }
}

void FhdParticleContainer::InitializeFields(MultiFab& particleInstant,
                                            MultiFab& cellVols, const species particleInfo)
{
    BL_PROFILE_VAR("InitializeFields()",InitializeFields);  

    UpdateCellVectors();
    const int lev = 0;

    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti) 
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();

        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& parts = particle_tile.GetArrayOfStructs();
        const int Np = parts.numParticles();

        initialize_fields(parts.data(),
                         ARLIM_3D(tile_box.loVect()),
                         ARLIM_3D(tile_box.hiVect()),
                         m_vector_ptrs[grid_id].dataPtr(),
                         m_vector_size[grid_id].dataPtr(),
                         ARLIM_3D(m_vector_ptrs[grid_id].loVect()),
                         ARLIM_3D(m_vector_ptrs[grid_id].hiVect()),   
                         BL_TO_FORTRAN_3D(particleInstant[pti]),
                         BL_TO_FORTRAN_3D(cellVols[pti]),&particleInfo.Neff, &Np, &particleInfo.R, &particleInfo.T
                        );

    }
}

void FhdParticleContainer::EvaluateStats(MultiFab& particleInstant,
                                         MultiFab& particleMeans,
                                         MultiFab& particleVars,
                                         MultiFab& cellVols, species particleInfo, const Real delt, int steps)
{
    BL_PROFILE_VAR("EvaluateStats()",EvaluateStats);
    
    const int lev = 0;
    const double Neff = particleInfo.Neff;
    const double n0 = particleInfo.n0;
    const double T0 = particleInfo.T;

    double tp = 0;
    double te = 0;
    double tm = 0;

    double totalMass;

    RealVector avcurrent_tile(3), avcurrent_proc(3);
    RealVector varcurrent_tile(3), varcurrent_proc(3);

    std::fill(avcurrent_tile.begin(), avcurrent_tile.end(), 0.);
    std::fill(avcurrent_proc.begin(), avcurrent_proc.end(), 0.);
    std::fill(varcurrent_tile.begin(), varcurrent_tile.end(), 0.);
    std::fill(varcurrent_proc.begin(), varcurrent_proc.end(), 0.);
    
    BoxArray ba = particleMeans.boxArray();
    long cellcount = ba.numPts();

    // zero instantaneous values
    particleInstant.setVal(0.);
    
    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti) 
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();

        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& parts = particle_tile.GetArrayOfStructs();
        const int Np = parts.numParticles();

        tp = tp + Np;

        evaluate_fields(parts.data(),
                         ARLIM_3D(tile_box.loVect()),
                         ARLIM_3D(tile_box.hiVect()),
                         m_vector_ptrs[grid_id].dataPtr(),
                         m_vector_size[grid_id].dataPtr(),
                         ARLIM_3D(m_vector_ptrs[grid_id].loVect()),
                         ARLIM_3D(m_vector_ptrs[grid_id].hiVect()),   
                         BL_TO_FORTRAN_3D(particleInstant[pti]),
                         BL_TO_FORTRAN_3D(cellVols[pti]),&Neff, &Np
                        );

    }

    // FIXME - tiling doesn't work
    for (MFIter mfi(particleInstant,false); mfi.isValid(); ++mfi )
    {
        const Box& tile_box  = mfi.tilebox();

        evaluate_means(ARLIM_3D(tile_box.loVect()),
                       ARLIM_3D(tile_box.hiVect()),
                       BL_TO_FORTRAN_3D(particleInstant[mfi]),
                       BL_TO_FORTRAN_3D(particleMeans[mfi]),
                       BL_TO_FORTRAN_3D(particleVars[mfi]),
                       BL_TO_FORTRAN_3D(cellVols[mfi]),
                       &n0,&T0,&delt,&steps,
                       avcurrent_tile.dataPtr());

        // gather statistics
        for (int i=0; i<3; ++i) {
            avcurrent_proc[i] += avcurrent_tile[i];
        }
    }
    
    // gather statistics
    ParallelDescriptor::ReduceRealSum(avcurrent_proc.dataPtr(),3);

    // FIXME - this needs to be converted to an MFIter so it works (like evaluate_means)    
    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti) 
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();

        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& parts = particle_tile.GetArrayOfStructs();
        const int Np = parts.numParticles();

        evaluate_corrs(parts.data(),
                       ARLIM_3D(tile_box.loVect()),
                       ARLIM_3D(tile_box.hiVect()),
                       m_vector_ptrs[grid_id].dataPtr(),
                       m_vector_size[grid_id].dataPtr(),
                       ARLIM_3D(m_vector_ptrs[grid_id].loVect()),
                       ARLIM_3D(m_vector_ptrs[grid_id].hiVect()), 
                       BL_TO_FORTRAN_3D(particleInstant[pti]),
                       BL_TO_FORTRAN_3D(particleMeans[pti]),
                       BL_TO_FORTRAN_3D(particleVars[pti]),
                       BL_TO_FORTRAN_3D(cellVols[pti]), &Np,&Neff,&n0,&T0,&delt, &steps,
                       varcurrent_tile.dataPtr()
            );

        // gather statistics
        for (int i=0; i<3; ++i) {
            varcurrent_proc[i] += varcurrent_tile[i];
        }
    }

    // gather statistics
    ParallelDescriptor::ReduceRealSum(varcurrent_proc.dataPtr(),3);

    // write out current mean and variance to file
    if(ParallelDescriptor::MyProc() == 0) {
        std::string filename = "currentEst";
        std::ofstream ofs(filename, std::ofstream::app);
        ofs << avcurrent_proc[0]/cellcount << "  "
            << avcurrent_proc[1]/cellcount << "  "
            << avcurrent_proc[2]/cellcount << "  "
            << varcurrent_proc[0]/cellcount << "  "
            << varcurrent_proc[1]/cellcount << "  "
            << varcurrent_proc[2]/cellcount << "\n";
        ofs.close();
    }
}

void FhdParticleContainer::WriteParticlesAscii(std::string asciiName)
{
    BL_PROFILE_VAR("WriteParticlesAscii()",WriteParticlesAscii);
    
    WriteAsciiFile(asciiName);
}

void
FhdParticleContainer::UpdateCellVectors()
{
    BL_PROFILE_VAR("UpdateCellVectors()",UpdateCellVectors);
    
    const int lev = 0;

    bool needs_update = true;
    if (not m_vectors_initialized)
    {
        // this is the first call, so we must update
        m_vectors_initialized = true;
        needs_update = true;
    }
    else if ((m_BARef != this->ParticleBoxArray(lev).getRefID()) or 
             (m_DMRef != this->ParticleDistributionMap(lev).getRefID()))
    {
        // the grids have changed, so we must update
        m_BARef = this->ParticleBoxArray(lev).getRefID();
        m_DMRef = this->ParticleDistributionMap(lev).getRefID();
        needs_update = true;
    }
    
    if (not needs_update) return;

    // clear old data
    m_cell_vectors.clear();
    m_vector_size.clear();
    m_vector_ptrs.clear();
    
    // allocate storage for cell vectors. NOTE - do not tile this loop
    for(MFIter mfi = MakeMFIter(lev, false); mfi.isValid(); ++mfi)
    {
        const Box& box = mfi.validbox();
        const int grid_id = mfi.index();
        m_cell_vectors[grid_id].resize(box.numPts());
        m_vector_size[grid_id].resize(box);
        m_vector_ptrs[grid_id].resize(box);
    }

    // insert particles into vectors - this can be tiled
#ifdef _OPENMP
#pragma omp parallel
#endif    
    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        auto& particles = pti.GetArrayOfStructs();
        const int np    = pti.numParticles();
        const Box& tile_box  = pti.tilebox();

        for(int pindex = 0; pindex < np; ++pindex) {
            ParticleType& p = particles[pindex];
            const IntVect& iv = this->Index(p, lev);
            p.idata(FHD_intData::sorted) = 1;
            p.idata(FHD_intData::i) = iv[0];
            p.idata(FHD_intData::j) = iv[1];
            p.idata(FHD_intData::k) = iv[2];
            // note - use 1-based indexing for convenience with Fortran
            //m_cell_vectors[pti.index()](iv).push_back(pindex + 1);

            long imap = tile_box.index(iv);
            m_cell_vectors[pti.index()][imap].push_back(pindex + 1);
        }
    }
    
    UpdateFortranStructures();
}


void
FhdParticleContainer::UpdateFortranStructures()
{
    BL_PROFILE_VAR("UpdateFortranStructures()",UpdateFortranStructures);
    
    const int lev = 0;

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi = MakeMFIter(lev); mfi.isValid(); ++mfi)
    {
        const Box& tile_box  = mfi.tilebox();
        const int grid_id = mfi.index();
        for (IntVect iv = tile_box.smallEnd(); iv <= tile_box.bigEnd(); tile_box.next(iv))
        {
            long imap = tile_box.index(iv);
            m_vector_size[grid_id](iv) = m_cell_vectors[grid_id][imap].size();
            m_vector_ptrs[grid_id](iv) = m_cell_vectors[grid_id][imap].data();
        }
    }
}

void
FhdParticleContainer::ReBin()
{
    BL_PROFILE_VAR("ReBin()",ReBin);
    
    const int lev = 0;

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        const int grid_id = pti.index();
        const int tile_id = pti.LocalTileIndex();
        const Box& tile_box  = pti.tilebox();

        auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
        auto& particles = particle_tile.GetArrayOfStructs();
        const int np = particles.numParticles();
        for(int pindex = 0; pindex < np; ++pindex)
        {
	    ParticleType& p = particles[pindex];
	    if (p.idata(FHD_intData::sorted)) continue;
            const IntVect& iv = this->Index(p, lev);
            p.idata(FHD_intData::sorted) = 1;
            p.idata(FHD_intData::i) = iv[0];
            p.idata(FHD_intData::j) = iv[1];
            p.idata(FHD_intData::k) = iv[2];

            long imap = tile_box.index(iv);
            // note - use 1-based indexing for convenience with Fortran
            m_cell_vectors[pti.index()][imap].push_back(pindex + 1);
        }
    }

    UpdateFortranStructures();
}

void
FhdParticleContainer::PrintParticles()
{
    BL_PROFILE_VAR("PrintParticles()",PrintParticles);
    
    int lev =0;

    for(FhdParIter pti(* this, lev); pti.isValid(); ++pti)
    {
        PairIndex index(pti.index(), pti.LocalTileIndex());

        AoS & particles = this->GetParticles(lev).at(index).GetArrayOfStructs();
        long np = this->GetParticles(lev).at(index).numParticles();

        for(int i=0; i<np; ++i)
        {
            ParticleType & part = particles[i];

            double bigM  = part.rdata(FHD_realData::totalDiff)/(T_init[0]*k_B);

            std::cout << "Particle " << ParallelDescriptor::MyProc() << ", " << i << ", force: " << part.rdata(FHD_realData::forcex) << ", " << part.rdata(FHD_realData::forcey) << ", " << part.rdata(FHD_realData::forcez) << std::endl;
            std::cout << "Particle " << ParallelDescriptor::MyProc() << ", " << i << ", position/q: " << part.pos(0) << ", " << part.pos(1) << ", " << part.pos(2) << ", " << part.rdata(FHD_realData::q) << std::endl;
            std::cout << "Particle " << ParallelDescriptor::MyProc() << ", " << i << ", vel: " << part.rdata(FHD_realData::velx) << ", " << part.rdata(FHD_realData::vely) << ", " << part.rdata(FHD_realData::velz) << std::endl;
            std::cout << "Particle " << ParallelDescriptor::MyProc() << ", " << i << ", normalised mobility: " << (part.rdata(FHD_realData::velx)/part.rdata(FHD_realData::forcex))/bigM << ", " << (part.rdata(FHD_realData::vely)/part.rdata(FHD_realData::forcey))/bigM
                                                                                                                                                                         << ", " << (part.rdata(FHD_realData::velz)/part.rdata(FHD_realData::forcez))/bigM << std::endl;

        }
    }
}

void
FhdParticleContainer::SetPosition(int rank, int id, Real x, Real y, Real z)
{
    BL_PROFILE_VAR("SetPosition()",SetPosition);
    
    int lev =0;

    if(ParallelDescriptor::MyProc() == rank)
    {
        for(FhdParIter pti(* this, lev); pti.isValid(); ++pti)
        {
            PairIndex index(pti.index(), pti.LocalTileIndex());

            AoS & particles = this->GetParticles(lev).at(index).GetArrayOfStructs();
            long np = this->GetParticles(lev).at(index).numParticles();

            if(id <= np)
            {
 
                ParticleType & part = particles[id-1];

                part.pos(0) = x;
                part.pos(1) = y;
                part.pos(2) = z;

                std::cout << "Rank " << ParallelDescriptor::MyProc() << " particle " << id << " moving to " << x << ", " << y << ", " << z << std::endl;
            }else
            {
                std::cout << "Particle does not exist!\n";
            }
           
        }
    }
    clearNeighbors();
    Redistribute();
    UpdateCellVectors();
    ReBin();
    fillNeighbors();
}

void
FhdParticleContainer::SetVel(int rank, int id, Real x, Real y, Real z)
{
    BL_PROFILE_VAR("SetVel()",SetVel);
    
    int lev =0;

    if(ParallelDescriptor::MyProc() == rank)
    {
        for(FhdParIter pti(* this, lev); pti.isValid(); ++pti)
        {
            PairIndex index(pti.index(), pti.LocalTileIndex());

            AoS & particles = this->GetParticles(lev).at(index).GetArrayOfStructs();
            long np = this->GetParticles(lev).at(index).numParticles();
 
            ParticleType & part = particles[id-1];

            part.rdata(FHD_realData::velx) = x;
            part.rdata(FHD_realData::vely) = y;
            part.rdata(FHD_realData::velz) = z;
           
        }
    }
}

void
FhdParticleContainer::MeanSqrCalc(int lev, int reset) {

    BL_PROFILE_VAR("MeanSqrCalc()",MeanSqrCalc);

    Real diffTotal = 0;
    Real tt = 0.; // set to zero to protect against grids with no particles
    long nTotal = 0;
    Real sumPosQ[3] = {0,0,0};

    for (MyIBMarIter pti(* this, lev); pti.isValid(); ++pti) {

        TileIndex index(pti.index(), pti.LocalTileIndex());

        AoS & particles = this->GetParticles(lev).at(index).GetArrayOfStructs();
        long np = this->GetParticles(lev).at(index).numParticles();
        nTotal += np;

        for (int i=0; i<np; ++i) {
            ParticleType & part = particles[i];

            Real sqrPos = 0;
            for (int d=0; d<AMREX_SPACEDIM; ++d){
                sqrPos += pow(part.rdata(FHD_realData::ax + d),2);
                sumPosQ[d] += part.rdata(FHD_realData::ax + d)*part.rdata(FHD_realData::q);
            }
            //std::cout << part.rdata(FHD_realData::ax) << std::endl;

            diffTotal += sqrPos/(6.0*part.rdata(FHD_realData::travelTime));
            tt = part.rdata(FHD_realData::travelTime);
        }

        if(reset == 1)
        {
            for (int i=0; i<np; ++i) {
                ParticleType & part = particles[i];

                for (int d=0; d<AMREX_SPACEDIM; ++d){
                    part.rdata(FHD_realData::ax + d) = 0;
                }
                part.rdata(FHD_realData::travelTime) = 0;
            }
        }
    }

    ParallelDescriptor::ReduceRealSum(sumPosQ[0]);
    ParallelDescriptor::ReduceRealSum(sumPosQ[1]);
    ParallelDescriptor::ReduceRealSum(sumPosQ[2]);

    ParallelDescriptor::ReduceRealSum(diffTotal);
    ParallelDescriptor::ReduceLongSum(nTotal);

    // protect against grids with no particles
    ParallelDescriptor::ReduceRealMax(tt);

    if(ParallelDescriptor::MyProc() == 0) {

        std::string filename = "diffusionEst";
        std::ofstream ofs(filename, std::ofstream::app);
    
        ofs << diffTotal/nTotal << std::endl;
        
        ofs.close();
    }

    if(ParallelDescriptor::MyProc() == 0) {

        Real domainVol = (prob_hi[0]-prob_lo[0])*(prob_hi[1]-prob_lo[1])*(prob_hi[2]-prob_lo[2]);
        Real condTotal = (pow(sumPosQ[0],2) + pow(sumPosQ[1],2) + pow(sumPosQ[2],2))/(6.0*k_B*T_init[0]*domainVol*tt);

        std::string filename = "conductivityEst";
        std::ofstream ofs(filename, std::ofstream::app);
        ofs << condTotal << std::endl;
        ofs.close();
    }
    
}

void
FhdParticleContainer::BuildCorrectionTable(const Real* dx, int setMeasureFinal) {

    BL_PROFILE_VAR("BuildCorrectionTable()",BuildCorrectionTable);

    int lev = 0;

    Real x0,y0,z0,x1,y1,z1, costheta, sintheta, cosphi, sinphi;

    Real dr = threepmCurrentBin*(threepmRange/threepmBins)*dx[0];

    if(threepmCurrentBin == threepmBins)
    {
        setMeasureFinal = 2;
    }
    if(threepmCurrentBin > threepmBins)
    {
        setMeasureFinal = 3;
    }

    for (MyIBMarIter pti(* this, lev); pti.isValid(); ++pti) {

        TileIndex index(pti.index(), pti.LocalTileIndex());

        AoS & particles = this->GetParticles(lev).at(index).GetArrayOfStructs();
        long np = this->GetParticles(lev).at(index).numParticles();

        if(setMeasureFinal == 0)
        {
            

            ParticleType & part0 = particles[0];
            ParticleType & part1 = particles[1];

            x0 = prob_lo[0] + get_uniform_func()*(prob_hi[0]-prob_lo[0]);
            y0 = prob_lo[1] + get_uniform_func()*(prob_hi[1]-prob_lo[1]);
            z0 = prob_lo[2] + get_uniform_func()*(prob_hi[2]-prob_lo[2]);
    
            SetPosition(0, 1, x0, y0, z0);

            get_angles(&costheta, &sintheta, &cosphi, &sinphi);

            x1 = x0 + dr*sintheta*cosphi;
            y1 = y0 + dr*sintheta*sinphi;
            z1 = z0 + dr*costheta;

            SetPosition(0, 2, x1, y1, z1);
        }

        if(setMeasureFinal == 1)
        {
            

            ParticleType & part0 = particles[0];
            ParticleType & part1 = particles[1];

            Real ee = (permittivity*4*3.142);

            Real re = threepmCurrentBin*(threepmRange/threepmBins)/(1);

            Real forceNorm = -(dx[0]*dx[0])*ee/(part0.rdata(FHD_realData::q)*part1.rdata(FHD_realData::q));

            //Print() << "Force: " << part0.rdata(FHD_realData::forcex) << ", " << part0.rdata(FHD_realData::forcey) << ", " << part0.rdata(FHD_realData::forcez) << std::endl;

            Real forceMag = sqrt(pow(part0.rdata(FHD_realData::forcex),2) + pow(part0.rdata(FHD_realData::forcey),2) + pow(part0.rdata(FHD_realData::forcez),2))*forceNorm;

            threepmVals[threepmCurrentBin] = (threepmVals[threepmCurrentBin]*(threepmCurrentSample - 1) + forceMag)/threepmCurrentSample;

            if(forceMag < threepmMin[threepmCurrentBin])
            {
                threepmMin[threepmCurrentBin] = forceMag;
            }

            if(forceMag > threepmMax[threepmCurrentBin])
            {
                threepmMax[threepmCurrentBin] = forceMag;
            }

            Print() << "Bin " << threepmCurrentBin << " norm: " << threepmVals[threepmCurrentBin] << endl;

            threepmCurrentSample++;

            if(threepmCurrentSample > threepmSamples)
            {
                threepmCurrentSample = 1;
                threepmCurrentBin++;
            }             
        }

    }

    if(setMeasureFinal == 2)
    {

        Print() << "Outputting correction data\n";
        std::string filename = "threepmPoints";
        std::ofstream ofs0(filename, std::ofstream::out);

        // normalize by
        for(int i=0;i<threepmBins;i++) {
            ofs0 << threepmPoints[i] << ", ";
        }
        ofs0 << std::endl;
        ofs0.close();

        filename = "threepmMax";
        std::ofstream ofs1(filename, std::ofstream::out);

        // normalize by
        for(int i=0;i<threepmBins;i++) {
            ofs1 << threepmMax[i] << ", ";
        }
        ofs1 << std::endl;
        ofs1.close();

        filename = "threepmMin";
        std::ofstream ofs2(filename, std::ofstream::out);

        // normalize by
        for(int i=0;i<threepmBins;i++) {
            ofs2 << threepmMin[i] << ", ";
        }
        ofs2 << std::endl;
        ofs2.close();

        filename = "threepmVals";
        std::ofstream ofs3(filename, std::ofstream::out);

        // normalize by
        for(int i=0;i<threepmBins;i++) {
            ofs3 << threepmVals[i] << ", ";
        }
        ofs3 << std::endl;
        ofs3.close();

        threepmCurrentBin++;
    }
    
}

//void
//FhdParticleContainer::correctCellVectors(int old_index, int new_index, 
//						int grid, const ParticleType& p)
//{
//    if (not p.idata(FHD_intData::sorted)) return;
//    IntVect iv(p.idata(FHD_intData::i), p.idata(FHD_intData::j), p.idata(FHD_intData::k));
//    
//    auto& cell_vector = m_cell_vectors[grid](iv);
//    for (int i = 0; i < static_cast<int>(cell_vector.size()); ++i) {
//        if (cell_vector[i] == old_index + 1) {
//            cell_vector[i] = new_index + 1;
//            return;
//        }
//    }
//}

int
FhdParticleContainer::numWrongCell()
{
    BL_PROFILE_VAR("numWrongCell()",numWrongCell);
    
    const int lev = 0;
    int num_wrong = 0;
    
#ifdef _OPENMP
#pragma omp parallel reduction(+:num_wrong)
#endif    
    for (FhdParIter pti(*this, lev); pti.isValid(); ++pti)
    {
        auto& particles = pti.GetArrayOfStructs();
        const int np    = pti.numParticles();
        for(int pindex = 0; pindex < np; ++pindex) {
            const ParticleType& p = particles[pindex];
            const IntVect& iv = this->Index(p, lev);
            if ((iv[0] != p.idata(FHD_intData::i)) or (iv[1] != p.idata(FHD_intData::j)) or (iv[2] != p.idata(FHD_intData::k))) {
                num_wrong += 1;
            }
        }
    }
    
    ParallelDescriptor::ReduceIntSum(num_wrong, ParallelDescriptor::IOProcessorNumber());
    return num_wrong;
}

void FhdParticleContainer::PostRestart()
{
    BL_PROFILE_VAR("PostRestart()",PostRestart);
    
    UpdateCellVectors();
}
