﻿// -*- tab-width: 2; indent-tabs-mode: nil; coding: utf-8-with-signature -*-
//-----------------------------------------------------------------------------
// Copyright 2000-2022 CEA (www.cea.fr) IFPEN (www.ifpenergiesnouvelles.com)
// See the top-level COPYRIGHT file for details.
// SPDX-License-Identifier: Apache-2.0
//-----------------------------------------------------------------------------
/*---------------------------------------------------------------------------*/
/* TrackingMCModule.cc                                         (C) 2000-2022 */
/*                                                                           */
/* Module de tracking QAMA                                                   */
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#include "TrackingMCModule.hh"
#include "MC_RNG_State.hh"
#include "PhysicalConstants.hh"
#include <arcane/Concurrency.h>
#include <map>
#include <set>

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

/**
 * @brief Méthode permettant d'initialiser le ParticleExchanger
 * et les materiaux du maillage.
 */
void TrackingMCModule::
initModule()
{
  m_material_mng = IMeshMaterialMng::getReference(mesh());

  m_particle_family = mesh()->findItemFamily("ArcaneParticles");

  IParticleExchanger* pe = options()->particleExchanger();
  pe->initialize(m_particle_family);

  m_timer = new Timer(subDomain(), "TrackingMC", Timer::TimerReal);

  // Configuration des materiaux.
  initNuclearData();
}

/**
 * @brief Méthode permettant de lancer le suivi des particules.
 */
void TrackingMCModule::
cycleTracking()
{
  {
    Timer::Sentry ts(m_timer);
    computeCrossSection();
    tracking();
    
    if(m_absorb_a != 0 || m_escape_a != 0){
      m_particle_family->compactItems(false);

      // TODO : A retirer lors de la correction du compactItems() dans Arcane.
      m_particle_family->prepareForDump();
    }

    updateTallies();
  }

  Real time = mesh()->parallelMng()->reduce(Parallel::ReduceMax, m_timer->lastActivationTime());
  info() << "--- Tracking duration: " << time << " s ---";

  ISimpleOutput* csv = ServiceBuilder<ISimpleOutput>(subDomain()).getSingleton();
  csv->addElemRow("Tracking", time);
}

/**
 * @brief Méthode appelée à la fin de la boucle en temps.
 */
void TrackingMCModule::
endModule()
{
  delete (m_timer);
}

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

/**
 * @brief Méthode permettant d'initialiser les matériaux du maillage.
 */
void TrackingMCModule::
initNuclearData()
{
  info() << "Initialisation des matériaux";

  m_nuclearData = new NuclearData(m_n_groups(), m_e_min(), m_e_max());

  Integer num_cross_sections = options()->cross_section().size();
  Integer num_materials = options()->material().size();
  Integer num_geometries = options()->geometry().size();
  Integer num_isotopes = 0;

  std::map<String, Integer> crossSection;
  UniqueArray<Polynomial> polynomials;

  for (Integer i = 0; i < num_cross_sections; i++) {
    String cs_name = options()->cross_section[i].getName();

    Real aa = options()->cross_section[i].getA();
    Real bb = options()->cross_section[i].getB();
    Real cc = options()->cross_section[i].getC();
    Real dd = options()->cross_section[i].getD();
    Real ee = options()->cross_section[i].getE();

    crossSection.insert(std::make_pair(cs_name, i));
    polynomials.add(Polynomial(aa, bb, cc, dd, ee));
  }

  UniqueArray<Int32UniqueArray> localIdMat(num_geometries);
  ENUMERATE_CELL (icell, ownCells()) {
    // On prend le dernier materiau de la liste (si des geométries ont des zones communes) comme dans QS original.
    for (Integer i = num_geometries - 1; i >= 0; i--) {
      if (isInGeometry(i, (*icell))) {
        localIdMat[i].add(icell.localId());
        break;
      }
    }
  }

  for (Integer i = 0; i < num_materials; i++) {
    String mat_name = options()->material[i].getName();
    num_isotopes += options()->material[i].getNIsotopes();
    m_material_mng->registerMaterialInfo(mat_name);
    MeshEnvironmentBuildInfo ebi1(mat_name);
    ebi1.addMaterial(mat_name);
    m_material_mng->createEnvironment(ebi1);
  }

  m_material_mng->endCreate();
  m_nuclearData->_isotopes.reserve(num_isotopes);

  ConstArrayView<IMeshMaterial*> materials = m_material_mng->materials();
  {
    MeshMaterialModifier modifier(m_material_mng);
    for (Integer i = 0; i < num_geometries; i++) {
      String materialName = options()->geometry[i].getMaterial();

      for (Integer j = 0; j < num_materials; j++) {
        // Le nom du materiau est "nomEnvironnement_nomMateriau" et nomEnvironnement = nomMateriau.
        if (materials[j]->name() == materialName + "_" + materialName) {
          modifier.addCells(materials[j], localIdMat[i]);
          break;
        }
      }
    }
  }

  for (Integer i = 0; i < num_materials; i++) {
    String mat_name = options()->material[i].getName();
    Real mass = options()->material[i].getMass();
    Integer nIsotopes = options()->material[i].getNIsotopes();
    Integer nReactions = options()->material[i].getNReactions();
    Real sourceRate = options()->material[i].getSourceRate();
    if (mat_name == "defaultMaterial" && sourceRate == 0.0)
      sourceRate = 1e10;
    String fissionCrossSection = options()->material[i].getFissionCrossSection();
    String scatteringCrossSection = options()->material[i].getScatteringCrossSection();
    String absorptionCrossSection = options()->material[i].getAbsorptionCrossSection();
    Real total_cross_section = options()->material[i].getTotalCrossSection();
    Real fissionCrossSectionRatio = options()->material[i].getFissionCrossSectionRatio();
    Real scatteringCrossSectionRatio = options()->material[i].getScatteringCrossSectionRatio();
    Real absorptionCrossSectionRatio = options()->material[i].getAbsorptionCrossSectionRatio();

    Real nuBar = options()->cross_section[crossSection.at(fissionCrossSection)].getNuBar();

    ENUMERATE_MATCELL(icell, materials[i])
    {
      m_mass[icell] = mass;
      m_source_rate[icell] = sourceRate;
    }
    m_iso_gid.resize(nIsotopes);
    m_atom_fraction.resize(nIsotopes);

    for (Integer iIso = 0; iIso < nIsotopes; ++iIso) {
      Integer isotope_gid = m_nuclearData->addIsotope(
      nReactions,
      polynomials[crossSection.at(fissionCrossSection)],
      polynomials[crossSection.at(scatteringCrossSection)],
      polynomials[crossSection.at(absorptionCrossSection)],
      nuBar,
      total_cross_section,
      fissionCrossSectionRatio,
      scatteringCrossSectionRatio,
      absorptionCrossSectionRatio);

      // atom_fraction for each isotope is 1/nIsotopes.  Treats all
      // isotopes as equally prevalent.
      ENUMERATE_MATCELL(icell, materials[i])
      {
        m_iso_gid[icell][iIso] = isotope_gid;
        m_atom_fraction[icell][iIso] = 1.0 / nIsotopes;
      }
    }
  }
}

/**
 * @brief Méthode permettant de suivre les particules jusqu'a que leur temps
 * de census soit atteint.
 * Cette méthode effectue une itération et plusieurs sous-itérations.
 */
void TrackingMCModule::
tracking()
{
  bool done = false;

  // Toutes les particles de m_particle_family sont à suivre.
  ParticleVectorView processing_view = m_particle_family->view();
  ParticleVectorView incoming_particles_view;

  Int32UniqueArray incoming_particles_local_ids;
  Int32UniqueArray extra_clone;

  // Coordonnées des nodes (en cm).
  VariableNodeReal3& node_coord = nodesCoordinates();

  pinfo(3) << "P" << mesh()->parallelMng()->commRank() << " - Tracking of " << processing_view.size() << " particles.";

  IParticleExchanger* pe = options()->particleExchanger();
  //IAsyncParticleExchanger* ae = pe->asyncParticleExchanger();
  if (mesh()->parallelMng()->commSize() > 1) {
    pe->beginNewExchange(-123);
  }

  Integer particle_count = 0; // Initialize count of num_particles processed
  Integer iter = 1;

  while (!done) {
    arcaneParallelForeach(processing_view, [&](ParticleVectorView particles) {
      ENUMERATE_PARTICLE (iparticle, particles) {
        Particle particle = (*iparticle);
        cycleTrackingGuts(particle, node_coord);

        //if (iparticle.index() % 50000 == 0) {
        // pinfo(5) << "--------";
        // pinfo(4) << "P" << mesh()->parallelMng()->commRank() << " - SubIter #" << iter << " - Number of particles processed : " << iparticle.index() << "/" << processing_view.size();
        // pinfo(5) << "  m_local_ids_processed : " << m_census_a << " m_extra_particles_local_ids : " << m_extra_particles_local_ids.size();
        // pinfo(5) << "--------";
        //}
      }
    });

    particle_count += processing_view.size();

    // if (processing_view.size() != 0) {
    // pinfo(4) << "P" << mesh()->parallelMng()->commRank() << " - SubIter #" << iter << " - Number of particles processed : " << processing_view.size() << "/" << processing_view.size();
    // }

    if (mesh()->parallelMng()->commSize() > 1 && incoming_particles_view.size() > 0) {

      // pinfo(5) << "P" << mesh()->parallelMng()->commRank() << " - SubIter #" << iter << " - Computing incoming particles";

      arcaneParallelForeach(incoming_particles_view, [&](ParticleVectorView particles) {
        ENUMERATE_PARTICLE (iparticle, particles) {
          Particle particle = (*iparticle);
          cycleTrackingGuts(particle, node_coord);

          // if (iparticle.index() % 50000 == 0) {
          // pinfo(4) << "P" << mesh()->parallelMng()->commRank() << " - SubIter #" << iter << " - Number of incoming particles processed : " << iparticle.index() << "/" << incoming_particles_view.size();
          // }
        }
      });
      particle_count += incoming_particles_view.size();

      // pinfo(4) << "P" << mesh()->parallelMng()->commRank() << " - SubIter #" << iter << " - Number of incoming particles processed : " << incoming_particles_view.size() << "/" << incoming_particles_view.size();
    }

    // pinfo(5) << "========";
    // pinfo(3) << "P" << mesh()->parallelMng()->commRank() << " - End SubIter #" << iter << " - Total number of particles processed : " << particle_count;
    // pinfo(5) << "  m_exited_particles_local_ids : " << m_exited_particles_local_ids.size() << " m_extra_particles_cellid_dst : " << m_extra_particles_cellid_dst.size() << " m_extra_particles_local_ids : " << m_extra_particles_local_ids.size();
    // pinfo(5) << "  m_local_ids_processed : " << m_census_a << " m_exited_particles_local_ids : " << m_exited_particles_local_ids.size();
    // pinfo(5) << "========";

    // On retire les particules qui sont sortie du maillage.
    m_particle_family->toParticleFamily()->removeParticles(m_exited_particles_local_ids);
    // endUpdate fait par collisionEventSuite;
    m_exited_particles_local_ids.clear();

    // On effectue la suite des collisions, si besoin.
    collisionEventSuite();

    if (mesh()->parallelMng()->commSize() > 1) {
      incoming_particles_local_ids.clear();

      // On essaye de recevoir tant que quelqu'un bosse encore.
      do {
        // Echange particles.
        pe->exchangeItems(m_outgoing_particles_local_ids.size(), m_outgoing_particles_local_ids, m_outgoing_particles_rank_to, &incoming_particles_local_ids, 0);

        done = ((mesh()->parallelMng()->reduce(
                Parallel::ReduceMax,
                incoming_particles_local_ids.size() + m_extra_particles_local_ids.size())) == 0);

        // pinfo(6) << "/////////";
        // pinfo(6) << "Proc#" << mesh()->parallelMng()->commRank();
        // pinfo(6) << "Nb Particles out : " << m_outgoing_particles_local_ids.size();
        // pinfo(6) << "Nb Particles in : " << incoming_particles_local_ids.size();
        // pinfo(6) << "/////////";

        m_outgoing_particles_rank_to.clear();
        m_outgoing_particles_local_ids.clear();
      } while (incoming_particles_local_ids.size() == 0 && m_extra_particles_local_ids.size() == 0 && !done);

      incoming_particles_view = m_particle_family->view(incoming_particles_local_ids);
    }

    else if (m_extra_particles_local_ids.size() == 0) {
      done = true;
    }

    extra_clone = m_extra_particles_local_ids.clone();
    m_extra_particles_local_ids.clear();

    processing_view = m_particle_family->view(extra_clone);
    iter++;
  }

  m_end = m_particle_family->view().size();
}

/**
 * @brief Méthode permettant de copier les atomics dans les variables Arcane.
 */
void TrackingMCModule::
updateTallies()
{
  m_absorb = m_absorb_a;
  m_census = m_census_a;
  m_escape = m_escape_a;
  m_collision = m_collision_a;
  m_fission = m_fission_a;
  m_produce = m_produce_a;
  m_scatter = m_scatter_a;
  m_num_segments = m_num_segments_a;

  m_absorb_a = 0;
  m_census_a = 0;
  m_escape_a = 0;
  m_collision_a = 0;
  m_fission_a = 0;
  m_produce_a = 0;
  m_scatter_a = 0;
  m_num_segments_a = 0;
}

/**
 * @brief Méthode permettant de savoir si une cellule est dans un matériau.
 * On a les dimensions du matériau (en cm) et la position du centre de la cellule
 * (en cm), cette méthode permet de savoir si le centre de la cellule est dans le 
 * matériau.
 * 
 * @param pos La position de la géométrie du matériau dans le fichier d'entrée.
 * @param cell La cellule étudiée.
 * @return true Si la cellule à son centre dans le matériau.
 * @return false Si la cellule n'a pas son centre dans le matériau.
 */
bool TrackingMCModule::
isInGeometry(const Integer& pos, Cell cell)
{
  bool inside = false;
  switch (options()->geometry[pos].getShape()) {
  case eShape::BRICK: {
    Real xMin = ((options()->geometry[pos].getXMin() == -1.0) ? 0.0 : options()->geometry[pos].getXMin());
    Real xMax = ((options()->geometry[pos].getXMax() == -1.0) ? m_lx() : options()->geometry[pos].getXMax());

    Real yMin = ((options()->geometry[pos].getYMin() == -1.0) ? 0.0 : options()->geometry[pos].getYMin());
    Real yMax = ((options()->geometry[pos].getYMax() == -1.0) ? m_ly() : options()->geometry[pos].getYMax());

    Real zMin = ((options()->geometry[pos].getZMin() == -1.0) ? 0.0 : options()->geometry[pos].getZMin());
    Real zMax = ((options()->geometry[pos].getZMax() == -1.0) ? m_lz() : options()->geometry[pos].getZMax());

    if ((m_cell_center_coord[cell][MD_DirX] >= xMin && m_cell_center_coord[cell][MD_DirX] <= xMax) &&
        (m_cell_center_coord[cell][MD_DirY] >= yMin && m_cell_center_coord[cell][MD_DirY] <= yMax) &&
        (m_cell_center_coord[cell][MD_DirZ] >= zMin && m_cell_center_coord[cell][MD_DirZ] <= zMax))
      inside = true;
  } break;

  case eShape::SPHERE: {
    Real xCenter = ((options()->geometry[pos].getXCenter() == -1.0) ? (m_lx() / 2) : options()->geometry[pos].getXCenter());
    Real yCenter = ((options()->geometry[pos].getYCenter() == -1.0) ? (m_ly() / 2) : options()->geometry[pos].getYCenter());
    Real zCenter = ((options()->geometry[pos].getZCenter() == -1.0) ? (m_lz() / 2) : options()->geometry[pos].getZCenter());
    Real radius = ((options()->geometry[pos].getRadius() == -1.0) ? (m_lx() / 2) : options()->geometry[pos].getRadius());

    Real3 center(xCenter, yCenter, zCenter);
    Real3 rr(m_cell_center_coord[cell]);
    if ((rr - center).normL2() <= radius)
      inside = true;
  } break;

  default:
    ARCANE_ASSERT(false, "Pb dans isInGeometry");
  }
  return inside;
}

/**
 * @brief Méthode permettant d'initialiser des grandeurs de la particule puis de lancer son tracking.
 * 
 * @param particle La particule à suivre.
 * @param node_coord Les coordonnées des nodes.
 */
void TrackingMCModule::
cycleTrackingGuts(Particle particle, VariableNodeReal3& node_coord)
{
  if (m_particle_status[particle] == ParticleState::exitedParticle || m_particle_status[particle] == ParticleState::censusParticle) {
    ARCANE_FATAL("Particule déjà traitée.");
  }

  if (m_particle_time_census[particle] <= 0.0) {
    m_particle_time_census[particle] += m_global_deltat();
  }

  if (m_particle_age[particle] < 0.0) {
    m_particle_age[particle] = 0.0;
  }

  m_particle_ene_grp[particle] = m_nuclearData->getEnergyGroup(m_particle_kin_ene[particle]);

  // loop over this particle until we cannot do anything more with it on this processor
  cycleTrackingFunction(particle, node_coord);
}

/**
 * @brief Méthode permettant de suivre la particule p.
 * Suivi de la particule jusqu'a que son temps de suivi (census) soit fini ou
 * jusqu'a qu'elle sorte du maillage ou
 * jusqu'a qu'elle se split (dans ce cas, elle sera resuivi à la prochaine sous-itération).
 * 
 * @param particle La particule à suivre.
 * @param node_coord Les coordonnées des nodes.
 */
void TrackingMCModule::
cycleTrackingFunction(Particle particle, VariableNodeReal3& node_coord)
{
  bool done = false;
  do {
    // Determine the outcome of a particle at the end of this segment such as:
    //
    //   (0) Undergo a collision within the current cell,
    //   (1) Cross a facet of the current cell,
    //   (2) Reach the end of the time step and enter census,
    //
    computeNextEvent(particle, node_coord);
    m_num_segments_a++;

    m_particle_num_seg[particle] += 1.; /* Track the number of segments this particle has
                                          undergone this cycle on all processes. */
    switch (m_particle_last_event[particle]) {
    case ParticleEvent::collision: {

      switch (collisionEvent(particle)) {
      case 0: // La particule est absorbée.
        done = false;
        m_particle_status[particle] = ParticleState::exitedParticle;
        {
          GlobalMutex::ScopedLock(m_mutex_exit);
          m_exited_particles_local_ids.add(particle.localId());
        }
        break;

      case 1: // La particule a juste changée de trajectoire.
        done = true;
        break;

      default: // La particule splitte.
        // On arrete pour pouvoir cloner la particle source.
        done = false;
        break;
      }
    } break;

    // On a la particule sur la face, plus qu'à déterminer la suite.
    case ParticleEvent::faceEventUndefined: {
      facetCrossingEvent(particle);

      switch (m_particle_last_event[particle]) {
      case ParticleEvent::cellChange:
        done = true;
        break;

      case ParticleEvent::escape:
        m_escape_a++;
        done = false;
        m_particle_status[particle] = ParticleState::exitedParticle;
        {
          GlobalMutex::ScopedLock(m_mutex_exit);
          m_exited_particles_local_ids.add(particle.localId());
        }
        break;

      case ParticleEvent::reflection:
        reflectParticle(particle, node_coord);
        done = true;
        break;

      default:
        // Enters an adjacent cell in an off-processor domain.
        done = false;
        // Pas de m_exited_particles_local_ids car la particle sera retirée de la famille par ExchangeParticles.
        break;
      }
    } break;

    case ParticleEvent::census: {
      // The particle has reached the end of the time step.
      // {
      //   GlobalMutex::ScopedLock(m_mutex_processed);
      //   m_local_ids_processed.add(particle.localId());
      // }
      m_census_a++;
      done = false;
      m_particle_status[particle] = ParticleState::censusParticle;
      break;
    }

    default:
      ARCANE_ASSERT(false, "Evenement particle inconnu");
      break;
    }

  } while (done);
}

/**
 * @brief Méthode permettant de finir une collision.
 * Une collision peut aboutir en un dédoublement de particule.
 * Dans ce cas, la particule doit être dédoublé en dehors de l'ENUMERATE_PARTICLE
 * pour que la vue ne soit pas modifié 'en vol' (et donc que le ENUMERATE reste valide).
 */
void TrackingMCModule::
collisionEventSuite()
{
  // On créé les particules.
  Int32UniqueArray particles_lid(m_extra_particles_global_id.size());

  m_particle_family->toParticleFamily()->addParticles(m_extra_particles_global_id, m_extra_particles_cellid_dst, particles_lid);
  m_particle_family->toParticleFamily()->endUpdate();

  // On leur donne les bonnes propriétés.
  cloneParticles(m_extra_particles_particle_src, particles_lid, m_extra_particles_rns);

  ParticleVectorView particles_view = m_particle_family->view(particles_lid);

  // On effectue la suite de la collision.
  arcaneParallelFor(0, particles_view.size(), [&](Integer begin, Integer size) {
    for (Integer i = begin; i < (begin + size); i++) {
      Particle particle(particles_view[i].internal());
      updateTrajectory(m_extra_particles_energy_out[i], m_extra_particles_angle_out[i], particle);
    }
  });
  // ENUMERATE_PARTICLE(ipartic, particles_view) {
  //   Integer index = ipartic.index();
  //   updateTrajectory(m_extra_particles_energy_out[index], m_extra_particles_angle_out[index], (*ipartic));
  // }

  particles_view = m_particle_family->view(m_extra_particles_local_ids);

  arcaneParallelFor(0, particles_view.size(), [&](Integer begin, Integer size) {
    for (Integer i = begin; i < (begin + size); i++) {
      Particle particle(particles_view[i].internal());
      updateTrajectory(m_extra_particles_energy_out_particle_src[i], m_extra_particles_angle_out_particle_src[i], particle);
      m_particle_ene_grp[particle] = m_nuclearData->getEnergyGroup(m_particle_kin_ene[particle]);
    }
  });

  // ENUMERATE_PARTICLE(ipartic, particles_view) {
  //   Integer index = ipartic.index();
  //   updateTrajectory(m_extra_particles_energy_out_particle_src[index], m_extra_particles_angle_out_particle_src[index], (*ipartic));
  //   m_particle_ene_grp[ipartic] = m_nuclearData->getEnergyGroup(m_particle_kin_ene[ipartic]);
  // }

  //m_extra_particles_local_ids.copy(particles_lid);

  //m_extra_particles_local_ids.resize(m_extra_particles_local_ids.size() + particles_lid.size());
  // On fusionne la liste des particules sources et la liste des nouvelles particules.
  for (Integer i = 0; i < particles_lid.size(); i++) {
    m_extra_particles_local_ids.add(particles_lid[i]);
  }

  m_extra_particles_rns.clear();
  m_extra_particles_global_id.clear();
  m_extra_particles_cellid_dst.clear();
  m_extra_particles_particle_src.clear();
  m_extra_particles_energy_out.clear();
  m_extra_particles_angle_out.clear();
  m_extra_particles_energy_out_particle_src.clear();
  m_extra_particles_angle_out_particle_src.clear();
}

/**
 * @brief Méthode permettant de trouver le prochain événement de la particule.
 * On a le choix entre Census, Collision et faceEvent.
 * Census : La particule a fini son itération.
 * Collision : La particule fait une collision avec son environnement.
 * faceEvent : La particule se trouve sur le bord de la cellule et doit faire une action.
 * On choisi l'événement le plus proche.
 * 
 * @param particle La particule à étudier.
 * @param node_coord Les coordonnées des nodes.
 */
void TrackingMCModule::
computeNextEvent(Particle particle, VariableNodeReal3& node_coord)
{
  // initialize distances to large number
  RealUniqueArray distance(3);
  distance[0] = distance[1] = distance[2] = 1e80;

  // Calculate the particle speed
  Real particle_speed = m_particle_velocity[particle].normL2();

  // Force collision if a census event narrowly preempts a collision
  bool force_collision = false;
  if (m_particle_num_mean_free_path[particle] < 0.0) {
    force_collision = true;

    if (m_particle_num_mean_free_path[particle] > -900.0) {
      printf(" computeNextEvent: m_particle_num_mean_free_path[particle] > -900.0 \n");
    }

    m_particle_num_mean_free_path[particle] = PhysicalConstants::_smallDouble;
  }

  // Randomly determine the distance to the next collision
  // based upon the composition of the current cell.
  //Real macroscopic_total_cross_section = weightedMacroscopicCrossSection(particle.cell(), m_particle_ene_grp[particle]);
  Real macroscopic_total_cross_section = m_total[particle.cell()][m_particle_ene_grp[particle]];

  // Cache the cross section
  m_particle_total_cross_section[particle] = macroscopic_total_cross_section;
  if (macroscopic_total_cross_section == 0.0) {
    m_particle_mean_free_path[particle] = PhysicalConstants::_hugeDouble;
  }
  else {
    m_particle_mean_free_path[particle] = 1.0 / macroscopic_total_cross_section;
  }

  if (m_particle_num_mean_free_path[particle] == 0.0) {
    // Sample the number of mean-free-paths remaining before
    // the next collision from an exponential distribution.
    Real random_number = rngSample(&m_particle_rns[particle]);

    m_particle_num_mean_free_path[particle] = -1.0 * std::log(random_number);
  }

  // Calculate the distances to collision, nearest facet, and census.

  // Calculate the minimum distance to the selected events.

  // Force a collision (if required).
  if (force_collision) {
    distance[ParticleEvent::faceEventUndefined] = PhysicalConstants::_hugeDouble;
    distance[ParticleEvent::census] = PhysicalConstants::_hugeDouble;
    // Forced collisions do not need to move far.
    distance[ParticleEvent::collision] = PhysicalConstants::_tinyDouble;

    m_particle_seg_path_length[particle] = PhysicalConstants::_tinyDouble;
    m_particle_last_event[particle] = ParticleEvent::collision;

    // If collision was forced, set mc_particle.num_mean_free_paths = 0
    // so that a new value is randomly selected on next pass.
    m_particle_num_mean_free_path[particle] = 0.0;
  }

  else {
    // Calculate the minimum distance to each facet of the cell.
    DistanceToFacet nearest_facet = getNearestFacet(particle, node_coord);

    distance[ParticleEvent::faceEventUndefined] = nearest_facet.distance;

    // Get out of here if the tracker failed to bound this particle's volume.
    if (m_particle_last_event[particle] == ParticleEvent::faceEventUndefined) {
      return;
    }
    distance[ParticleEvent::collision] = m_particle_num_mean_free_path[particle] * m_particle_mean_free_path[particle];
    // process census
    distance[ParticleEvent::census] = particle_speed * m_particle_time_census[particle];

    // we choose our segment outcome here
    ParticleEvent segment_outcome = (ParticleEvent)findMin(distance);

    m_particle_seg_path_length[particle] = distance[segment_outcome];
    m_particle_last_event[particle] = segment_outcome;

    // Set the segment path length to be the minimum of
    //   (i)   the distance to collision in the cell, or
    //   (ii)  the minimum distance to a facet of the cell, or
    //   (iii) the distance to census at the end of the time step
    if (segment_outcome == ParticleEvent::collision) {
      m_particle_num_mean_free_path[particle] = 0.0;
    }
    else {
      m_particle_num_mean_free_path[particle] -= m_particle_seg_path_length[particle] / m_particle_mean_free_path[particle];
    }

    if (segment_outcome == ParticleEvent::faceEventUndefined) {
      m_particle_face[particle] = nearest_facet.facet / 4;
      m_particle_facet[particle] = nearest_facet.facet % 4;
    }

    else if (segment_outcome == ParticleEvent::census) {
      m_particle_time_census[particle] = std::min(m_particle_time_census[particle], 0.0);
    }

    // Do not perform any tallies if the segment path length is zero.
    //   This only introduces roundoff errors.
    if (m_particle_seg_path_length[particle] == 0.0) {
      return;
    }
  }

  // Move particle to end of segment, accounting for some physics processes along the segment.
  // Project the particle trajectory along the segment path length.

  m_particle_coord[particle][MD_DirX] += (m_particle_dir_cos[particle][MD_DirA] * m_particle_seg_path_length[particle]);
  m_particle_coord[particle][MD_DirY] += (m_particle_dir_cos[particle][MD_DirB] * m_particle_seg_path_length[particle]);
  m_particle_coord[particle][MD_DirZ] += (m_particle_dir_cos[particle][MD_DirG] * m_particle_seg_path_length[particle]);

  Real segment_path_time = (m_particle_seg_path_length[particle] / particle_speed);

  // Decrement the time to census and increment age.
  m_particle_time_census[particle] -= segment_path_time;
  m_particle_age[particle] += segment_path_time;

  // Ensure mc_particle.time_to_census is non-negative.
  if (m_particle_time_census[particle] < 0.0) {
    m_particle_time_census[particle] = 0.0;
  }

  // Accumulate the particle's contribution to the scalar flux.
  // Atomic
  GlobalMutex::ScopedLock(m_mutex_flux);
  m_scalar_flux_tally[particle.cell()][m_particle_ene_grp[particle]] += m_particle_seg_path_length[particle] * m_particle_weight[particle];
}

/**
 * @brief Méthode permettant d'exécuter l'événement Collision sur la particule p.
 * 
 * @param particle La particule à modifier.
 * @return Integer 0 si la particule est absorbé par la maille.
 * 1 si la particule fait simplement une collision.
 * 2 ou plus si la particule doit se splitter.
 */
Integer TrackingMCModule::
collisionEvent(Particle particle)
{
  Cell cell = particle.cell();

  // Pick the isotope and reaction.
  Real random_number = rngSample(&m_particle_rns[particle]);
  Real total_cross_section = m_particle_total_cross_section[particle];
  Real current_cross_section = total_cross_section * random_number;

  Integer selected_iso = -1;
  Integer selected_unique_number = -1;
  Integer selected_react = -1;

  Real cell_number_density = m_cell_number_density[cell];
  ConstArrayView<Real> atom_fraction_av = m_atom_fraction[cell];
  ConstArrayView<Integer> isotope_gid_av = m_iso_gid[cell];

  Integer num_isos = isotope_gid_av.size();

  for (Integer isoIndex = 0; isoIndex < num_isos && current_cross_section >= 0; isoIndex++) {
    const Real atom_fraction = atom_fraction_av[isoIndex];
    const Integer isotope_gid = isotope_gid_av[isoIndex];
    Integer numReacts = m_nuclearData->getNumberReactions(isotope_gid);
    for (Integer reactIndex = 0; reactIndex < numReacts; reactIndex++) {
      current_cross_section -= macroscopicCrossSection(reactIndex, cell_number_density, atom_fraction, isotope_gid,
                                                       isoIndex, m_particle_ene_grp[particle]);
      if (current_cross_section < 0) {
        selected_iso = isoIndex;
        selected_unique_number = isotope_gid;
        selected_react = reactIndex;
        break;
      }
    }
  }
  ARCANE_ASSERT(selected_iso != -1, "selected_iso == -1");

  Integer max_production_size = options()->getMax_production_size();

  // Do the collision.
  RealUniqueArray energyOut(max_production_size);
  RealUniqueArray angleOut(max_production_size);
  Integer nOut = 0;
  Real mat_mass = m_mass[cell];

  m_nuclearData->_isotopes[selected_unique_number]._species[0]._reactions[selected_react].sampleCollision(
  m_particle_kin_ene[particle], mat_mass, energyOut, angleOut, nOut, &(m_particle_rns[particle]), max_production_size);

  m_collision_a++;

// Dans QS original, une particule peut se fissionner en 1 partie (une particule => une particule).
// {nOut = 1 / reactionType = Fission} possible.
#ifdef QS_LEGACY_COMPATIBILITY

  // Set the reaction for this particle.
  NuclearDataReaction::Enum reactionType = m_nuclearData->_isotopes[selected_unique_number]._species[0]._reactions[selected_react]._reactionType;

  switch (reactionType) {
  case NuclearDataReaction::Absorption:
    m_absorb_a++;
    break;
  case NuclearDataReaction::Scatter:
    m_scatter_a++;
    break;
  case NuclearDataReaction::Fission:
    m_fission_a++;
    m_produce_a += nOut;
    break;
  case NuclearDataReaction::Undefined:
    ARCANE_ASSERT(false, "reactionType invalid");
  }

#endif

  // Si nOut == 0, la particule est absorbée.
  if (nOut == 0) {
#ifndef QS_LEGACY_COMPATIBILITY
    m_absorb_a++;
#endif

    return 0;
  }

  // Si nOut == 1, la particule change de trajectoire.
  else if (nOut == 1) {
#ifndef QS_LEGACY_COMPATIBILITY
    m_scatter_a++;
#endif
    updateTrajectory(energyOut[0], angleOut[0], particle);
    m_particle_ene_grp[particle] = m_nuclearData->getEnergyGroup(m_particle_kin_ene[particle]);
  }

  // Si nOut > 1, la particule se "multiplie" et change de trajectoire.
  // Cette étape est effectuée après car on ne peut pas créer des particules
  // dans un ENUMERATE_PARTICLE sans invalider sa vue.
  // Et la particule va dans extra pour continuer à être suivie dans la future
  // sous-itération car on a besoin de ses variables dans leurs états actuels
  // pour créer ses clones (on aurait pu aussi sauvegarder toutes ses variables
  // dans une structure pour pouvoir continuer la poursuite mais ça pourrait
  // devenir lourd si la particule effectue plusieurs fissions).
  // On enregistre les infos pour la future phase création des particules.
  else {
#ifndef QS_LEGACY_COMPATIBILITY
    m_fission_a++;
    m_produce_a += nOut;
#endif

    GlobalMutex::ScopedLock(m_mutex_extra);
    for (Integer i = 1; i < nOut; i++) {
      Int64 rns = rngSpawn_Random_Number_Seed(&m_particle_rns[particle]);
      m_extra_particles_rns.add(rns);
      rns &= ~(1UL << 63);
      m_extra_particles_global_id.add(rns);
      m_extra_particles_cellid_dst.add(particle.cell().localId());
      m_extra_particles_particle_src.add(particle.localId());
      m_extra_particles_energy_out.add(energyOut[i]);
      m_extra_particles_angle_out.add(angleOut[i]);
    }

    m_extra_particles_local_ids.add(particle.localId());
    m_extra_particles_energy_out_particle_src.add(energyOut[0]);
    m_extra_particles_angle_out_particle_src.add(angleOut[0]);
  }

  return nOut;
}

/**
 * @brief Méthode permettant d'exécuter l'événement faceEventUndefined.
 * Et de définir le 'Undefined' :
 * Soit escape (sortie de maillage).
 * Soit reflection (rebondie sur la face)
 * Soit cellChange (changement de maille interne au sous-domaine)
 * Soit subDChange (changement de maille externe au sous-domaine)
 * 
 * @param particle La particule à étudier.
 */
void TrackingMCModule::
facetCrossingEvent(Particle particle)
{
  GlobalMutex::ScopedLock(m_mutex_out);
  Face face = particle.cell().face(m_particle_face[particle]);
  m_particle_last_event[particle] = m_boundary_cond[face];

  if (m_boundary_cond[face] == ParticleEvent::cellChange) {

    // The particle will enter into an adjacent cell.
    Cell cell = face.frontCell();

    if (cell == particle.cell()) {
      cell = face.backCell();
    }

    m_particle_family->toParticleFamily()->setParticleCell(particle, cell);

    if (face.frontCell().owner() != face.backCell().owner()) {
      // The particle will enter into an adjacent cell on a spatial neighbor.
      m_particle_last_event[particle] = ParticleEvent::subDChange;
      m_outgoing_particles_local_ids.add(particle.localId());
      m_outgoing_particles_rank_to.add(cell.owner());
    }
  }
}

/**
 * @brief Méthode permettant d'exécuter l'événement reflection.
 * 
 * @param particle La particule à étudier.
 * @param node_coord Les coordonnées des nodes.
 */
void TrackingMCModule::
reflectParticle(Particle particle, VariableNodeReal3& node_coord)
{
  Real3 facet_normal(m_normal_face[m_particle_face[particle]]);

  Real dot = 2.0 * (m_particle_dir_cos[particle][MD_DirA] * facet_normal.x + m_particle_dir_cos[particle][MD_DirB] * facet_normal.y + m_particle_dir_cos[particle][MD_DirG] * facet_normal.z);

  // do not reflect a particle that is ALREADY pointing inward
  if (dot > 0) {
    // reflect the particle
    m_particle_dir_cos[particle][MD_DirA] -= dot * facet_normal.x;
    m_particle_dir_cos[particle][MD_DirB] -= dot * facet_normal.y;
    m_particle_dir_cos[particle][MD_DirG] -= dot * facet_normal.z;
  }

  // Calculate the reflected, velocity components.
  Real particle_speed = m_particle_velocity[particle].normL2();
  m_particle_velocity[particle][MD_DirX] = particle_speed * m_particle_dir_cos[particle][MD_DirA];
  m_particle_velocity[particle][MD_DirY] = particle_speed * m_particle_dir_cos[particle][MD_DirB];
  m_particle_velocity[particle][MD_DirZ] = particle_speed * m_particle_dir_cos[particle][MD_DirG];
}

/**
 * @brief Méthode permettant de cloner des particules.
 * Copie des propriétés de particules sources vers les particules destinations.
 * La taille des trois tableaux doit être identique.
 * 
 * @param idsSrc Tableau des ids des particules sources.
 * @param idsNew Tableau des ids des particules destinations.
 * @param rnsNew Tableau contenant les graines à donner aux particules.
 */
void TrackingMCModule::
cloneParticles(Int32UniqueArray idsSrc, Int32UniqueArray idsNew, Int64UniqueArray rnsNew)
{
  ParticleVectorView src_particles_view = m_particle_family->view(idsSrc);
  ParticleVectorView cloned_particles_view = m_particle_family->view(idsNew);

  // ENUMERATE_PARTICLE (ipartic, src_particles_view) {
  //   Particle pNew(cloned_particles_view[ipartic.index()].internal());
  //   cloneParticle((*ipartic), pNew, rnsNew[ipartic.index()]);
  // }

  arcaneParallelFor(0, idsSrc.size(), [&](Integer begin, Integer size) {
    for (Integer i = begin; i < (begin + size); i++) {
      Particle p_src(src_particles_view[i].internal());
      Particle p_new(cloned_particles_view[i].internal());
      cloneParticle(p_src, p_new, rnsNew[i]);
    }
  });
}

/**
 * @brief Méthode permettant de cloner une particule.
 * Copie des propriétés d'une particule source vers une particule destination.
 * 
 * @param pSrc La particule source.
 * @param pNew La particule destination.
 * @param rns La graine à donner à la particule destination.
 */
void TrackingMCModule::
cloneParticle(Particle pSrc, Particle pNew, const Int64& rns)
{
  m_particle_rns[pNew] = rns;

  m_particle_coord[pNew][MD_DirX] = m_particle_coord[pSrc][MD_DirX];
  m_particle_coord[pNew][MD_DirY] = m_particle_coord[pSrc][MD_DirY];
  m_particle_coord[pNew][MD_DirZ] = m_particle_coord[pSrc][MD_DirZ];

  m_particle_velocity[pNew][MD_DirX] = m_particle_velocity[pSrc][MD_DirX];
  m_particle_velocity[pNew][MD_DirY] = m_particle_velocity[pSrc][MD_DirY];
  m_particle_velocity[pNew][MD_DirZ] = m_particle_velocity[pSrc][MD_DirZ];

  m_particle_dir_cos[pNew][MD_DirA] = m_particle_dir_cos[pSrc][MD_DirA];
  m_particle_dir_cos[pNew][MD_DirB] = m_particle_dir_cos[pSrc][MD_DirB];
  m_particle_dir_cos[pNew][MD_DirG] = m_particle_dir_cos[pSrc][MD_DirG];

  m_particle_kin_ene[pNew] = m_particle_kin_ene[pSrc];
  m_particle_weight[pNew] = m_particle_weight[pSrc];
  m_particle_time_census[pNew] = m_particle_time_census[pSrc];
  m_particle_total_cross_section[pNew] = m_particle_total_cross_section[pSrc];
  m_particle_age[pNew] = m_particle_age[pSrc];
  m_particle_num_mean_free_path[pNew] = m_particle_num_mean_free_path[pSrc];
  m_particle_mean_free_path[pNew] = m_particle_mean_free_path[pSrc];
  m_particle_seg_path_length[pNew] = m_particle_seg_path_length[pSrc];
  m_particle_last_event[pNew] = m_particle_last_event[pSrc];
  m_particle_num_coll[pNew] = m_particle_num_coll[pSrc];
  m_particle_num_seg[pNew] = m_particle_num_seg[pSrc];
  m_particle_status[pNew] = ParticleState::clonedParticle;
  m_particle_ene_grp[pNew] = m_particle_ene_grp[pSrc];
  m_particle_face[pNew] = m_particle_face[pSrc];
  m_particle_facet[pNew] = m_particle_facet[pSrc];
}

/**
 * @brief Méthode permettant de mettre à jour la trajectoire de la particule.
 * 
 * @param energy Energie cinétique de la particule.
 * @param angle Angle permettant de mettre à jour la trajectoire de la particule.
 * @param particle La particule à traiter.
 */
void TrackingMCModule::
updateTrajectory(const Real& energy, const Real& angle, Particle particle)
{
  m_particle_kin_ene[particle] = energy;
  Real cosTheta = angle;
  Real random_number = rngSample(&m_particle_rns[particle]);
  Real phi = 2 * 3.14159265 * random_number;
  Real sinPhi = sin(phi);
  Real cosPhi = cos(phi);
  Real sinTheta = sqrt((1.0 - (cosTheta * cosTheta)));

  rotate3DVector(particle, sinTheta, cosTheta, sinPhi, cosPhi);

  Real speed = (PhysicalConstants::_speedOfLight *
                sqrt((1.0 - ((PhysicalConstants::_neutronRestMassEnergy * PhysicalConstants::_neutronRestMassEnergy) / ((energy + PhysicalConstants::_neutronRestMassEnergy) * (energy + PhysicalConstants::_neutronRestMassEnergy))))));

  m_particle_velocity[particle][MD_DirX] = speed * m_particle_dir_cos[particle][MD_DirA];
  m_particle_velocity[particle][MD_DirY] = speed * m_particle_dir_cos[particle][MD_DirB];
  m_particle_velocity[particle][MD_DirZ] = speed * m_particle_dir_cos[particle][MD_DirG];

  random_number = rngSample(&m_particle_rns[particle]);
  m_particle_num_mean_free_path[particle] = -1.0 * std::log(random_number);
}

/**
 * @brief Méthode permettant de déterminer les cross sections de toutes les mailles.
 */
void TrackingMCModule::
computeCrossSection()
{
  arcaneParallelForeach(ownCells(), [&](CellVectorView cells) {
    ENUMERATE_CELL (icell, cells) {

      const Real cell_number_density = m_cell_number_density[icell];
      ConstArrayView<Real> atom_fraction_av = m_atom_fraction[icell];
      ConstArrayView<Integer> iso_gid_av = m_iso_gid[icell];
      ArrayView<Real> total = m_total[icell];
      Integer nIsotopes = iso_gid_av.size();

      for (Integer energyGroup = 0; energyGroup < m_n_groups(); energyGroup++) {
        Real sum = 0.0;

        for (Integer isoIndex = 0; isoIndex < nIsotopes; isoIndex++) {
          const Real atom_fraction = atom_fraction_av[isoIndex];
          const Integer isotope_gid = iso_gid_av[isoIndex];
          sum += macroscopicCrossSection(-1, cell_number_density, atom_fraction, isotope_gid, isoIndex, energyGroup);
        }

        total[energyGroup] = sum;
      }
    }
  });
}

/**
 * @brief Méthode permettant de déterminer la distance entre la particule p et la prochaine collision.
 * 
 * @param cell La cellule où se trouve la particule
 * @param energyGroup Le groupe d'energie.
 * @return Real La distance entre la particule et la prochaine collision.
 */
void TrackingMCModule::
weightedMacroscopicCrossSection(Cell cell, const Integer& energyGroup)
{
  Real cell_number_density = m_cell_number_density[cell];
  ConstArrayView<Real> atom_fraction_av = m_atom_fraction[cell];
  ConstArrayView<Integer> iso_gid_av = m_iso_gid[cell];

  Integer nIsotopes = iso_gid_av.size();
  Real sum = 0.0;

  for (Integer isoIndex = 0; isoIndex < nIsotopes; isoIndex++) {
    const Real atom_fraction = atom_fraction_av[isoIndex];
    const Integer isotope_gid = iso_gid_av[isoIndex];
    sum += macroscopicCrossSection(-1, cell_number_density, atom_fraction, isotope_gid, isoIndex, energyGroup);
  }

  m_total[cell][energyGroup] = sum;
}

/**
 * @brief Routine MacroscopicCrossSection calculates the number-density-weighted 
 * macroscopic cross section of a cell.
 * 
 * @param reactionIndex 
 * @param cell 
 * @param isoIndex 
 * @param energyGroup 
 * @return Real 
 */
Real TrackingMCModule::
macroscopicCrossSection(const Integer& reactionIndex,
                        const Real& cell_number_density,
                        const Real& atom_fraction,
                        const Integer& isotope_gid,
                        const Integer& isoIndex,
                        const Integer& energyGroup)
{
  // The cell number density is the fraction of the atoms in cell
  // volume of this isotope.  We set this (elsewhere) to 1/nIsotopes.
  // This is a statement that we treat materials as if all of their
  // isotopes are present in equal amounts

  if (atom_fraction == 0.0 || cell_number_density == 0.0) {
    return 1e-20;
  }

  Real microscopicCrossSection = 0.0;

  if (reactionIndex < 0) {
    // Return total cross section
    microscopicCrossSection = m_nuclearData->getTotalCrossSection(isotope_gid, energyGroup);
  }
  else {
    // Return the reaction cross section
    microscopicCrossSection = m_nuclearData->getReactionCrossSection(reactionIndex,
                                                                     isotope_gid, energyGroup);
  }

  return atom_fraction * cell_number_density * microscopicCrossSection;
}

/**
 * @brief Méthode permettant de trouver la facet la plus proche de la particule p.
 * 
 * @param particle La particule à étudier.
 * @param node_coord Les coordonnées des nodes.
 * @return DistanceToFacet Les caractéristiques de la facet la plus proche.
 */
DistanceToFacet TrackingMCModule::
getNearestFacet(Particle particle, VariableNodeReal3& node_coord)
{
  Cell cell = particle.cell();
  Integer iteration = 0;
  Real move_factor = 0.5 * PhysicalConstants::_smallDouble;
  DistanceToFacet nearest_facet;
  Integer retry = 1;

  Real plane_tolerance = 1e-16 * (m_particle_coord[particle][MD_DirX] * m_particle_coord[particle][MD_DirX] + m_particle_coord[particle][MD_DirY] * m_particle_coord[particle][MD_DirY] + m_particle_coord[particle][MD_DirZ] * m_particle_coord[particle][MD_DirZ]);

  while (retry) // will break out when distance is found
  {
    // Determine the distance to each facet of the cell.
    // (1e-8 * Radius)^2

    DistanceToFacet distance_to_facet[24];

    ENUMERATE_FACE (iface, cell.faces()) {
      Face face = *iface;
      Real3 point2(m_face_center_coord[iface]);

      Integer index = iface.index();
      Integer facet_index = index * 4;

      Real dd = -1.0 *
      (m_normal_face[index].x * point2.x +
       m_normal_face[index].y * point2.y +
       m_normal_face[index].z * point2.z);

      Real facet_normal_dot_direction_cosine =
      (m_normal_face[index].x * m_particle_dir_cos[particle][MD_DirA] +
       m_normal_face[index].y * m_particle_dir_cos[particle][MD_DirB] +
       m_normal_face[index].z * m_particle_dir_cos[particle][MD_DirG]);

      // Consider only those facets whose outer normals have
      // a positive dot product with the direction cosine.
      // I.e. the particle is LEAVING the cell.
      if (facet_normal_dot_direction_cosine <= 0.0) {
        for (Integer i = 0; i < 4; i++) {
          distance_to_facet[facet_index + i].distance = PhysicalConstants::_hugeDouble;
        }
      }
      else {
        for (Integer i = 0; i < 4; i++) {

          Node first_node = face.node(i);
          Node second_node = face.node((i == 3) ? 0 : i + 1);

          Real3 point0(node_coord[first_node]);
          Real3 point1(node_coord[second_node]);

          distance_to_facet[facet_index + i].distance = distanceToSegmentFacet(
          plane_tolerance,
          facet_normal_dot_direction_cosine,
          m_normal_face[index].x, m_normal_face[index].y, m_normal_face[index].z, dd,
          point2, point0, point1,
          particle, false);
        }
      }
    }

    nearest_facet = findNearestFacet(
    particle,
    iteration, move_factor,
    distance_to_facet,
    retry);
  }

  if (nearest_facet.distance < 0) {
    nearest_facet.distance = 0;
  }

  ARCANE_ASSERT(nearest_facet.distance < PhysicalConstants::_hugeDouble, "nearest_facet.distance_to_facet >= PhysicalConstants::_hugeDouble");

  return nearest_facet;
}

/**
 * @brief Méthode permettant de trouver la distance entre une facet et une particule.
 * 
 * @param plane_tolerance 
 * @param facet_normal_dot_direction_cosine 
 * @param A 
 * @param B 
 * @param C 
 * @param D 
 * @param facet_coords0 
 * @param facet_coords1 
 * @param facet_coords2 
 * @param particle 
 * @param allow_enter 
 * @return Real 
 */
Real TrackingMCModule::
distanceToSegmentFacet(const Real& plane_tolerance,
                       const Real& facet_normal_dot_direction_cosine,
                       const Real& A, const Real& B, const Real& C, const Real& D,
                       const Real3& facet_coords0,
                       const Real3& facet_coords1,
                       const Real3& facet_coords2,
                       Particle particle,
                       bool allow_enter)
{
  Real boundingBox_tolerance = 1e-9;
  Real numerator = -1.0 * (A * m_particle_coord[particle][MD_DirX] + B * m_particle_coord[particle][MD_DirY] + C * m_particle_coord[particle][MD_DirZ] + D);

  /* Plane equation: numerator = -P(x,y,z) = -(Ax + By + Cz + D)
      if: numerator < -1e-8*length(x,y,z)   too negative!
      if: numerator < 0 && numerator^2 > ( 1e-8*length(x,y,z) )^2   too negative!
      reverse inequality since squaring function is decreasing for negative inputs.
      If numerator is just SLIGHTLY negative, then the particle is just outside of the face */

  // Filter out too negative distances
  if (!allow_enter && numerator < 0.0 && numerator * numerator > plane_tolerance) {
    return PhysicalConstants::_hugeDouble;
  }

  // we have to restrict the solution to within the triangular face
  Real distance = numerator / facet_normal_dot_direction_cosine;

  // see if the intersection point of the ray and the plane is within the triangular facet
  Real3 intersection_pt;
  intersection_pt.x = m_particle_coord[particle][MD_DirX] + distance * m_particle_dir_cos[particle][MD_DirA];
  intersection_pt.y = m_particle_coord[particle][MD_DirY] + distance * m_particle_dir_cos[particle][MD_DirB];
  intersection_pt.z = m_particle_coord[particle][MD_DirZ] + distance * m_particle_dir_cos[particle][MD_DirG];

  // if the point is completely below the triangle, it is not in the triangle
#define IF_POINT_BELOW_CONTINUE(axis) \
  if (facet_coords0.axis > intersection_pt.axis + boundingBox_tolerance && \
      facet_coords1.axis > intersection_pt.axis + boundingBox_tolerance && \
      facet_coords2.axis > intersection_pt.axis + boundingBox_tolerance) { \
    return PhysicalConstants::_hugeDouble; \
  }

#define IF_POINT_ABOVE_CONTINUE(axis) \
  if (facet_coords0.axis < intersection_pt.axis - boundingBox_tolerance && \
      facet_coords1.axis < intersection_pt.axis - boundingBox_tolerance && \
      facet_coords2.axis < intersection_pt.axis - boundingBox_tolerance) { \
    return PhysicalConstants::_hugeDouble; \
  }

  // Is the intersection point inside the triangular facet?  Project to 2D and see.

  // A^2 + B^2 + C^2 = 1, so max(|A|,|B|,|C|) >= 1/sqrt(3) = 0.577
  // (all coefficients can't be small)
#define AB_CROSS_AC(ax, ay, bx, by, cx, cy) ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax))
  Real cross0 = 0, cross1 = 0, cross2 = 0;
  if (C < -0.5 || C > 0.5) {
    IF_POINT_BELOW_CONTINUE(x);
    IF_POINT_ABOVE_CONTINUE(x);
    IF_POINT_BELOW_CONTINUE(y);
    IF_POINT_ABOVE_CONTINUE(y);

    cross1 = AB_CROSS_AC(facet_coords0.x, facet_coords0.y,
                         facet_coords1.x, facet_coords1.y,
                         intersection_pt.x, intersection_pt.y);
    cross2 = AB_CROSS_AC(facet_coords1.x, facet_coords1.y,
                         facet_coords2.x, facet_coords2.y,
                         intersection_pt.x, intersection_pt.y);
    cross0 = AB_CROSS_AC(facet_coords2.x, facet_coords2.y,
                         facet_coords0.x, facet_coords0.y,
                         intersection_pt.x, intersection_pt.y);
  }
  else if (B < -0.5 || B > 0.5) {
    IF_POINT_BELOW_CONTINUE(x);
    IF_POINT_ABOVE_CONTINUE(x);
    IF_POINT_BELOW_CONTINUE(z);
    IF_POINT_ABOVE_CONTINUE(z);

    cross1 = AB_CROSS_AC(facet_coords0.z, facet_coords0.x,
                         facet_coords1.z, facet_coords1.x,
                         intersection_pt.z, intersection_pt.x);
    cross2 = AB_CROSS_AC(facet_coords1.z, facet_coords1.x,
                         facet_coords2.z, facet_coords2.x,
                         intersection_pt.z, intersection_pt.x);
    cross0 = AB_CROSS_AC(facet_coords2.z, facet_coords2.x,
                         facet_coords0.z, facet_coords0.x,
                         intersection_pt.z, intersection_pt.x);
  }
  else if (A < -0.5 || A > 0.5) {
    IF_POINT_BELOW_CONTINUE(z);
    IF_POINT_ABOVE_CONTINUE(z);
    IF_POINT_BELOW_CONTINUE(y);
    IF_POINT_ABOVE_CONTINUE(y);

    cross1 = AB_CROSS_AC(facet_coords0.y, facet_coords0.z,
                         facet_coords1.y, facet_coords1.z,
                         intersection_pt.y, intersection_pt.z);
    cross2 = AB_CROSS_AC(facet_coords1.y, facet_coords1.z,
                         facet_coords2.y, facet_coords2.z,
                         intersection_pt.y, intersection_pt.z);
    cross0 = AB_CROSS_AC(facet_coords2.y, facet_coords2.z,
                         facet_coords0.y, facet_coords0.z,
                         intersection_pt.y, intersection_pt.z);
  }

  Real cross_tol = 1e-9 * std::abs(cross0 + cross1 + cross2); // cross product tolerance

  if ((cross0 > -cross_tol && cross1 > -cross_tol && cross2 > -cross_tol) ||
      (cross0 < cross_tol && cross1 < cross_tol && cross2 < cross_tol)) {
    return distance;
  }
  return PhysicalConstants::_hugeDouble;
}

/**
 * @brief Méthode permettant de trouver la facet la plus proche de la particule.
 * Cette méthode essaye de trouver la facet la plus proche en appelant nearestFacet.
 * Si on ne trouve pas, la méthode déplace la particule et rééssaye.
 * TODO : Assez moche comme méthode, à changer.
 * 
 */
DistanceToFacet TrackingMCModule::
findNearestFacet(Particle particle,
                 Integer& iteration, // input/output
                 Real& move_factor, // input/output
                 DistanceToFacet* distance_to_facet,
                 Integer& retry /* output */)
{
  DistanceToFacet nearest_facet = nearestFacet(distance_to_facet);

  const Integer max_allowed_segments = 10000000;

  retry = 0;

  if ((nearest_facet.distance == PhysicalConstants::_hugeDouble && move_factor > 0) ||
      (m_particle_num_seg[particle] > max_allowed_segments && nearest_facet.distance <= 0.0)) {

    error() << "Attention, peut-être problème de facet.";
    error() << (nearest_facet.distance == PhysicalConstants::_hugeDouble) << " && " << (move_factor > 0)
            << " || " << (m_particle_num_seg[particle] > max_allowed_segments) << " && " << (nearest_facet.distance <= 0.0);

    // Could not find a solution, so move the particle towards the center of the cell
    // and try again.
    Cell cell = particle.cell();

    m_particle_coord[particle][MD_DirX] += move_factor * (m_cell_center_coord[cell][MD_DirX] - m_particle_coord[particle][MD_DirX]);
    m_particle_coord[particle][MD_DirY] += move_factor * (m_cell_center_coord[cell][MD_DirY] - m_particle_coord[particle][MD_DirY]);
    m_particle_coord[particle][MD_DirZ] += move_factor * (m_cell_center_coord[cell][MD_DirZ] - m_particle_coord[particle][MD_DirZ]);

    iteration++;
    move_factor *= 2.0;

    if (move_factor > 1.0e-2)
      move_factor = 1.0e-2;

    Integer max_iterations = 10;

    if (iteration == max_iterations) {
      ARCANE_FATAL("Problème de facet.");
    }
    else
      retry = 1;
  }
  return nearest_facet;
}

/**
 * @brief Méthode permettant de trouver la facet la plus proche de la particule.
 */
DistanceToFacet TrackingMCModule::
nearestFacet(DistanceToFacet* distance_to_facet)
{
  DistanceToFacet nearest_facet;
  nearest_facet.distance = 1e80;

  // largest negative distance (smallest magnitude, but negative)
  DistanceToFacet nearest_negative_facet;
  nearest_negative_facet.distance = -PhysicalConstants::_hugeDouble;

  // Determine the facet that is closest to the specified coordinates.
  for (Integer facet_index = 0; facet_index < 24; facet_index++) {
    if (distance_to_facet[facet_index].distance > 0.0) {
      if (distance_to_facet[facet_index].distance <= nearest_facet.distance) {
        nearest_facet.distance = distance_to_facet[facet_index].distance;
        nearest_facet.facet = facet_index;
      }
    }
    else // zero or negative distance
    {
      if (distance_to_facet[facet_index].distance > nearest_negative_facet.distance) {
        // smallest in magnitude, but negative
        nearest_negative_facet.distance = distance_to_facet[facet_index].distance;
        nearest_negative_facet.facet = facet_index;
      }
    }
  }

  if (nearest_facet.distance == PhysicalConstants::_hugeDouble) {
    if (nearest_negative_facet.distance != -PhysicalConstants::_hugeDouble) {
      // no positive solution, so allow a negative solution, that had really small magnitude.
      nearest_facet.distance = nearest_negative_facet.distance;
      nearest_facet.facet = nearest_negative_facet.facet;
    }
  }

  return nearest_facet;
}

/**
 * @brief Méthode permettant de trouver le plus petit élément d'un UniqueArray.
 * 
 * @tparam T Le type de l'UniqueArray.
 * @param array L'UniqueArray.
 * @return Integer La position du plus petit élément.
 */
template <typename T>
Integer TrackingMCModule::
findMin(UniqueArray<T> array)
{
  T min = array[0];
  Integer min_index = 0;

  for (Integer element_index = 1; element_index < array.size(); ++element_index) {
    if (array[element_index] < min) {
      min = array[element_index];
      min_index = element_index;
    }
  }

  return min_index;
}

/**
 * @brief Méthode permettant de faire une rotation à une particule.
 * 
 * @param particle La particule à tourner.
 * @param sin_Theta 
 * @param cos_Theta 
 * @param sin_Phi 
 * @param cos_Phi 
 */
void TrackingMCModule::
rotate3DVector(Particle particle, const Real& sin_Theta, const Real& cos_Theta, const Real& sin_Phi, const Real& cos_Phi)
{
  // Calculate additional variables in the rotation matrix.
  Real cos_theta = m_particle_dir_cos[particle][MD_DirG];
  Real sin_theta = sqrt((1.0 - (cos_theta * cos_theta)));

  Real cos_phi;
  Real sin_phi;
  if (sin_theta < 1e-6) // Order of sqrt(PhysicalConstants::tiny_double)
  {
    cos_phi = 1.0; // assume phi  = 0.0;
    sin_phi = 0.0;
  }
  else {
    cos_phi = m_particle_dir_cos[particle][MD_DirA] / sin_theta;
    sin_phi = m_particle_dir_cos[particle][MD_DirB] / sin_theta;
  }

  // Calculate the rotated direction cosine
  m_particle_dir_cos[particle][MD_DirA] = cos_theta * cos_phi * (sin_Theta * cos_Phi) - sin_phi * (sin_Theta * sin_Phi) + sin_theta * cos_phi * cos_Theta;
  m_particle_dir_cos[particle][MD_DirB] = cos_theta * sin_phi * (sin_Theta * cos_Phi) + cos_phi * (sin_Theta * sin_Phi) + sin_theta * sin_phi * cos_Theta;
  m_particle_dir_cos[particle][MD_DirG] = -sin_theta * (sin_Theta * cos_Phi) + cos_theta * cos_Theta;
}
