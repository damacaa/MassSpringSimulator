#include "pch.h"
#include "PhysicsManager.h"
#include "Object.h"


PhysicsManager::~PhysicsManager()
{
	for (int i = 0; i < SimObjects.size(); i++)
	{
		delete SimObjects[i];
	}
	SimObjects.clear();

	for (int i = 0; i < Fixers.size(); i++)
	{
		delete Fixers[i];
	}
	Fixers.clear();
}

int PhysicsManager::AddObject(Vector3f position, Vector3f* vertices, int nVertices, int* triangles, int nTriangles, float stiffness, float mass)
{
	Object* o = new Object(position, vertices, nVertices, triangles, nTriangles, stiffness, mass);
	o->id = (int)SimObjects.size();
	SimObjects.push_back(o);

	for (size_t i = 0; i < Fixers.size(); i++)
	{
		o->FixnodeArray(Fixers[i]);
	}

	Start();

	return o->id;
}

void PhysicsManager::AddFixer(Vector3f position, Vector3f scale)
{
	Fixer* f = new Fixer(position, scale);
	Fixers.push_back(f);

	for (size_t i = 0; i < SimObjects.size(); i++)
	{
		SimObjects[i]->FixnodeArray(f);
	}
}

void PhysicsManager::Start()
{
	//Parse the simulable objects and initialize their state indices
	m_numDoFs = 0;
	//m_objs = new List<ISimulable>(SimObjects.Capacity);

	for (size_t i = 0; i < SimObjects.size(); i++)
	{
		// Initialize simulable object
		SimObjects[i]->Initialize(&m_numDoFs);

		// Retrieve pos and vel size
		m_numDoFs += SimObjects[i]->GetNumDoFs();
	}

	initialized = false;
	_v = Eigen::VectorXd(m_numDoFs);//Velocities
}

void PhysicsManager::Update(float time, float h)
{
	Updated = false;

	if (Paused)
		return; // Not simulating

	// Select integration method
	switch (IntegrationMethod)
	{
	case PhysicsManager::Integration::Explicit:
		break;
	case PhysicsManager::Integration::Symplectic:
		StepSymplecticSparse(time, h);
		break;
	case PhysicsManager::Integration::Implicit:
		StepImplicit(time, h);
		break;
	default:
		break;
	}

	Updated = true;
}

void PhysicsManager::StepSymplecticOld(float time, float h) {
	for (int i = 0; i < SimObjects.size(); i++)
	{
		SimObjects[i]->Update(time, h);
	}
}

void PhysicsManager::StepSymplecticDense(float time, float h) {
	Eigen::VectorXd x = Eigen::VectorXd(m_numDoFs);
	Eigen::VectorXd v = Eigen::VectorXd(m_numDoFs);
	Eigen::VectorXd f = Eigen::VectorXd::Constant(m_numDoFs, 0.0);
	//Eigen::MatrixXd Minv = Eigen::MatrixXd::Constant(m_numDoFs, m_numDoFs, 0.0);
	Eigen::MatrixXd Minv = Eigen::MatrixXd::Zero(m_numDoFs, m_numDoFs);

	for (int i = 0; i < SimObjects.size(); i++)
	{
		SimObjects[i]->GetPosition(&x);
		SimObjects[i]->GetVelocity(&v);
		SimObjects[i]->GetForce(&f);
		SimObjects[i]->GetMassInverse(&Minv);
	}

	for (int i = 0; i < SimObjects.size(); i++)
	{
		SimObjects[i]->FixVector(&f);
		SimObjects[i]->FixMatrix(&Minv);
	}

	//v += h * (Minv * f);
	v = (v * 0.99) + h * (Minv * f);
	x += h * v;
	//x = Eigen::VectorXd::Zero(m_numDoFs);

	for (int i = 0; i < SimObjects.size(); i++)
	{
		SimObjects[i]->SetPosition(&x);
		SimObjects[i]->SetVelocity(&v);
	}
}

void PhysicsManager::StepSymplecticSparse(float time, float h)
{
	Eigen::VectorXd x = Eigen::VectorXd(m_numDoFs);
	Eigen::VectorXd v = Eigen::VectorXd(m_numDoFs);
	Eigen::VectorXd f = Eigen::VectorXd::Constant(m_numDoFs, 0.0);

	SpMat M(m_numDoFs, m_numDoFs);
	SpMat Minv(m_numDoFs, m_numDoFs);

	SpMat dFdx(m_numDoFs, m_numDoFs);
	SpMat dFdv(m_numDoFs, m_numDoFs);

	std::vector<T> masses = std::vector<T>();
	std::vector<T> massesInv = std::vector<T>();
	std::vector<T> derivPos = std::vector<T>();
	std::vector<T> derivVel = std::vector<T>();

	for (int i = 0; i < SimObjects.size(); i++)
	{
		SimObjects[i]->GetPosition(&x);
		SimObjects[i]->GetVelocity(&v);
		SimObjects[i]->GetForce(&f);

		SimObjects[i]->GetForceJacobian(&derivPos, &derivVel);
		SimObjects[i]->GetMassInverse(&massesInv);
		SimObjects[i]->GetMass(&masses);
	}

	std::vector<bool> fixedIndices(m_numDoFs); // m_numDoFs/3?
	for (int i = 0; i < SimObjects.size(); i++)
	{
		SimObjects[i]->GetFixedIndices(&fixedIndices);
	}

	for (size_t i = 0; i < massesInv.size(); i += 3)
	{
		int id = massesInv[i].col() / 3;
		if (fixedIndices[id]) {
			massesInv[i] = T(massesInv[i].col(), massesInv[i].row(), massesInv[i].value());
		}
	}

	//Matriz a triplets, fixing y luego vuelve

	Minv.setFromTriplets(massesInv.begin(), massesInv.end());
	v += h * (Minv * f);

	for (size_t i = 0; i < fixedIndices.size(); i += 3)
	{
		if (fixedIndices[i]) {
			v[i] = 0;
			v[i + 1] = 0;
			v[i + 2] = 0;
		}
	}

	x += h * v;

	for (int i = 0; i < SimObjects.size(); i++)
	{
		SimObjects[i]->SetPosition(&x);
		SimObjects[i]->SetVelocity(&v);
	}
}

void PhysicsManager::StepImplicit(float time, float h)
{
	Eigen::VectorXd x = Eigen::VectorXd(m_numDoFs);//Positions
	Eigen::VectorXd v = Eigen::VectorXd(m_numDoFs);//Velocities
	Eigen::VectorXd f = Eigen::VectorXd::Constant(m_numDoFs, 0.0);//Forces

	SpMat M(m_numDoFs, m_numDoFs);//Masses
	SpMat dFdx(m_numDoFs, m_numDoFs);
	SpMat dFdv(m_numDoFs, m_numDoFs);

	std::vector<T> masses = std::vector<T>();
	std::vector<T> derivPos = std::vector<T>();
	std::vector<T> derivVel = std::vector<T>();

	SpMat Minv(m_numDoFs, m_numDoFs);//Masses
	std::vector<T> massesInv = std::vector<T>();

	for (int i = 0; i < SimObjects.size(); i++)
	{
		SimObjects[i]->GetPosition(&x);
		SimObjects[i]->GetVelocity(&v);

		SimObjects[i]->GetForce(&f);
		SimObjects[i]->GetForceJacobian(&derivPos, &derivVel);
		SimObjects[i]->GetMass(&masses);

		SimObjects[i]->GetMassInverse(&massesInv);
	}


	std::vector<bool> fixedIndices(m_numDoFs);
	for (int i = 0; i < SimObjects.size(); i++)
		SimObjects[i]->GetFixedIndices(&fixedIndices);

	/*for (size_t i = 0; i < fixedIndices.size(); i++)
	{
		for (size_t x = 0; x < derivPos.size(); x++)
		{
			if (i == derivPos[x].col() / 3) {

				if (derivPos[x].col() == derivPos[x].row())
					derivPos[x] = T(derivPos[x].row(), derivPos[x].col(), 1);
				else
					derivPos[x] = T(derivPos[x].row(), derivPos[x].col(), 0);

			}
		}
	}*/

	//For future reference
	//https://stackoverflow.com/questions/45301305/set-sparsity-pattern-of-eigensparsematrix-without-memory-overhead
	M.setFromTriplets(masses.begin(), masses.end());
	dFdx.setFromTriplets(derivPos.begin(), derivPos.end(), [](const double& a, const double& b) { return a + b; });
	dFdv.setFromTriplets(derivVel.begin(), derivVel.end(), [](const double& a, const double& b) { return a + b; });


	/*for (size_t i = 0; i < derivPos.size(); i++)
	{
		//fila y columna coinciden y fijo = 1
		//fila y columna no coinciden y fila O columna fijo = 0
		int nodeIndex = derivPos[i].col() / 3;

		if (fixedIndices[nodeIndex]) {
			if (derivPos[i].col() == derivPos[i].row())
				derivPos[i] = T(derivPos[i].row(), derivPos[i].col(), 1);
			else
				derivPos[i] = T(derivPos[i].row(), derivPos[i].col(), 0);
		}
	}*/

	SpMat A = M - h * dFdv - h * h * dFdx;
	Eigen::VectorXd b = (M - h * dFdv) * v + h * f;

	//A.Solve(b, v);
	//Eigen::SimplicialCholesky<SpMat> chol(A);
	//v = chol.solve(b);

	Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::UnitLower | Eigen::UnitUpper> cg;
	//cg.compute(A);
	cg.factorize(A);

	if (initialized) {
		//Minv.setFromTriplets(massesInv.begin(), massesInv.end());
		//_v = _v + h * (Minv * f);
		v = cg.solveWithGuess(b, _v);
	}
	else {
		v = cg.solve(b);
		initialized = true;
	}
	_v = v;

	//v = cg.solve(b);

	for (size_t i = 0; i < m_numDoFs; i += 3)
	{
		if (fixedIndices[i]) {
			v[i] = 0;
			v[i + 1] = 0;
			v[i + 2] = 0;
		}
	}

	x += h * v;

	for (int i = 0; i < SimObjects.size(); i++)
	{
		SimObjects[i]->SetPosition(&x);
		SimObjects[i]->SetVelocity(&v);
	}
}

Vector3f* PhysicsManager::GetVertices(int id, int* count)
{
	if (id >= SimObjects.size())
		return new Vector3f();

	*count = SimObjects[id]->nVertices;

	return SimObjects[id]->GetVertices();
}
