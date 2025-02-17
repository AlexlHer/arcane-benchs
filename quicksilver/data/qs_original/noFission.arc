<?xml version="1.0"?>
<case codename="Quicksilver" xml:lang="en" codeversion="1.0">
  <arcane>
    <title>noFission</title>
    <timeloop>QAMALoop</timeloop>
  </arcane>

  <meshes>
    <mesh>
      <generator name="Cartesian3D" >
        <face-numbering-version>1</face-numbering-version>

        <nb-part-x>2</nb-part-x> 
        <nb-part-y>2</nb-part-y>
        <nb-part-z>1</nb-part-z>

        <origin>0.0 0.0 0.0</origin>

        <x>
          <length>100.0</length>
          <n>10</n>
        </x>

        <y>
          <length>100.0</length>
          <n>10</n>
        </y>

        <z>
          <length>100.0</length>
          <n>10</n>
        </z>

      </generator>
    </mesh>
  </meshes>

  <q-s>
    <dt>1e-08</dt>
    <boundaryCondition>octant</boundaryCondition>
    <lx>100</lx>
    <ly>100</ly>
    <lz>100</lz>
    <nSteps>10</nSteps>
    <eMax>20</eMax>
    <eMin>1e-9</eMin>
    <nGroups>230</nGroups>

  </q-s>

  <sampling-m-c>
    <fMax>0.1</fMax>
    <nParticles>10000000</nParticles>
    <seed>1029384756</seed>
  </sampling-m-c>

  <tracking-m-c>
    <geometry>
      <material>sourceMaterial</material>
      <shape>brick</shape>
      <xMax>100</xMax>
      <xMin>0</xMin>
      <yMax>100</yMax>
      <yMin>0</yMin>
      <zMax>100</zMax>
      <zMin>0</zMin>
    </geometry>

    <material>
      <name>sourceMaterial</name>
      <nIsotopes>10</nIsotopes>
      <nReactions>9</nReactions>
      <sourceRate>1e+10</sourceRate>
      <totalCrossSection>0.198</totalCrossSection>
      <absorptionCrossSection>flat</absorptionCrossSection>
      <fissionCrossSection>flat</fissionCrossSection>
      <scatteringCrossSection>flat</scatteringCrossSection>
      <absorptionCrossSectionRatio>0.494949495</absorptionCrossSectionRatio>
      <fissionCrossSectionRatio>0</fissionCrossSectionRatio>
      <scatteringCrossSectionRatio>0.505050505</scatteringCrossSectionRatio>
    </material>

    <cross_section>
      <name>flat</name>
      <A>0</A>
      <B>0</B>
      <C>0</C>
      <D>0</D>
      <E>1</E>
      <nuBar>2.4</nuBar>
    </cross_section>
  </tracking-m-c>

</case>
