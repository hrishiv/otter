//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#include "LayeredBeam.h"
#include "MooseMesh.h"
#include "Assembly.h"
#include "NonlinearSystem.h"
#include "MooseVariable.h"
#include "Function.h"

#include "libmesh/quadrature.h"
#include "libmesh/utility.h"

registerMooseObject("TensorMechanicsApp", LayeredBeam);

defineLegacyParams(LayeredBeam);

InputParameters
LayeredBeam::validParams()
{
  InputParameters params = Material::validParams();
  params.addClassDescription("Compute a infinitesimal/large strain increment for the beam.");
  params.addRequiredCoupledVar(
      "rotations", "The rotations appropriate for the simulation geometry and coordinate system");
  params.addRequiredCoupledVar(
      "displacements",
      "The displacements appropriate for the simulation geometry and coordinate system");
  params.addRequiredParam<unsigned int>("num_layers",
      "the number of layers to consider for the plastic beam formulation.");
  params.addRequiredParam<RealGradient>("y_orientation",
                                        "Orientation of the y direction along "
                                        "with Iyy is provided. This should be "
                                        "perpendicular to the axis of the beam.");
  params.addRequiredCoupledVar(
      "area",
      "Cross-section area of the beam. Can be supplied as either a number or a variable name.");
  params.addRequiredParam<Real>(
      "width",
      "Width of the beam. Can be supplied as either a number or a variable name.");
  params.addRequiredParam<Real>(
      "depth",
      "Depth of the beam. Can be supplied as either a number or a variable name.");

  params.addCoupledVar("Ay",
                       0.0,
                       "First moment of area of the beam about y axis. Can be supplied "
                       "as either a number or a variable name.");
  params.addCoupledVar("Az",
                       0.0,
                       "First moment of area of the beam about z axis. Can be supplied "
                       "as either a number or a variable name.");
  params.addCoupledVar("Ix",
                       "Second moment of area of the beam about x axis. Can be "
                       "supplied as either a number or a variable name. Defaults to Iy+Iz.");
  params.addRequiredCoupledVar("Iy",
                               "Second moment of area of the beam about y axis. Can be "
                               "supplied as either a number or a variable name.");
  params.addRequiredCoupledVar("Iz",
                               "Second moment of area of the beam about z axis. Can be "
                               "supplied as either a number or a variable name.");
  params.addParam<bool>("large_strain", false, "Set to true if large strain are to be calculated.");
  params.addParam<std::vector<MaterialPropertyName>>(
      "eigenstrain_names", "List of beam eigenstrains to be applied in this strain calculation.");
  params.addParam<FunctionName>(
      "elasticity_prefactor",
      "Optional function to use as a scalar prefactor on the elasticity vector for the beam.");
  params.addRequiredParam<Real>("yield_stress",
                                "Yield stress after which plastic strain starts accumulating");
  params.addParam<Real>("hardening_constant", 0.0, "Hardening slope");
  params.addParam<FunctionName>("hardening_function",
                                "Engineering stress as a function of plastic strain");
  params.addParam<Real>(
      "absolute_tolerance", 1e-10, "Absolute convergence tolerance for Newton iteration");
  params.addParam<Real>(
      "relative_tolerance", 1e-8, "Relative convergence tolerance for Newton iteration");
  return params;
}

LayeredBeam::LayeredBeam(const InputParameters & parameters)
  : Material(parameters),
    _has_Ix(isParamValid("Ix")),
    _nrot(coupledComponents("rotations")),
    _ndisp(coupledComponents("displacements")),
    _nlayers(getParam<unsigned int>("num_layers")),
    _rot_num(_nrot),
    _disp_num(_ndisp),
    _area(coupledValue("area")),
    _width(getParam<Real>("width")),
    _depth(getParam<Real>("depth")),
    _Ay(coupledValue("Ay")),
    _Az(coupledValue("Az")),
    _Iy(coupledValue("Iy")),
    _Iz(coupledValue("Iz")),
    _Ix(_has_Ix ? coupledValue("Ix") : _zero),
    _original_length(declareProperty<Real>("original_length")),
    _total_rotation(declareProperty<RankTwoTensor>("total_rotation")),
    _total_disp_strain(declareProperty<RealVectorValue>("total_disp_strain")),
    _total_rot_strain(declareProperty<RealVectorValue>("total_rot_strain")),
    _total_disp_strain_old(getMaterialPropertyOld<RealVectorValue>("total_disp_strain")),
    _total_rot_strain_old(getMaterialPropertyOld<RealVectorValue>("total_rot_strain")),
    _mech_disp_strain_increment(declareProperty<RealVectorValue>("mech_disp_strain_increment")),
    _mech_rot_strain_increment(declareProperty<RealVectorValue>("mech_rot_strain_increment")),
    _material_stiffness(getMaterialPropertyByName<RealVectorValue>("material_stiffness")),
    _K11(declareProperty<RankTwoTensor>("Jacobian_11")),
    _K21_cross(declareProperty<RankTwoTensor>("Jacobian_12")),
    _K21(declareProperty<RankTwoTensor>("Jacobian_21")),
    _K22(declareProperty<RankTwoTensor>("Jacobian_22")),
    _K22_cross(declareProperty<RankTwoTensor>("Jacobian_22_cross")),
    _large_strain(getParam<bool>("large_strain")),
    _eigenstrain_names(getParam<std::vector<MaterialPropertyName>>("eigenstrain_names")),
    _disp_eigenstrain(_eigenstrain_names.size()),
    _rot_eigenstrain(_eigenstrain_names.size()),
    _disp_eigenstrain_old(_eigenstrain_names.size()),
    _rot_eigenstrain_old(_eigenstrain_names.size()),
    _nonlinear_sys(_fe_problem.getNonlinearSystemBase()),
    _soln_disp_index_0(_ndisp),
    _soln_disp_index_1(_ndisp),
    _soln_rot_index_0(_ndisp),
    _soln_rot_index_1(_ndisp),
    _initial_rotation(declareProperty<RankTwoTensor>("initial_rotation")),
    _effective_stiffness(declareProperty<Real>("effective_stiffness")),
    _prefactor_function(isParamValid("elasticity_prefactor") ? &getFunction("elasticity_prefactor")
                                                             : nullptr),
    _yield_stress(getParam<Real>("yield_stress")), // Read from input file
    _hardening_constant(getParam<Real>("hardening_constant")),
    _hardening_function(isParamValid("hardening_function") ? &getFunction("hardening_function")
                                                           : NULL),
    _absolute_tolerance(parameters.get<Real>("absolute_tolerance")),
    _relative_tolerance(parameters.get<Real>("relative_tolerance")),
    _total_stretch(declareProperty<Real>("total_stretch")),                 //curvature
    _total_stretch_old(getMaterialPropertyOld<Real>("total_stretch")),
    _direct_stress(),
    _direct_stress_old(),
    _plastic_strain(),
    _plastic_strain_old(),
    _stres(declareProperty<Real>("stress_resultant")),
    _stres_old(getMaterialPropertyOld<Real>("stress_resultant")),
    _moment_old(getMaterialPropertyOld<RealVectorValue>("moments")),
    _material_flexure(getMaterialPropertyByName<RealVectorValue>("material_flexure")),
    _hardening_variable(),
    _hardening_variable_old(),
    _max_its(1000)

{
  // Checking for consistency between length of the provided displacements and rotations vector
  if (_ndisp != _nrot)
    mooseError("LayeredBeam: The number of variables supplied in 'displacements' "
               "and 'rotations' must match.");

  // fetch coupled variables and gradients (as stateful properties if necessary)
  for (unsigned int i = 0; i < _ndisp; ++i)
  {
    MooseVariable * disp_variable = getVar("displacements", i);
    _disp_num[i] = disp_variable->number();

    MooseVariable * rot_variable = getVar("rotations", i);
    _rot_num[i] = rot_variable->number();
  }

  if (_large_strain && (_Ay[0] > 0.0 || _Ay[1] > 0.0 || _Az[0] > 0.0 || _Az[1] > 0.0))
    mooseError("LayeredBeam: Large strain calculation does not currently "
               "support asymmetric beam configurations with non-zero first or third moments of "
               "area.");

  for (unsigned int i = 0; i < _eigenstrain_names.size(); ++i)
  {
    _disp_eigenstrain[i] = &getMaterialProperty<RealVectorValue>("disp_" + _eigenstrain_names[i]);
    _rot_eigenstrain[i] = &getMaterialProperty<RealVectorValue>("rot_" + _eigenstrain_names[i]);
    _disp_eigenstrain_old[i] =
        &getMaterialPropertyOld<RealVectorValue>("disp_" + _eigenstrain_names[i]);
    _rot_eigenstrain_old[i] =
        &getMaterialPropertyOld<RealVectorValue>("rot_" + _eigenstrain_names[i]);
  }


  _direct_stress.resize(_nlayers);
  _direct_stress_old.resize(_nlayers);
  _plastic_strain.resize(_nlayers);
  _plastic_strain_old.resize(_nlayers);
  _hardening_variable.resize(_nlayers);
  _hardening_variable_old.resize(_nlayers);

  for (unsigned int i = 0; i < _nlayers; ++i)
  {
    _direct_stress[i] = &declareProperty<Real>("direct_stress" + Moose::stringify(i));
    _direct_stress_old[i] = &getMaterialPropertyOld<Real>("direct_stress" + Moose::stringify(i));
    _plastic_strain[i] = &declareProperty<Real>("plastic_strain" + Moose::stringify(i));
    _plastic_strain_old[i] = &getMaterialPropertyOld<Real>("plastic_strain" + Moose::stringify(i));
    _hardening_variable[i] = &declareProperty<Real>("hardening_variable" + Moose::stringify(i));
    _hardening_variable_old[i] = &getMaterialPropertyOld<Real>("hardening_variable" + Moose::stringify(i));

  }
}

void
LayeredBeam::initQpStatefulProperties()
{
  _total_stretch[_qp] = 0.0;

  for (unsigned int i = 0; i < _nlayers; ++i)
  {
    (*_direct_stress[i])[_qp] = 0.0;
    (*_plastic_strain[i])[_qp] = 0.0;
    (*_hardening_variable[i])[_qp] = 0.0;
  }

  _stres[_qp] = 0.0;

  // compute initial orientation of the beam for calculating initial rotation matrix
  const std::vector<RealGradient> * orientation =
      &_subproblem.assembly(_tid).getFE(FEType(), 1)->get_dxyzdxi();
  RealGradient x_orientation = (*orientation)[0];
  x_orientation /= x_orientation.norm();

  RealGradient y_orientation = getParam<RealGradient>("y_orientation");
  y_orientation /= y_orientation.norm();
  Real sum = x_orientation(0) * y_orientation(0) + x_orientation(1) * y_orientation(1) +
             x_orientation(2) * y_orientation(2);

  if (std::abs(sum) > 1e-4)
    mooseError("LayeredBeam: y_orientation should be perpendicular to "
               "the axis of the beam.");

  // Calculate z orientation as a cross product of the x and y orientations
  RealGradient z_orientation;
  z_orientation(0) = (x_orientation(1) * y_orientation(2) - x_orientation(2) * y_orientation(1));
  z_orientation(1) = (x_orientation(2) * y_orientation(0) - x_orientation(0) * y_orientation(2));
  z_orientation(2) = (x_orientation(0) * y_orientation(1) - x_orientation(1) * y_orientation(0));

  // Rotation matrix from global to original beam local configuration
  _original_local_config(0, 0) = x_orientation(0);
  _original_local_config(0, 1) = x_orientation(1);
  _original_local_config(0, 2) = x_orientation(2);
  _original_local_config(1, 0) = y_orientation(0);
  _original_local_config(1, 1) = y_orientation(1);
  _original_local_config(1, 2) = y_orientation(2);
  _original_local_config(2, 0) = z_orientation(0);
  _original_local_config(2, 1) = z_orientation(1);
  _original_local_config(2, 2) = z_orientation(2);

  _total_rotation[_qp] = _original_local_config;

  RealVectorValue temp;
  _total_disp_strain[_qp] = temp;
  _total_rot_strain[_qp] = temp;
}

void
LayeredBeam::computeProperties()
{
  // fetch the two end nodes for current element
  std::vector<const Node *> node;
  for (unsigned int i = 0; i < 2; ++i)
    node.push_back(_current_elem->node_ptr(i));

  // calculate original length of a beam element
  // Nodal positions do not change with time as undisplaced mesh is used by material classes by
  // default
  RealGradient dxyz;
  for (unsigned int i = 0; i < _ndisp; ++i)
    dxyz(i) = (*node[1])(i) - (*node[0])(i);

  _original_length[0] = dxyz.norm();

  // Fetch the solution for the two end nodes at time t
  const NumericVector<Number> & sol = *_nonlinear_sys.currentSolution();
  const NumericVector<Number> & sol_old = _nonlinear_sys.solutionOld();


  std::cout<<std::endl<<"current soln ="<<sol<<std::endl;
  std::cout<<"old soln ="<<sol_old<<std::endl;


  for (unsigned int i = 0; i < _ndisp; ++i)
  {
    _soln_disp_index_0[i] = node[0]->dof_number(_nonlinear_sys.number(), _disp_num[i], 0);
    _soln_disp_index_1[i] = node[1]->dof_number(_nonlinear_sys.number(), _disp_num[i], 0);
    _soln_rot_index_0[i] = node[0]->dof_number(_nonlinear_sys.number(), _rot_num[i], 0);
    _soln_rot_index_1[i] = node[1]->dof_number(_nonlinear_sys.number(), _rot_num[i], 0);

    _disp0(i) = sol(_soln_disp_index_0[i]) - sol_old(_soln_disp_index_0[i]);
    _disp1(i) = sol(_soln_disp_index_1[i]) - sol_old(_soln_disp_index_1[i]);
    _rot0(i) = sol(_soln_rot_index_0[i]) - sol_old(_soln_rot_index_0[i]);
    _rot1(i) = sol(_soln_rot_index_1[i]) - sol_old(_soln_rot_index_1[i]);
  }

  // For small rotation problems, the rotation matrix is essentially the transformation from the
  // global to original beam local configuration and is never updated. This method has to be
  // overriden for scenarios with finite rotation
  computeRotation();
  _initial_rotation[0] = _original_local_config;

  for (_qp = 0; _qp < _qrule->n_points(); ++_qp)
    computeQpStrain();

  if (_fe_problem.currentlyComputingJacobian())
    computeStiffnessMatrix();
}

void
LayeredBeam::computeQpStrain()
{
  const Real A_avg = (_area[0] + _area[1]) / 2.0;
  const Real Iz_avg = (_Iz[0] + _Iz[1]) / 2.0;
  Real Ix = _Ix[_qp];
  if (!_has_Ix)
    Ix = _Iy[_qp] + _Iz[_qp];

  // Rotate the gradient of displacements and rotations at t+delta t from global coordinate
  // frame to beam local coordinate frame
  const RealVectorValue grad_disp_0(1.0 / _original_length[0] * (_disp1 - _disp0));
  const RealVectorValue grad_rot_0(1.0 / _original_length[0] * (_rot1 - _rot0));
  const RealVectorValue avg_rot(
      0.5 * (_rot0(0) + _rot1(0)), 0.5 * (_rot0(1) + _rot1(1)), 0.5 * (_rot0(2) + _rot1(2)));

  _grad_disp_0_local_t = _total_rotation[0] * grad_disp_0;
  _grad_rot_0_local_t = _total_rotation[0] * grad_rot_0;
  _avg_rot_local_t = _total_rotation[0] * avg_rot;


  std::cout<<"rot 0 ="<<_rot0<<" rot 1 ="<<_rot1<<std::endl;
  _total_stretch[_qp] = _grad_rot_0_local_t(2);
  // std::cout<<"curvature vector = "<<_grad_rot_0_local_t<<std::endl;

  computeQpStress();
  // std::cout<<"curvature = "<<_grad_rot_0_local_t(2)<<std::endl;
  //

  // displacement at any location on beam in local coordinate system at t
  // u_1 = u_n1 - rot_3 * y + rot_2 * z
  // u_2 = u_n2 - rot_1 * z
  // u_3 = u_n3 + rot_1 * y
  // where u_n1, u_n2, u_n3 are displacements at neutral axis

  // small strain
  // e_11 = u_1,1 = u_n1, 1 - rot_3, 1 * y + rot_2, 1 * z
  // e_12 = 2 * 0.5 * (u_1,2 + u_2,1) = (- rot_3 + u_n2,1 - rot_1,1 * z)
  // e_13 = 2 * 0.5 * (u_1,3 + u_3,1) = (rot_2 + u_n3,1 + rot_1,1 * y)

  // axial and shearing strains at each qp along the length of the beam
  _mech_disp_strain_increment[_qp](0) = _grad_disp_0_local_t(0) * _area[_qp] -
                                        _grad_rot_0_local_t(2) * _Ay[_qp] +
                                        _grad_rot_0_local_t(1) * _Az[_qp];
  _mech_disp_strain_increment[_qp](1) = -_avg_rot_local_t(2) * _area[_qp] +
                                        _grad_disp_0_local_t(1) * _area[_qp] -
                                        _grad_rot_0_local_t(0) * _Az[_qp];
  _mech_disp_strain_increment[_qp](2) = _avg_rot_local_t(1) * _area[_qp] +
                                        _grad_disp_0_local_t(2) * _area[_qp] +
                                        _grad_rot_0_local_t(0) * _Ay[_qp];

  // rotational strains at each qp along the length of the beam
  // rot_strain_1 = integral(e_13 * y - e_12 * z) dA
  // rot_strain_2 = integral(e_11 * z) dA
  // rot_strain_3 = integral(e_11 * -y) dA
  // Iyz is the product moment of inertia which is zero for most cross-sections so it is assumed to
  // be zero for this analysis
  const Real Iyz = 0;
  _mech_rot_strain_increment[_qp](0) =
      _avg_rot_local_t(1) * _Ay[_qp] + _grad_disp_0_local_t(2) * _Ay[_qp] +
      _grad_rot_0_local_t(0) * Ix + _avg_rot_local_t(2) * _Az[_qp] -
      _grad_disp_0_local_t(1) * _Az[_qp];
  _mech_rot_strain_increment[_qp](1) = _grad_disp_0_local_t(0) * _Az[_qp] -
                                       _grad_rot_0_local_t(2) * Iyz +
                                       _grad_rot_0_local_t(1) * _Iz[_qp];
  _mech_rot_strain_increment[_qp](2) = -_grad_disp_0_local_t(0) * _Ay[_qp] +
                                       _grad_rot_0_local_t(2) * _Iy[_qp] -
                                       _grad_rot_0_local_t(1) * Iyz;

  if (_large_strain)
  {
    _mech_disp_strain_increment[_qp](0) +=
        0.5 *
        ((Utility::pow<2>(_grad_disp_0_local_t(0)) + Utility::pow<2>(_grad_disp_0_local_t(1)) +
          Utility::pow<2>(_grad_disp_0_local_t(2))) *
             _area[_qp] +
         Utility::pow<2>(_grad_rot_0_local_t(2)) * _Iy[_qp] +
         Utility::pow<2>(_grad_rot_0_local_t(1)) * _Iz[_qp] +
         Utility::pow<2>(_grad_rot_0_local_t(0)) * Ix);
    _mech_disp_strain_increment[_qp](1) += (-_avg_rot_local_t(2) * _grad_disp_0_local_t(0) +
                                            _avg_rot_local_t(0) * _grad_disp_0_local_t(2)) *
                                           _area[_qp];
    _mech_disp_strain_increment[_qp](2) += (_avg_rot_local_t(1) * _grad_disp_0_local_t(0) -
                                            _avg_rot_local_t(0) * _grad_disp_0_local_t(1)) *
                                           _area[_qp];

    _mech_rot_strain_increment[_qp](0) += -_avg_rot_local_t(1) * _grad_rot_0_local_t(2) * _Iy[_qp] +
                                          _avg_rot_local_t(2) * _grad_rot_0_local_t(1) * _Iz[_qp];
    _mech_rot_strain_increment[_qp](1) += (_grad_disp_0_local_t(0) * _grad_rot_0_local_t(1) -
                                           _grad_disp_0_local_t(1) * _grad_rot_0_local_t(0)) *
                                          _Iz[_qp];
    _mech_rot_strain_increment[_qp](2) += -(_grad_disp_0_local_t(2) * _grad_rot_0_local_t(0) -
                                           _grad_disp_0_local_t(0) * _grad_rot_0_local_t(2)) *
                                          _Iy[_qp];
  }

  _total_disp_strain[_qp] = _total_rotation[0].transpose() * _mech_disp_strain_increment[_qp] +
                            _total_disp_strain_old[_qp];
  _total_rot_strain[_qp] = _total_rotation[0].transpose() * _mech_rot_strain_increment[_qp] +
                           _total_disp_strain_old[_qp];

  // Convert eigenstrain increment from global to beam local coordinate system and remove eigen
  // strain increment
  for (unsigned int i = 0; i < _eigenstrain_names.size(); ++i)
  {
    _mech_disp_strain_increment[_qp] -=
        _total_rotation[0] * ((*_disp_eigenstrain[i])[_qp] - (*_disp_eigenstrain_old[i])[_qp]) *
        _area[_qp];
    _mech_rot_strain_increment[_qp] -=
        _total_rotation[0] * ((*_rot_eigenstrain[i])[_qp] - (*_rot_eigenstrain_old[i])[_qp]);
  }

  Real c1_paper = std::sqrt(_material_stiffness[0](0));
  Real c2_paper = std::sqrt(_material_stiffness[0](1));

  Real effec_stiff_1 = std::max(c1_paper, c2_paper);

  Real effec_stiff_2 = 2 / (c2_paper * std::sqrt(A_avg / Iz_avg));

  _effective_stiffness[_qp] = std::max(effec_stiff_1, _original_length[0] / effec_stiff_2);

  if (_prefactor_function)
    _effective_stiffness[_qp] *= std::sqrt(_prefactor_function->value(_t, _q_point[_qp]));
}

void
LayeredBeam::computeStiffnessMatrix()
{
  const Real youngs_modulus = _material_stiffness[0](0);
  const Real shear_modulus = _material_stiffness[0](1);

  const Real A_avg = (_area[0] + _area[1]) / 2.0;
  const Real Iy_avg = (_Iy[0] + _Iy[1]) / 2.0;
  const Real Iz_avg = (_Iz[0] + _Iz[1]) / 2.0;
  Real Ix_avg = (_Ix[0] + _Ix[1]) / 2.0;
  if (!_has_Ix)
    Ix_avg = Iy_avg + Iz_avg;

  // K = |K11 K12|
  //     |K21 K22|

  // relation between translational displacements at node 0 and translational forces at node 0
  RankTwoTensor K11_local;
  K11_local.zero();
  K11_local(0, 0) = youngs_modulus * A_avg / _original_length[0];
  K11_local(1, 1) = shear_modulus * A_avg / _original_length[0];
  K11_local(2, 2) = shear_modulus * A_avg / _original_length[0];
  _K11[0] = _total_rotation[0].transpose() * K11_local * _total_rotation[0];

  // relation between displacements at node 0 and rotational moments at node 0
  RankTwoTensor K21_local;
  K21_local.zero();
  K21_local(2, 1) = shear_modulus * A_avg * 0.5;
  K21_local(1, 2) = -shear_modulus * A_avg * 0.5;
  _K21[0] = _total_rotation[0].transpose() * K21_local * _total_rotation[0];

  // relation between rotations at node 0 and rotational moments at node 0
  RankTwoTensor K22_local;
  K22_local.zero();
  K22_local(0, 0) = shear_modulus * Ix_avg / _original_length[0];
  K22_local(1, 1) = youngs_modulus * Iz_avg / _original_length[0] +
                    shear_modulus * A_avg * _original_length[0] / 4.0;
  K22_local(2, 2) = youngs_modulus * Iy_avg / _original_length[0] +
                    shear_modulus * A_avg * _original_length[0] / 4.0;
  _K22[0] = _total_rotation[0].transpose() * K22_local * _total_rotation[0];

  // relation between rotations at node 0 and rotational moments at node 1
  RankTwoTensor K22_local_cross = -K22_local;
  K22_local_cross(1, 1) += 2.0 * shear_modulus * A_avg * _original_length[0] / 4.0;
  K22_local_cross(2, 2) += 2.0 * shear_modulus * A_avg * _original_length[0] / 4.0;
  _K22_cross[0] = _total_rotation[0].transpose() * K22_local_cross * _total_rotation[0];

  // relation between displacements at node 0 and rotational moments at node 1
  _K21_cross[0] = -_K21[0];

  // stiffness matrix for large strain
  if (_large_strain)
  {
    // k1_large is the stiffness matrix obtained from sigma_xx * d(epsilon_xx)
    RankTwoTensor k1_large_11;
    // row 1
    k1_large_11(0, 0) = Utility::pow<2>(_grad_disp_0_local_t(0)) +
                        1.5 * Utility::pow<2>(_grad_rot_0_local_t(2)) * Iy_avg +
                        1.5 * Utility::pow<2>(_grad_rot_0_local_t(1)) * Iz_avg +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(1)) +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(2)) +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(0)) * Ix_avg;
    k1_large_11(1, 0) = 0.5 * _grad_disp_0_local_t(0) * _grad_disp_0_local_t(1) -
                        1.0 / 3.0 * _grad_rot_0_local_t(0) * _grad_rot_0_local_t(1) * Iz_avg;
    k1_large_11(2, 0) = 0.5 * _grad_disp_0_local_t(0) * _grad_disp_0_local_t(2) -
                        1.0 / 3.0 * _grad_rot_0_local_t(0) * _grad_rot_0_local_t(2) * Iy_avg;

    // row 2
    k1_large_11(0, 1) = k1_large_11(1, 0);
    k1_large_11(1, 1) = Utility::pow<2>(_grad_disp_0_local_t(1)) +
                        1.5 * Utility::pow<2>(_grad_rot_0_local_t(0)) * Iz_avg +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(0)) +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(2)) +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(2)) * Iy_avg +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(1)) * Iz_avg +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(0)) * Iy_avg;
    k1_large_11(2, 1) = 0.5 * _grad_disp_0_local_t(1) * _grad_disp_0_local_t(2);

    // row 3
    k1_large_11(0, 2) = k1_large_11(2, 0);
    k1_large_11(1, 2) = k1_large_11(2, 1);
    k1_large_11(2, 2) = Utility::pow<2>(_grad_disp_0_local_t(2)) +
                        1.5 * Utility::pow<2>(_grad_rot_0_local_t(0)) * Iy_avg +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(0)) +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(1)) +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(0)) * Iz_avg +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(2)) * Iy_avg +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(2)) * Iz_avg;

    k1_large_11 *= 1.0 / 4.0 / Utility::pow<2>(_original_length[0]);

    RankTwoTensor k1_large_21;
    // row 1
    k1_large_21(0, 0) = 0.5 * _grad_disp_0_local_t(0) * _grad_rot_0_local_t(0) * (Ix_avg)-1.0 /
                            3.0 * _grad_disp_0_local_t(1) * _grad_rot_0_local_t(1) * Iz_avg -
                        1.0 / 3.0 * _grad_disp_0_local_t(2) * _grad_rot_0_local_t(2);
    k1_large_21(1, 0) = 1.5 * _grad_disp_0_local_t(0) * _grad_rot_0_local_t(1) * Iz_avg -
                        1.0 / 3.0 * _grad_disp_0_local_t(1) * _grad_rot_0_local_t(0) * Iz_avg;
    k1_large_21(2, 0) = 1.5 * _grad_disp_0_local_t(0) * _grad_rot_0_local_t(2) * Iy_avg -
                        1.0 / 3.0 * _grad_disp_0_local_t(2) * _grad_rot_0_local_t(0) * Iy_avg;

    // row 2
    k1_large_21(0, 1) = k1_large_21(1, 0);
    k1_large_21(1, 1) = 0.5 * _grad_disp_0_local_t(1) * _grad_rot_0_local_t(1) * Iz_avg -
                        1.0 / 3.0 * _grad_disp_0_local_t(0) * _grad_rot_0_local_t(0) * Iz_avg;
    k1_large_21(2, 1) = 0.5 * _grad_disp_0_local_t(1) * _grad_rot_0_local_t(2) * Iy_avg;

    // row 3
    k1_large_21(0, 2) = k1_large_21(2, 0);
    k1_large_21(1, 2) = k1_large_21(2, 1);
    k1_large_21(2, 2) = 0.5 * _grad_disp_0_local_t(2) * _grad_rot_0_local_t(2) * Iy_avg -
                        1.0 / 3.0 * _grad_disp_0_local_t(0) * _grad_rot_0_local_t(0) * Iy_avg;
    k1_large_21 *= 1.0 / 4.0 / Utility::pow<2>(_original_length[0]);

    RankTwoTensor k1_large_22;
    // row 1
    k1_large_22(0, 0) = Utility::pow<2>(_grad_rot_0_local_t(0)) * Utility::pow<2>(Ix_avg) +
                        1.5 * Utility::pow<2>(_grad_disp_0_local_t(1)) * Iz_avg +
                        1.5 * Utility::pow<2>(_grad_disp_0_local_t(2)) * Iy_avg +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(0)) * Ix_avg +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(2)) * Iz_avg +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(1)) * Iy_avg +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(2)) * Iy_avg * Ix_avg +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(1)) * Iz_avg * Ix_avg;
    k1_large_22(1, 0) = 0.5 * _grad_rot_0_local_t(0) * _grad_rot_0_local_t(1) * Iz_avg * Ix_avg -
                        1.0 / 3.0 * _grad_disp_0_local_t(0) * _grad_disp_0_local_t(1) * Iz_avg;
    k1_large_22(2, 0) = 0.5 * _grad_rot_0_local_t(0) * _grad_rot_0_local_t(2) * Iy_avg * Ix_avg -
                        1.0 / 3.0 * _grad_disp_0_local_t(0) * _grad_disp_0_local_t(2) * Iy_avg;

    // row 2
    k1_large_22(0, 1) = k1_large_22(1, 0);
    k1_large_22(1, 1) = Utility::pow<2>(_grad_rot_0_local_t(1)) * Iz_avg * Iz_avg +
                        1.5 * Utility::pow<2>(_grad_disp_0_local_t(0)) * Iz_avg +
                        1.5 * Utility::pow<2>(_grad_rot_0_local_t(2)) * Iy_avg * Iz_avg +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(1)) * Iz_avg +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(2)) * Iz_avg +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(0)) * Iz_avg * Ix_avg;
    k1_large_22(2, 1) = 1.5 * _grad_rot_0_local_t(1) * _grad_rot_0_local_t(2) * Iy_avg * Iz_avg;

    // row 3
    k1_large_22(0, 2) = k1_large_22(2, 0);
    k1_large_22(1, 2) = k1_large_22(2, 1);
    k1_large_22(2, 2) = Utility::pow<2>(_grad_rot_0_local_t(2)) * Iy_avg * Iy_avg +
                        1.5 * Utility::pow<2>(_grad_disp_0_local_t(0)) * Iy_avg +
                        1.5 * Utility::pow<2>(_grad_rot_0_local_t(1)) * Iy_avg * Iz_avg +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(1)) * Iy_avg +
                        0.5 * Utility::pow<2>(_grad_disp_0_local_t(2)) * Iy_avg +
                        0.5 * Utility::pow<2>(_grad_rot_0_local_t(0)) * Iz_avg * Ix_avg;

    k1_large_22 *= 1.0 / 4.0 / Utility::pow<2>(_original_length[0]);

    // k2_large and k3_large are contributions from tau_xy * d(gamma_xy) and tau_xz * d(gamma_xz)
    // k2_large for node 1 is negative of that for node 0
    RankTwoTensor k2_large_11;
    // col 1
    k2_large_11(0, 0) =
        0.25 * Utility::pow<2>(_avg_rot_local_t(2)) + 0.25 * Utility::pow<2>(_avg_rot_local_t(1));
    k2_large_11(1, 0) = -1.0 / 6.0 * _avg_rot_local_t(0) * _avg_rot_local_t(1);
    k2_large_11(2, 0) = -1.0 / 6.0 * _avg_rot_local_t(0) * _avg_rot_local_t(2);

    // col 2
    k2_large_11(0, 1) = k2_large_11(1, 0);
    k2_large_11(1, 1) = 0.25 * _avg_rot_local_t(0);

    // col 3
    k2_large_11(0, 2) = k2_large_11(2, 0);
    k2_large_11(2, 2) = 0.25 * Utility::pow<2>(_avg_rot_local_t(0));

    k2_large_11 *= 1.0 / 4.0 / Utility::pow<2>(_original_length[0]);

    RankTwoTensor k2_large_22;
    // col1
    k2_large_22(0, 0) = 0.25 * Utility::pow<2>(_avg_rot_local_t(0)) * Ix_avg;
    k2_large_22(1, 0) = 1.0 / 6.0 * _avg_rot_local_t(0) * _avg_rot_local_t(1) * Iz_avg;
    k2_large_22(2, 0) = 1.0 / 6.0 * _avg_rot_local_t(0) * _avg_rot_local_t(2) * Iy_avg;

    // col2
    k2_large_22(0, 1) = k2_large_22(1, 0);
    k2_large_22(1, 1) = 0.25 * Utility::pow<2>(_avg_rot_local_t(2)) * Iz_avg +
                        0.25 * Utility::pow<2>(_avg_rot_local_t(1)) * Iz_avg;

    // col3
    k2_large_22(0, 2) = k2_large_22(2, 0);
    k2_large_22(2, 2) = 0.25 * Utility::pow<2>(_avg_rot_local_t(2)) * Iy_avg +
                        0.25 * Utility::pow<2>(_avg_rot_local_t(1)) * Iy_avg;

    k2_large_22 *= 1.0 / 4.0 / Utility::pow<2>(_original_length[0]);

    // k3_large for node 1 is same as that for node 0
    RankTwoTensor k3_large_22;
    // col1
    k3_large_22(0, 0) = 0.25 * Utility::pow<2>(_grad_disp_0_local_t(2)) +
                        0.25 * _grad_rot_0_local_t(0) * Ix_avg +
                        0.25 * Utility::pow<2>(_grad_disp_0_local_t(1));
    k3_large_22(1, 0) = -1.0 / 6.0 * _grad_disp_0_local_t(0) * _grad_disp_0_local_t(1) +
                        1.0 / 6.0 * _grad_rot_0_local_t(0) * _grad_rot_0_local_t(1) * Iz_avg;
    k3_large_22(2, 0) = -1.0 / 6.0 * _grad_disp_0_local_t(0) * _grad_disp_0_local_t(2) +
                        1.0 / 6.0 * _grad_rot_0_local_t(0) * _grad_rot_0_local_t(2) * Iy_avg;

    // col2
    k3_large_22(0, 1) = k3_large_22(1, 0);
    k3_large_22(2, 2) = 0.25 * Utility::pow<2>(_grad_disp_0_local_t(0)) +
                        0.25 * _grad_rot_0_local_t(2) * Iy_avg +
                        0.25 * _grad_rot_0_local_t(1) * Iz_avg;

    // col3
    k3_large_22(0, 2) = k3_large_22(2, 0);
    k3_large_22(2, 2) = 0.25 * Utility::pow<2>(_grad_disp_0_local_t(0)) +
                        0.25 * _grad_rot_0_local_t(2) * Iy_avg +
                        0.25 * _grad_rot_0_local_t(1) * Iz_avg;

    k3_large_22 *= 1.0 / 16.0;

    RankTwoTensor k3_large_21;
    // col1
    k3_large_21(0, 0) = -1.0 / 6.0 *
                        (_grad_disp_0_local_t(2) * _avg_rot_local_t(2) +
                         _grad_disp_0_local_t(1) * _avg_rot_local_t(1));
    k3_large_21(1, 0) = 0.25 * _grad_disp_0_local_t(0) * _avg_rot_local_t(1) -
                        1.0 / 6.0 * _grad_disp_0_local_t(1) * _avg_rot_local_t(0);
    k3_large_21(2, 0) = 0.25 * _grad_disp_0_local_t(0) * _avg_rot_local_t(2) -
                        1.0 / 6.0 * _grad_disp_0_local_t(2) * _avg_rot_local_t(0);

    // col2
    k3_large_21(0, 1) = 0.25 * _grad_disp_0_local_t(1) * _avg_rot_local_t(0) -
                        1.0 / 6.0 * _grad_disp_0_local_t(0) * _avg_rot_local_t(1);
    k3_large_21(1, 1) = -1.0 / 6.0 * _grad_disp_0_local_t(0) * _avg_rot_local_t(0);

    // col3
    k3_large_21(0, 2) = 0.25 * _grad_disp_0_local_t(2) * _avg_rot_local_t(0) -
                        1.0 / 6.0 * _grad_disp_0_local_t(0) * _avg_rot_local_t(2);
    k3_large_21(2, 2) = -1.0 / 6.0 * _grad_disp_0_local_t(0) * _avg_rot_local_t(0);

    k3_large_21 *= 1.0 / 8.0 / _original_length[0];

    RankTwoTensor k4_large_22;
    // col 1
    k4_large_22(0, 0) = 0.25 * _grad_rot_0_local_t(0) * _avg_rot_local_t(0) * Ix_avg +
                        1.0 / 6.0 * _grad_rot_0_local_t(2) * _avg_rot_local_t(2) * Iy_avg +
                        1.0 / 6.0 * _grad_rot_0_local_t(1) * _avg_rot_local_t(1) * Iz_avg;
    k4_large_22(1, 0) = 1.0 / 6.0 * _grad_rot_0_local_t(1) * _avg_rot_local_t(0) * Iz_avg;
    k4_large_22(2, 0) = 1.0 / 6.0 * _grad_rot_0_local_t(2) * _avg_rot_local_t(0) * Iy_avg;

    // col2
    k4_large_22(0, 1) = 1.0 / 6.0 * _grad_rot_0_local_t(0) * _avg_rot_local_t(1) * Iz_avg;
    k4_large_22(1, 1) = 0.25 * _grad_rot_0_local_t(1) * _avg_rot_local_t(1) * Iz_avg +
                        1.0 / 6.0 * _grad_rot_0_local_t(0) * _avg_rot_local_t(0) * Iz_avg;
    k4_large_22(2, 1) = 0.25 * _grad_rot_0_local_t(1) * _avg_rot_local_t(2) * Iz_avg;

    // col 3
    k4_large_22(0, 2) = 1.0 / 6.0 * _grad_rot_0_local_t(0) * _avg_rot_local_t(2) * Iy_avg;
    k4_large_22(1, 2) = 0.25 * _grad_rot_0_local_t(2) * _avg_rot_local_t(1) * Iy_avg;
    k4_large_22(2, 2) = 0.25 * _grad_rot_0_local_t(2) * _avg_rot_local_t(2) * Iy_avg +
                        1.0 / 6.0 * _grad_rot_0_local_t(0) * _avg_rot_local_t(0) * Iy_avg;

    k3_large_22 += 1.0 / 8.0 / _original_length[0] * (k4_large_22 + k4_large_22.transpose());

    // Assembling final matrix
    _K11[0] += _total_rotation[0].transpose() * (k1_large_11 + k2_large_11) * _total_rotation[0];
    _K22[0] += _total_rotation[0].transpose() * (k1_large_22 + k2_large_22 + k3_large_22) *
               _total_rotation[0];
    _K21[0] += _total_rotation[0].transpose() * (k1_large_21 + k3_large_21) * _total_rotation[0];
    _K21_cross[0] +=
        _total_rotation[0].transpose() * (-k1_large_21 + k3_large_21) * _total_rotation[0];
    _K22_cross[0] += _total_rotation[0].transpose() * (-k1_large_22 - k2_large_22 + k3_large_22) *
                     _total_rotation[0];
  }
}

void
LayeredBeam::computeRotation()
{
  _total_rotation[0] = _original_local_config;
}

void LayeredBeam::computeQpStress()
{
  std::cout << "\n\n\n***************************\n";
  std::cout << "*** computeQpStress() ***";
  std::cout << "\n***************************\n";

  std::cout << "For QP = (" << (_q_point[_qp])(0) << ", ";
  std::cout << (_q_point[_qp])(1) << ", " << (_q_point[_qp])(2) << "):\n";

  Real strain_increment = _total_stretch[_qp];
  Real zmidl = -0.5 * _depth;

  // std::cout<<"zmidl = "<<zmidl<<std::endl;

  Real thick = _depth/_nlayers;                        //thickness of each layer

  // std::cout<<"thickness = "<<thick<<std::endl;


  Real moment = 0.0;

  for (unsigned int i = 0; i < _nlayers; ++i)
  {
    // std::cout<<"integration layer = "<<i<<std::endl;

    zmidl += thick/2.0;

    Real trial_stress = (*_direct_stress_old[i])[_qp] + _material_flexure[_qp](2) * strain_increment * zmidl;

    //
    std::cout<<"direct stress old "<<_qp<<i<<" = "<<(*_direct_stress_old[i])[_qp]<<std::endl;
    std::cout<<"trial stress = "<<trial_stress<<std::endl;
    //

    (*_hardening_variable[i])[_qp] = (*_hardening_variable_old[i])[_qp];
    (*_plastic_strain[i])[_qp] = (*_plastic_strain_old[i])[_qp];

    Real yield_condition = std::abs(trial_stress) - (*_hardening_variable[i])[_qp] - _yield_stress;
    Real iteration = 0;
    Real plastic_strain_increment = 0.0;
    Real elastic_strain_increment = strain_increment * zmidl;

    //
    std::cout<<"yield condition = "<<yield_condition<<std::endl;
    //

    if (yield_condition > 0.0)
    {
      Real residual = std::abs(trial_stress) - (*_hardening_variable[i])[_qp] - _yield_stress -
                    _material_flexure[_qp](2) * plastic_strain_increment;

      Real reference_residual =
          std::abs(trial_stress) - _material_flexure[_qp](2) * plastic_strain_increment;

      while (std::abs(residual) > _absolute_tolerance ||
             std::abs(residual / reference_residual) > _relative_tolerance)
      {
        (*_hardening_variable[i])[_qp] = computeHardeningValue(plastic_strain_increment,i);
        Real hardening_slope = computeHardeningDerivative(plastic_strain_increment,i);

        Real scalar = (std::abs(trial_stress) - (*_hardening_variable[i])[_qp] - _yield_stress -
                     _material_flexure[_qp](2) * plastic_strain_increment) /
                    (_material_flexure[_qp](2) + hardening_slope);

        plastic_strain_increment += scalar;

        residual = std::abs(trial_stress) - (*_hardening_variable[i])[_qp] - _yield_stress -
                 _material_flexure[_qp](2) * plastic_strain_increment;

        reference_residual = std::abs(trial_stress) - _material_flexure[_qp](2) * plastic_strain_increment;

        ++iteration;
        if (iteration > _max_its) // not converging
          throw MooseException("LayeredBeam: Plasticity model did not converge");
      }
      plastic_strain_increment *= MathUtils::sign(trial_stress);

      std::cout<<"plastic strain inc = "<<plastic_strain_increment<<std::endl;

      (*_plastic_strain[i])[_qp] += plastic_strain_increment;
      elastic_strain_increment = strain_increment * zmidl - plastic_strain_increment;

      std::cout<<"elastic strain_increment = "<<elastic_strain_increment<<std::endl;

    }
    (*_direct_stress[i])[_qp] = (*_direct_stress_old[i])[_qp] + elastic_strain_increment * _material_flexure[_qp](2);

    std::cout<<"new direct stress "<<_qp<<i<<" = "<<(*_direct_stress[i])[_qp]<<std::endl;

    moment += (*_direct_stress[i])[_qp] * _width * zmidl * thick;
    _stres[_qp] = moment;
    std::cout<<"moment = "<<_stres[_qp]<<std::endl<<std::endl;

    zmidl += thick/2.0;
  }
  _stres[_qp] = _stres[_qp];
  std::cout<<" final moment = "<<_stres[_qp]<<std::endl;
}

Real
LayeredBeam::computeHardeningValue(Real scalar, Real j)
{
  if (_hardening_function)
  {
    const Real strain_old = (*_plastic_strain_old[j])[_qp];
    const Point p;

    return _hardening_function->value(std::abs(strain_old) + scalar, p) - _yield_stress;
  }

  return (*_hardening_variable_old[j])[_qp] + _hardening_constant * scalar;
}

Real LayeredBeam::computeHardeningDerivative(Real scalar, Real j)
{
  if (_hardening_function)
  {
    const Real strain_old = (*_plastic_strain_old[j])[_qp];
    const Point p;

    return _hardening_function->timeDerivative(std::abs(strain_old), p);
  }

  return _hardening_constant;
}
