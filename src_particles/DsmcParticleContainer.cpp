#include "DsmcParticleContainer.H"

//#include "particle_functions_K.H"
#include "paramplane_functions_K.H"
#include <math.h>

// Lifkely excessive
// Relates lower diagonal matrix indices to those of 1D array
int FhdParticleContainer::getSpeciesIndex(int species1, int species2) {
	if(species1<species2){
		return species1+nspecies*(species2-1);
	} else {
		return species2+nspecies*(species1-1);
	}
}

// Same as FhdParticleContainer.H?
FhdParticleContainer::FhdParticleContainer(const Geometry & geom,
                              const DistributionMapping & dmap,
                              const BoxArray            & ba,
                              int ncells)
    : NeighborParticleContainer<FHD_realData::count, FHD_intData::count> (geom, dmap, ba, ncells)
{
    BL_PROFILE_VAR("FhdParticleContainer()",FhdParticleContainer);

    realParticles = 0; // Do we need to keep track of this?
    simParticles = 0;
	 // in AMREX_Math (I think)
	 Real pi_usr = 4.0*atan(1.0);
	 
    totalCollisionCells = n_cells[0]*n_cells[1]*n_cells[2]; // only correct if tile and box same
    domainVol = (prob_hi[0] - prob_lo[0])*(prob_hi[1] - prob_lo[1])*(prob_hi[2] - prob_lo[2]);
    // Print() << "Limits: " << prob_hi[0] << " " << prob_hi[1] << " " << prob_hi[2] << " \n";
    
	 collisionCellVol = domainVol/totalCollisionCells;
	 ocollisionCellVol = 1/collisionCellVol;
	 //Print() << phi_domain << "\n";
    for(int i=0;i<nspecies;i++) {
        properties[i].mass = mass[i];
        properties[i].radius = diameter[i]/2.0;
        properties[i].partVol = pow(diameter[i],3)*pi_usr/6;
        //properties[i].part2cellVol = properties[i].partVol*ocollisionCellVol;
        //properties[i].Neff = phi_domain[i]*(domainVol/properties[i].partVol); // number of real particles
        //properties[i].Neff = properties[i].Neff/properties[i].total;
        //Print() << "Neff: " << properties[i].Neff << ", phi" << phi_domain[i] << "\n";

        if (particle_count[i] >= 0) {
            properties[i].total = particle_count[i];
            // Print() << "Species " << i << " count " << properties[i].total << "\n";
        } 
        else {
            properties[i].total = (int)amrex::Math::ceil(particle_n0[i]*domainVol/particle_neff);
        }
        //realParticles = realParticles + properties[i].total*properties[i].Neff;
        //simParticles = simParticles + properties[i].total;  
    }

    for (int d=0; d<AMREX_SPACEDIM; ++d) {
        domSize[d] = prob_hi[d] - prob_lo[d];
    }

    // Print() << "Total real particles: " << realParticles << "\n";
    // Print() << "Total sim particles: " << simParticles << "\n";

    // Print() << "Collision cells: " << totalCollisionCells << "\n";
    // Print() << "Sim particles per cell: " << simParticles/totalCollisionCells << "\n";

    int lev=0;
    for(MFIter mfi = MakeMFIter(lev, false); mfi.isValid(); ++mfi) {
        const Box& box = mfi.validbox();
        const int grid_id = mfi.index();
        m_cell_vectors[grid_id].resize(box.numPts());
    }


}

// Use to approximate pair correlation fuunc with radial distr. func
Real FhdParticleContainer::g0_Ma_Ahmadi(int species1, int species2){
	const double C1 = 2.5, C2 = 4.5904;
	const double C3 = 4.515439, CPOW = 0.67802;
   // CHANGE: Divide by collision cell volume
   // Should be stored in the boxes of BoxArray
   // Don't think it has been set up quite yet
//	Real phi1 = properties[species1].partVol*properties[species1].Neff*properties[species1].
//	Real phi2 = properties[speices2].partVol
//	if(*species1==*species2) {
//		int indx = getSpecIndx(int i, int j)
//		// ADD: Solid fraction by combination of species in each collision cell
//		// double phi = (PartoCellVol[i])*np[i+NSpecies*(i-1)] // auto-type casted
//		double numer = 1 + C1*PoCVol + C2*pow(PoCVol,2) + C3*pow(PoCVol,3)
//		numer = numer * 4.0*PoCVol
//		// Declare phi
//		double phiRatio = phi/phi_max
//		double denom = pow(1.0 - pow(phiRatio,3),CPOW)
//		return std::min(1+numer/denom,chi_max)
//	} else {
//	 // add Lebowitz RDF here
//	}
	return 0;
}

void FhdParticleContainer::MoveParticlesCPP(const Real dt, const paramPlane* paramPlaneList, const int paramPlaneCount)
{
	BL_PROFILE_VAR("MoveParticlesCPP()", MoveParticlesCPP);

	const int lev = 0;
	const Real* dx = Geom(lev).CellSize();
	const Real* plo = Geom(lev).ProbLo();
	const Real* phi = Geom(lev).ProbHi();

	int        np_tile = 0 ,       np_proc = 0 ; // particle count
	Real    moves_tile = 0.,    moves_proc = 0.; // total moves
	Real  maxspeed_proc = 0.; // max speed
	Real  maxdist_proc = 0.; // max displacement (fraction of radius)

	Real adj = 0.99999;
	Real adjalt = 2.0*(1.0-0.99999);
	Real runtime, inttime;
	int intsurf, intside, push;

	Real maxspeed = 0;
	Real maxdist = 0;

	long moves = 0;
	int reDist = 0;

	for (FhdParIter pti(* this, lev); pti.isValid(); ++pti) {

		const int grid_id = pti.index();
		const int tile_id = pti.LocalTileIndex();
		const Box& tile_box  = pti.tilebox();

		auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
		auto& particles = particle_tile.GetArrayOfStructs();
		const long np = particles.numParticles();

		Box bx  = pti.tilebox();
		IntVect myLo = bx.smallEnd();
		IntVect myHi = bx.bigEnd();

		np_proc += np;

		for (int i = 0; i < np; ++ i) {
			ParticleType & part = particles[i];
			Real speed = 0;

			for (int d=0; d<AMREX_SPACEDIM; ++d) {                   
				speed += part.rdata(FHD_realData::velx + d)*part.rdata(FHD_realData::velx + d);
			}

			if(speed > maxspeed) {
				maxspeed = speed;
			}

			moves++;
			runtime = dt;

			while(runtime > 0) {
				find_inter_gpu(part, runtime, paramPlaneList, paramPlaneCount, &intsurf, &inttime, &intside, ZFILL(plo), ZFILL(phi));

				for (int d=0; d<AMREX_SPACEDIM; ++d) {
					part.pos(d) += inttime * part.rdata(FHD_realData::velx + d)*adj;
				}
				runtime = runtime - inttime;

				if(intsurf > 0) {
					const paramPlane& surf = paramPlaneList[intsurf-1];
					Real posAlt[3];
					for (int d=0; d<AMREX_SPACEDIM; ++d) {
						posAlt[d] = inttime * part.rdata(FHD_realData::velx + d)*adjalt;
					}
					Real dummy = 1;
					app_bc_gpu(&surf, part, intside, domSize, &push, dummy, dummy);
					if(push == 1) {
						for (int d=0; d<AMREX_SPACEDIM; ++d) {
							part.pos(d) += part.pos(d) + posAlt[d];
						}
					}
				}
			}


			int cell[3];
			cell[0] = (int)floor((part.pos(0)-plo[0])/dx[0]);
			cell[1] = (int)floor((part.pos(1)-plo[1])/dx[1]);
			cell[2] = (int)floor((part.pos(2)-plo[2])/dx[2]);

			//cout << "current cell: " << cell[0] << ", " << cell[1] << ", " << cell[2] << "\cout";
			//n << "current pos: " << part.pos(0) << ", " << part.pos(1) << ", " << part.pos(2) << "\n";

			if((cell[0] < myLo[0]) || (cell[1] < myLo[1]) || (cell[2] < myLo[2])) {
				reDist++;
			} 
			else if((cell[0] > myHi[0]) || (cell[1] > myHi[1]) || (cell[2] > myHi[2])) {
				reDist++;
			}

			if((part.idata(FHD_intData::i) != cell[0]) || (part.idata(FHD_intData::j) != cell[1]) || (part.idata(FHD_intData::k) != cell[2])) {
				//remove particle from old cell
				IntVect iv(part.idata(FHD_intData::i), part.idata(FHD_intData::j), part.idata(FHD_intData::k));
				long imap = tile_box.index(iv);
				int lastIndex = m_cell_vectors[pti.index()][imap].size() - 1;
				int lastPart = m_cell_vectors[pti.index()][imap][lastIndex];
				int newIndex = part.idata(FHD_intData::sorted);

				m_cell_vectors[pti.index()][imap][newIndex] = m_cell_vectors[pti.index()][imap][lastIndex];
				m_cell_vectors[pti.index()][imap].pop_back();
            part.idata(FHD_intData::sorted) = -1; // indicates need to be sorted
			}
		}

		maxspeed_proc = amrex::max(maxspeed_proc, maxspeed);
		maxdist_proc  = amrex::max(maxdist_proc, maxdist);
    }

    // gather statistics
    ParallelDescriptor::ReduceIntSum(np_proc);
    ParallelDescriptor::ReduceRealSum(moves_proc);
    ParallelDescriptor::ReduceRealMax(maxspeed_proc);
    ParallelDescriptor::ReduceRealMax(maxdist_proc);
    ParallelDescriptor::ReduceIntSum(reDist);

    // write out global diagnostics
    // if (ParallelDescriptor::IOProcessor()) {
    //    Print() << "I see " << np_proc << " particles\n";
    //    Print() << reDist << " particles to be redistributed.\n";
    //    Print() <<"Maximum observed speed: " << sqrt(maxspeed_proc) << "\n";
    //    Print() <<"Maximum observed displacement (fraction of radius): " << maxdist_proc << "\n";
    //}

    Redistribute();
    SortParticles();
}

void FhdParticleContainer::SortParticles() {
	int lev = 0;
	for (FhdParIter pti(* this, lev); pti.isValid(); ++pti) {
		const int grid_id = pti.index();
		const int tile_id = pti.LocalTileIndex();
		const Box& tile_box  = pti.tilebox();

		auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
		auto& particles = particle_tile.GetArrayOfStructs();
		const long np = particles.numParticles();

		for (int i = 0; i < np; ++ i) {
			ParticleType & part = particles[i];
			if(part.idata(FHD_intData::sorted) == -1) {
				const IntVect& iv = this->Index(part, lev);

				// i,j,k starts at 0 to numTiles-1
				part.idata(FHD_intData::i) = iv[0];
				part.idata(FHD_intData::j) = iv[1];
				part.idata(FHD_intData::k) = iv[2];

				long imap = tile_box.index(iv);

			   // size gives num of particles in the coll cell
				part.idata(FHD_intData::sorted) = m_cell_vectors[pti.index()][imap].size();
				m_cell_vectors[pti.index()][imap].push_back(i);
			}
		}
	}
}

void FhdParticleContainer::EvaluateStats(MultiFab& particleInstant, MultiFab& particleMeans,
                                         MultiFab& particleVars, const Real delt, int steps) {
	BL_PROFILE_VAR("EvaluateStats()",EvaluateStats);
    
	const int lev = 0;
    
	BoxArray ba = particleMeans.boxArray();
	long cellcount = ba.numPts();

	const Real* dx = Geom(lev).CellSize();
	const Real dxInv = 1.0/dx[0];
	const Real cellVolInv = 1.0/(dx[0]*dx[0]*dx[0]);

	const Real stepsInv = 1.0/steps;
	const int stepsMinusOne = steps-1;

	// zero instantaneous values
	particleInstant.setVal(0.);
    
	for (FhdParIter pti(*this, lev); pti.isValid(); ++pti) {
		PairIndex index(pti.index(), pti.LocalTileIndex());
		const int np = this->GetParticles(lev)[index].numRealParticles();
		auto& plev = this->GetParticles(lev);
		auto& ptile = plev[index];
		auto& aos   = ptile.GetArrayOfStructs();
		const Box& tile_box  = pti.tilebox();
		ParticleType* particles = aos().dataPtr();

		GpuArray<int, 3> bx_lo = {tile_box.loVect()[0], tile_box.loVect()[1], tile_box.loVect()[2]};
		GpuArray<int, 3> bx_hi = {tile_box.hiVect()[0], tile_box.hiVect()[1], tile_box.hiVect()[2]};

		Array4<Real> part_inst = particleInstant[pti].array();
		Array4<Real> part_mean = particleMeans[pti].array();
		Array4<Real> part_var = particleVars[pti].array();

		AMREX_FOR_1D( np, ni,
		{
			ParticleType & part = particles[ni];

			int i = floor(part.pos(0)*dxInv);
			int j = floor(part.pos(1)*dxInv);
			int k = floor(part.pos(2)*dxInv);
            
			amrex::Gpu::Atomic::Add(&part_inst(i,j,k,0), 1.0);

			for(int l=0;l<nspecies;l++) {

			}

		});

		amrex::ParallelFor(tile_box,[=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {
			part_inst(i,j,k,1) = part_inst(i,j,k,1)*cellVolInv;         
		});

		amrex::ParallelFor(tile_box,[=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {

			part_mean(i,j,k,1)  = (part_mean(i,j,k,1)*stepsMinusOne + part_inst(i,j,k,1))*stepsInv;

			for(int l=0;l<nspecies;l++)
			{

			}
             
		});

		amrex::ParallelFor(tile_box,[=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {

            Real del1 = part_inst(i,j,k,1) - part_mean(i,j,k,1);

            part_var(i,j,k,1)  = (part_var(i,j,k,1)*stepsMinusOne + del1*del1)*stepsInv;            
		});

	}

}
// Compute selections here?
void FhdParticleContainer::InitCollisionCells() {

	//for(int i=0;i<nspecies;i++) {
	//	properties[i].mass = mass[i];
	//	properties[i].radius = diameter[i]/2.0;
	//	properties[i].partVol = pow(diameter[i],3)*pi_usr*particle_neff/6;
	//	properties[i].Neff = particle_neff;
	//	if (particle_count[i] >= 0) {
	//		properties[i].total = particle_count[i];
	//	} else {
	//		properties[i].total = (int)amrex::Math::ceil(particle_n0[i]*domainVol/particle_neff);
	//	}
	//	realParticles = realParticles + properties[i].total*particle_neff;
	//	simParticles = simParticles + properties[i].total;
	//}
	
	int lev = 0;
	for(MFIter mfi(mfvrmax); mfi.isValid(); ++mfi) {
		//const Box& tile_box  = mfi.tilebox();
		//const int grid_id = mfi.index();
		//const int tile_id = mfi.LocalTileIndex();
		//auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];

		// Convert MultiFabs -> arrays
		const Array4<Real> & arrvrmax = mfvrmax.array(mfi);
		const Array4<Real> & arrphi = mfphi.array(mfi);
		const Array4<Real> & arrselect = mfselect.array(mfi);
		const Array4<Real> & arrnspec = mfnspec.array(mfi);
		//Print() << arrvrmax << "\n";
	}

//	for (FhdParIter pti(* this, lev); pti.isValid(); ++pti) {
//		const int grid_id = pti.index();
//		// Print() << "Grid ID: " << grid_id << " \n";
//		
//		const int tile_id = pti.LocalTileIndex();
//		const Box& tile_box  = pti.tilebox();
//		// const long ptindex = pti.index();
//		
//		auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
//		auto& particles = particle_tile.GetArrayOfStructs();
//		
//		// Shorter/cleaner way?
//		//PairIndex index(pti.index(), pti.LocalTileIndex());
//		//AoS & particles = this->GetParticles(lev).at(index).GetArrayOfStructs();
//		
//		const long np = particles.numParticles();
//		// Track number of particles in each tile per species
//		int numberParticleSpecies[nspecies];
//		
//		// ADD: Flag for 2D vs 3D (likely unneeded)
//		// Loop over collision cells (each tile_box)
//		// initially check if I can grab particles
//		
//		// Problem while using ParallelFor: forgets where pti is pointing
//		
//		// Because each box has one tile, only one index in imap
//		const long imap = 0;
//		
//		
//		//amrex::ParallelFor(tile_box,[=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {

			// Need imap to access m_cell_vectors
			// const IntVect& iv = {i,j,k};
			// long imap = tile_box.index(iv);
			// Print() << "ParallelFor: " << imap << " \n";
			
			// first calculate number of selections
//			for (int i_spec = 0; i_spec<nspecies; i_spec++) {
//				for (int j_spec = 0; j_spec<nspecies, j_spec++) { 
//					spec_pair_index
//					arr_select(i,j,k,l) = arr_select(i,j,k,l	
			
			// test that can extract the particles
//			long np = m_cell_vectors[pti.index()][imap].size();
//			long np = m_cell_vectors[ptindex][imap].size();
//			for(int i_select = 0; i_select < 5; i_select++) {
				// search through particles to find particle that matches chosen id
//				int rand_indx = std::ceil(amrex::Random()*np)-1;
//				int pindx = m_cell_vectors[ptindex][imap][rand_indx];
//				for (int i_part = 0; i_part < np; i_part++) {
//					ParticleType & part = particles[i_part];
//	            if(part.id() == pindx) {
//	            	Print() << "Part ID: " << part.id() << "Random ID: " << pindx << "\n";	
//	            }
//	         }
//	      }
//      });
//	}
}


// Compute selections here?
void FhdParticleContainer::CalcCollisionCells(MultiFab & mfvrmax, MultiFab & mfphi, MultiFab & mfselect) {
	int lev = 0;
	for (FhdParIter pti(* this, lev); pti.isValid(); ++pti) {
		const int grid_id = pti.index();
		Print() << "Grid ID: " << grid_id << " \n";
		
		const int tile_id = pti.LocalTileIndex();
		const Box& tile_box  = pti.tilebox();
		// const long ptindex = pti.index();
		
		auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
		auto& particles = particle_tile.GetArrayOfStructs();
		
		// Shorter/cleaner way?
		//PairIndex index(pti.index(), pti.LocalTileIndex());
		//AoS & particles = this->GetParticles(lev).at(index).GetArrayOfStructs();
		
		const long np = particles.numParticles();
		// Track number of particles in each tile per species
		int numberParticleSpecies[nspecies];
		
		// ADD: Flag for 2D vs 3D (likely unneeded)
		// Loop over collision cells (each tile_box)
		// initially check if I can grab particles
		
		// Problem while using ParallelFor: forgets where pti is pointing
		
		// Because each box has one tile, only one index in imap
		const long imap = 0;
		
		
		//amrex::ParallelFor(tile_box,[=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {

			// Need imap to access m_cell_vectors
			// const IntVect& iv = {i,j,k};
			// long imap = tile_box.index(iv);
			// Print() << "ParallelFor: " << imap << " \n";
			
			// first calculate number of selections
//			for (int i_spec = 0; i_spec<nspecies; i_spec++) {
//				for (int j_spec = 0; j_spec<nspecies, j_spec++) { 
//					spec_pair_index
//					arr_select(i,j,k,l) = arr_select(i,j,k,l	
			
			// test that can extract the particles
//			long np = m_cell_vectors[pti.index()][imap].size();
//			long np = m_cell_vectors[ptindex][imap].size();
//			for(int i_select = 0; i_select < 5; i_select++) {
				// search through particles to find particle that matches chosen id
//				int rand_indx = std::ceil(amrex::Random()*np)-1;
//				int pindx = m_cell_vectors[ptindex][imap][rand_indx];
//				for (int i_part = 0; i_part < np; i_part++) {
//					ParticleType & part = particles[i_part];
//	            if(part.id() == pindx) {
//	            	Print() << "Part ID: " << part.id() << "Random ID: " << pindx << "\n";	
//	            }
//	         }
//	      }
//      });
	}
}

void FhdParticleContainer::CollideParticles(MultiFab & mfselect, MultiFab & mfphi, MultiFab & mfvrmax) {
//	int lev = 0;
//	for (FhdParIter pti(* this, lev); pti.isValid(); ++pti) {
//		const int grid_id = pti.index();
//		const int tile_id = pti.LocalTileIndex();
//		const Box& tile_box  = pti.tilebox();

//	   // make_pair(grid_id,tile_id) -> indicates where tile is where each tile is a collision cell
//	   // tile has both grid and particle info so grab particle info (GetParticles)
//		auto& particle_tile = GetParticles(lev)[std::make_pair(grid_id,tile_id)];
//		// Actual particle data
//		auto& particles = particle_tile.GetArrayOfStructs();

//		// Convert Multifabs to arrays
//		Array4<Real> & arr_select = mfselect.array(pti);
//      const Array4<Real> & arr_phi = mfphi.array(pti);
//      const Array4<Real> & arr_vrmax = mfvrmax.array(pti);
//      
//      // Dummy Vars
//      ParticleType & part;
//		
//		// ADD: Flag for 2D vs 3D (likely unneeded)
//		// Loop over collision cells (each tile_box)
//		// initially check if I can grab particles
//		
//		
//		amrex::ParallelFor(tile_box,[=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept {

//			// Need imap to access m_cell_vectors
//			IntVect iv = {i,j,k};
//			long imap = tile_box.index(iv);
//			
//			// first calculate number of selections
//			for (int i_spec = 0; i_spec<nspecies; i_spec++) {
//				for (int j_spec = 0; j_spec<nspecies, j_spec++) { 
//					spec_pair_index
//					arr_select(i,j,k,l) = arr_select(i,j,k,l	
//			
//			// test that can extract the particles
//			long np = m_cell_vectors[pti.index()][imap].size();
//			for (int l = 0; l < np; l++) {
//	         part = m_cell_vectors[pti.index()][imap]
//            		[std::ceil(amrex::Random()*np)-1];
//            Print() << "Part ID: " << part.id() << "\n";
//         }
//      });
//	}
}
         
//		// for MultiFab mfselect, grab array at pti and store in arr_select
//		// We will be updating the selections
//	   Array4<Real> & arr_select = mfselect.array(pti);
//      const Array4<Real> & arr_phi = mfphi.array(pti);
//      const Array4<Real> & arr_vrmax = mfvrmax.array(pti);

//		for (int i = 0; i < np; ++ i) {
//			ParticleType & part = particles[i];
//			if(part.idata(FHD_intData::sorted) == -1) { // only sort those that have a new cell
//				const IntVect& iv = this->Index(part, lev);

//				part.idata(FHD_intData::i) = iv[0]; // where does i start and end
//				part.idata(FHD_intData::j) = iv[1];
//				part.idata(FHD_intData::k) = iv[2];

//				long imap = tile_box.index(iv);

//				part.idata(FHD_intData::sorted) = m_cell_vectors[pti.index()][imap].size();
//				m_cell_vectors[pti.index()][imap].push_back(i);
//				
//			}
//		}
			

			// for each collision cell, loop over the species and then selections
//			for (int l = 0; l < nspecies; l++) {
//				for (int m = 0; m < nspecies; m++) {
//					
//				Real membersInv = 1.0/part_inst(i,j,k,0);

//            part_inst(i,j,k,1) = part_inst(i,j,k,1)*cellVolInv;

//            part_inst(i,j,k,2) = part_inst(i,j,k,2)*membersInv;
//            part_inst(i,j,k,3) = part_inst(i,j,k,3)*membersInv;
//            part_inst(i,j,k,4) = part_inst(i,j,k,4)*membersInv;

//            part_inst(i,j,k,5) = part_inst(i,j,k,5)*cellVolInv;
//            part_inst(i,j,k,6) = part_inst(i,j,k,6)*cellVolInv;
//            part_inst(i,j,k,7) = part_inst(i,j,k,7)*cellVolInv;

//            for(int l=0;l<nspecies;l++)
//            {
//                part_inst(i,j,k,8 + l) = part_inst(i,j,k,8 + l)*cellVolInv;
//            }
//			}
//		});

//            ParticleType & part = 
//            	m_cell_vectors[pti.index()][imap][std::ceil(rand()*m_cell_vectors[pti.index()][imap].size())-1]
//		for (int i = 0; i < np; ++ i) {
//			ParticleType & part = particles[i];
//			if(part.idata(FHD_intData::sorted) == -1) { // only sort those that have a new cell
//				const IntVect& iv = this->Index(part, lev);

//				part.idata(FHD_intData::i) = iv[0]; // where does i start and end
//				part.idata(FHD_intData::j) = iv[1];
//				part.idata(FHD_intData::k) = iv[2];

//				long imap = tile_box.index(iv);

//				part.idata(FHD_intData::sorted) = m_cell_vectors[pti.index()][imap].size();
//				m_cell_vectors[pti.index()][imap].push_back(i);
//				
//			}
//		}
//	}
