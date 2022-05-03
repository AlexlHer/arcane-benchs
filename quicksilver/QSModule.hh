// -*- tab-width: 2; indent-tabs-mode: nil; coding: utf-8-with-signature -*-

#include <arcane/IParallelMng.h>
#include <arcane/ITimeLoopMng.h>
#include <arcane/cartesianmesh/CellDirectionMng.h>
#include <arcane/cartesianmesh/ICartesianMesh.h>

#include "structEnum.hh"

#include "MC_Vector.hh"
#include "QS_axl.h"

using namespace Arcane;

/*!
  \brief Module QS.
  Ce module permet d'initialiser les grandeurs au maillage et
  les tallies et permet d'afficher les résultats finaux.
 */
class QSModule : 
public ArcaneQSObject
{

 public:
  explicit QSModule(const ModuleBuildInfo& mbi)
  : ArcaneQSObject(mbi)
  {}

 public:
  void initModule() override;
  void cycleFinalize() override;
  void endModule() override;

  VersionInfo versionInfo() const override { return VersionInfo(1, 0, 0); }

 protected:
  ICartesianMesh* m_cartesian_mesh;

 protected:
  void cycleFinalizeTallies();
  void initMesh();
  void initTallies();
  ParticleEvent getBoundaryCondition(Integer pos);
};

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

ARCANE_REGISTER_MODULE_QS(QSModule);

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
