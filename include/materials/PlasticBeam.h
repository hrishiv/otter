//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#pragma once

#include "Material.h"
#include "RankTwoTensor.h"

/**
 * PlasticBeam defines a displacement and rotation strain increment and rotation
 * increment (=1), for small strains.
 */

// Forward Declarations
class PlasticBeam;
class Function;

template <>
InputParameters validParams<PlasticBeam>();

class PlasticBeam : public Material
{
public:
  static InputParameters validParams();

  PlasticBeam(const InputParameters & parameters);

  virtual void computeProperties() override;

protected:
  virtual void initQpStatefulProperties() override;

  /// Computes the displacement and rotation strain increments
  void computeQpStrain();

  /// Computes the stiffness matrices
  void computeStiffnessMatrix();

  /// Computes the rotation matrix at time t. For small rotation scenarios, the rotation matrix at time t is same as the intiial rotation matrix
  virtual void computeRotation();

  //
  void computeQpStress();
  virtual Real computeHardeningValue(Real scalar, Real j);
  virtual Real computeHardeningDerivative(Real scalar, Real j);


  /// Booleans for validity of params
  const bool _has_Ix;

  /// Number of coupled rotational variables
  unsigned int _nrot;

  /// Number of coupled displacement variables
  unsigned int _ndisp;

  /// number of x-sec layers to consider
  unsigned int _nlayers;

  /// Variable numbers corresponding to the rotational variables
  std::vector<unsigned int> _rot_num;

  /// Variable numbers corresponding to the displacement variables
  std::vector<unsigned int> _disp_num;

  /// Coupled variable for the beam cross-sectional area
  const VariableValue & _area;

  /// Coupled variable for the beam width
  const Real & _width;

  /// Coupled variable for the beam depth
  const Real & _depth;

  /// Coupled variable for the first moment of area in y direction, i.e., integral of y*dA over the cross-section
  const VariableValue & _Ay;

  /// Coupled variable for the first moment of area in z direction, i.e., integral of z*dA over the cross-section
  const VariableValue & _Az;

  /// Coupled variable for the second moment of area in y direction, i.e., integral of y^2*dA over the cross-section
  const VariableValue & _Iy;

  /// Coupled variable for the second moment of area in z direction, i.e., integral of z^2*dA over the cross-section
  const VariableValue & _Iz;

  /// Coupled variable for the second moment of area in x direction, i.e., integral of (y^2 + z^2)*dA over the cross-section
  const VariableValue & _Ix;

  /// Rotational transformation from global coordinate system to initial beam local configuration
  RankTwoTensor _original_local_config;

  /// Initial length of the beam
  MaterialProperty<Real> & _original_length;

  /// Rotational transformation from global coordinate system to beam local configuration at time t
  MaterialProperty<RankTwoTensor> & _total_rotation;

  /// Current total displacement strain integrated over the cross-section in global coordinate system.
  MaterialProperty<RealVectorValue> & _total_disp_strain;

  /// Current total rotational strain integrated over the cross-section in global coordinate system.
  MaterialProperty<RealVectorValue> & _total_rot_strain;

  /// Old total displacement strain integrated over the cross-section in global coordinate system.
  const MaterialProperty<RealVectorValue> & _total_disp_strain_old;

  /// Old total rotational strain integrated over the cross-section in global coordinate system.
  const MaterialProperty<RealVectorValue> & _total_rot_strain_old;

  /// Mechanical displacement strain increment (after removal of eigenstrains) integrated over the cross-section.
  MaterialProperty<RealVectorValue> & _mech_disp_strain_increment;

  /// Mechanical rotation strain increment (after removal of eigenstrains) integrated over the cross-section
  MaterialProperty<RealVectorValue> & _mech_rot_strain_increment;

  /// Material stiffness vector that relates displacement strain increments to force increments
  const MaterialProperty<RealVectorValue> & _material_stiffness;

  /// Stiffness matrix between displacement DOFs of same node or across nodes
  MaterialProperty<RankTwoTensor> & _K11;

  /// Stiffness matrix between displacement DOFs of one node to rotational DOFs of another node
  MaterialProperty<RankTwoTensor> & _K21_cross;

  /// Stiffness matrix between displacement DOFs and rotation DOFs of the same node
  MaterialProperty<RankTwoTensor> & _K21;

  /// Stiffness matrix between rotation DOFs of the same node
  MaterialProperty<RankTwoTensor> & _K22;

  /// Stiffness matrix between rotation DOFs of different nodes
  MaterialProperty<RankTwoTensor> & _K22_cross;

  /// Boolean flag to turn on large strain calculation
  const bool _large_strain;

  /// Gradient of displacement calculated in the beam local configuration at time t
  RealVectorValue _grad_disp_0_local_t;

  /// Gradient of rotation calculated in the beam local configuration at time t
  RealVectorValue _grad_rot_0_local_t;

  /// Average rotation calculated in the beam local configuration at time t
  RealVectorValue _avg_rot_local_t;

  /// Vector of beam eigenstrain names
  std::vector<MaterialPropertyName> _eigenstrain_names;

  /// Vector of current displacement eigenstrains
  std::vector<const MaterialProperty<RealVectorValue> *> _disp_eigenstrain;

  /// Vector of current rotational eigenstrains
  std::vector<const MaterialProperty<RealVectorValue> *> _rot_eigenstrain;

  /// Vector of old displacement eigenstrains
  std::vector<const MaterialProperty<RealVectorValue> *> _disp_eigenstrain_old;

  /// Vector of old rotational eigenstrains
  std::vector<const MaterialProperty<RealVectorValue> *> _rot_eigenstrain_old;

  /// Displacement and rotations at the two nodes of the beam in the global coordinate system
  RealVectorValue _disp0, _disp1, _rot0, _rot1;

  /// Reference to the nonlinear system object
  NonlinearSystemBase & _nonlinear_sys;
  /// Indices of solution vector corresponding to displacement DOFs at the node 0
  std::vector<unsigned int> _soln_disp_index_0;

  /// Indices of solution vector corresponding to displacement DOFs at the node 1
  std::vector<unsigned int> _soln_disp_index_1;

  /// Indices of solution vector corresponding to rotation DOFs at the node 0
  std::vector<unsigned int> _soln_rot_index_0;

  /// Indices of solution vector corresponding to rotation DOFs at the node 1
  std::vector<unsigned int> _soln_rot_index_1;

  /// Rotational transformation from global coordinate system to initial beam local configuration
  MaterialProperty<RankTwoTensor> & _initial_rotation;

  /// Psuedo stiffness for critical time step computation
  MaterialProperty<Real> & _effective_stiffness;
  /// Prefactor function to multiply the elasticity tensor with
  const Function * const _prefactor_function;


  Real _yield_stress;
  const Real _hardening_constant;
  const Function * _hardening_function;

  /// convergence tolerance
  Real _absolute_tolerance;
  Real _relative_tolerance;

  MaterialProperty<Real> & _total_stretch;
  const MaterialProperty<Real> & _total_stretch_old;

  /// basically, instead of material property that is a vector
  /// we're creating a vector of material properties
  std::vector<MaterialProperty<Real> *> _direct_stress;
  std::vector<const MaterialProperty<Real> *> _direct_stress_old;
  // get rid of these when you're ready. Right now, we're just
  // demonstrating that a vector of mat props works
  std::vector<MaterialProperty<Real> *> _direct_stress_two;
  std::vector<const MaterialProperty<Real> *> _direct_stress_two_old;

  MaterialProperty<RealVectorValue> & _plastic_strain;
  const MaterialProperty<RealVectorValue> & _plastic_strain_old;
  MaterialProperty<RealVectorValue> & _plastic_strain_two;
  const MaterialProperty<RealVectorValue> & _plastic_strain_two_old;
  MaterialProperty<Real> & _stres;
  const MaterialProperty<Real> & _stres_old;
  const MaterialProperty<RealVectorValue> & _moment_old;
  const MaterialProperty<RealVectorValue> & _material_flexure;

  MaterialProperty<RealVectorValue> & _hardening_variable;
  const MaterialProperty<RealVectorValue> & _hardening_variable_old;
  MaterialProperty<RealVectorValue> & _hardening_variable_two;
  const MaterialProperty<RealVectorValue> & _hardening_variable_two_old;


  // std::vector<Real> _bucket;

  /// maximum no. of iterations
  const unsigned int _max_its;


};
