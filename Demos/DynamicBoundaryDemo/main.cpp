#include "SPlisHSPlasH/Common.h"
#include "GL/glew.h"
#include "Visualization/MiniGL.h"
#include "GL/glut.h"
#include "SPlisHSPlasH/TimeManager.h"
#include <Eigen/Dense>
#include <iostream>
#include "SPlisHSPlasH/Utilities/Timing.h"
#include "Utilities/PartioReaderWriter.h"
#include "PositionBasedDynamicsWrapper/PBDRigidBody.h"
#include "Utilities/OBJLoader.h"
#include "SPlisHSPlasH/Utilities/PoissonDiskSampling.h"
#include "PositionBasedDynamicsWrapper/PBDWrapper.h"
#include "Demos/Common/DemoBase.h"
#include "Utilities/FileSystem.h"

// Enable memory leak detection
#ifdef _DEBUG
#ifndef EIGEN_ALIGN
#define new DEBUG_NEW
#endif
#endif

using namespace SPH;
using namespace Eigen;
using namespace std;

void timeStep();
void initBoundaryData();
void render();
void renderBoundary();
void reset();
void updateBoundaryParticles(const bool forceUpdate);
void updateBoundaryForces();
void simulationMethodChanged();

DemoBase   base;
PBDWrapper pbdWrapper;


// main
int main(int argc, char** argv)
{
    REPORT_MEMORY_LEAKS;

    base.init(argc, argv, "DynamicBoundaryDemo");

    //////////////////////////////////////////////////////////////////////////
    // PBD
    //////////////////////////////////////////////////////////////////////////
    pbdWrapper.initShader();
    pbdWrapper.readScene(base.getSceneFile());

    initBoundaryData();
    base.buildModel();
    base.setSimulationMethodChangedFct(simulationMethodChanged);
    pbdWrapper.initGUI();

    pbdWrapper.initModel(TimeManager::getCurrent()->getTimeStepSize());

    MiniGL::setClientIdleFunc(50, timeStep);
    MiniGL::setKeyFunc(0, 'r', reset);
    MiniGL::setClientSceneFunc(render);

    glutMainLoop();

    base.cleanup();

    Timing::printAverageTimes();
    Timing::printTimeSums();

    return 0;
}

void reset()
{
    Timing::printAverageTimes();
    Timing::reset();

    //////////////////////////////////////////////////////////////////////////
    // PBD
    //////////////////////////////////////////////////////////////////////////
    pbdWrapper.reset();

    updateBoundaryParticles(true);

    base.getSimulationMethod().simulation->reset();
    TimeManager::getCurrent()->setTime(0.0);
}



void timeStep()
{
    if((base.getPauseAt() > 0.0) && (base.getPauseAt() < TimeManager::getCurrent()->getTime()))
        base.setPause(true);

    if(base.getPause())
        return;

    // Simulation code
    for(unsigned int i = 0; i < base.getNumberOfStepsPerRenderUpdate(); i++)
    {
        START_TIMING("SimStep");
        base.getSimulationMethod().simulation->step();
        STOP_TIMING_AVG;

        updateBoundaryForces();

        //////////////////////////////////////////////////////////////////////////
        // PBD
        //////////////////////////////////////////////////////////////////////////
        START_TIMING("SimStep - PBD");
        pbdWrapper.timeStep();
        STOP_TIMING_AVG;

        updateBoundaryParticles(false);

        int savedFluidFrame = base.getSimulationMethod().model.writeFrameFluidData(TimeManager::getCurrent()->getTime());


        static std::vector<Vector3r> vertices;
        static std::vector<Vector3r> normals;

        if(savedFluidFrame > 0)
        {
            auto scene = base.getScene();
            PBD::SimulationModel&                  model = pbdWrapper.getSimulationModel();
            PBD::SimulationModel::RigidBodyVector& rb    = model.getRigidBodies();
            base.m_MeshWriter->reset_buffer();
            base.m_MeshWriter->getBuffer().push_back(static_cast<unsigned int>(rb.size() - 1));

            for(size_t i = 0; i < rb.size(); i++)
            {
                if(!scene.boundaryModels[i]->isWall)
                {
                    auto vertexData = rb[i]->getGeometry().getVertexData();
                    auto mesh       = rb[i]->getGeometry().getMesh();

                    auto nFaces            = mesh.numFaces();
                    auto faces             = mesh.getFaces();
                    auto faceVertices      = vertexData.getVertices();
                    auto faceVertexNormals = mesh.getVertexNormals();

                    vertices.resize(0);
                    vertices.reserve(nFaces * 3);
                    normals.resize(0);
                    normals.reserve(nFaces * 3);

                    for(unsigned int i = 0; i < nFaces; ++i)
                    {
                        for(unsigned int j = 0; j < 3; ++j)
                        {
                            unsigned int v_index = faces[i * 3 + j];
                            vertices.push_back((*faceVertices)[v_index]);
                            normals.push_back(faceVertexNormals[v_index]);
                        }
                    }

                    base.m_MeshWriter->getBuffer().push_back(static_cast<unsigned int>(vertices.size()));
                    base.m_MeshWriter->getBuffer().push_back_to_float_array(vertices, false);
                    base.m_MeshWriter->getBuffer().push_back_to_float_array(normals, false);
                }
            }

            base.m_MeshWriter->flush_buffer_async(savedFluidFrame);
        }
    }
}

void simulationMethodChanged()
{
    pbdWrapper.initGUI();
}

void renderBoundary()
{
    DemoBase::SimulationMethod& simulationMethod = base.getSimulationMethod();
    Shader&                     shader           = base.getShader();
    Shader&                     meshShader       = base.getMeshShader();
    SceneLoader::Scene&         scene            = base.getScene();
    const int                   renderWalls      = base.getRenderWalls();

    float                       wallColor[4] = { 0.1f, 0.6f, 0.6f, 1.0f };
    if((renderWalls == 1) || (renderWalls == 2))
    {
        if(MiniGL::checkOpenGLVersion(3, 3))
        {
            shader.begin();
            glUniform3fv(shader.getUniform("color"), 1, &wallColor[0]);
            glEnableVertexAttribArray(0);
            for(int body = simulationMethod.model.numberOfRigidBodyParticleObjects() - 1; body >= 0; body--)
            {
                if((renderWalls == 1) || (!scene.boundaryModels[body]->isWall))
                {
                    FluidModel::RigidBodyParticleObject* rb = simulationMethod.model.getRigidBodyParticleObject(body);
                    glVertexAttribPointer(0, 3, GL_DOUBLE, GL_FALSE, 0, &simulationMethod.model.getPosition(body + 1, 0));
                    glDrawArrays(GL_POINTS, 0, rb->numberOfParticles());
                }
            }
            glDisableVertexAttribArray(0);
            shader.end();
        }
        else
        {
            glDisable(GL_LIGHTING);
            glPointSize(4.0f);

            glBegin(GL_POINTS);
            for(int body = simulationMethod.model.numberOfRigidBodyParticleObjects() - 1; body >= 0; body--)
            {
                if((renderWalls == 1) || (!scene.boundaryModels[body]->isWall))
                {
                    FluidModel::RigidBodyParticleObject* rb = simulationMethod.model.getRigidBodyParticleObject(body);
                    for(unsigned int i = 0; i < rb->numberOfParticles(); i++)
                    {
                        glColor3fv(wallColor);
                        glVertex3v(&simulationMethod.model.getPosition(body + 1, i)[0]);
                    }
                }
            }
            glEnd();
            glEnable(GL_LIGHTING);
        }
    }
}


void render()
{
    MiniGL::coordinateSystem();

    base.renderFluid();
    renderBoundary();

    //////////////////////////////////////////////////////////////////////////
    // PBD
    //////////////////////////////////////////////////////////////////////////

    PBD::SimulationModel&                  model = pbdWrapper.getSimulationModel();
    PBD::SimulationModel::RigidBodyVector& rb    = model.getRigidBodies();

    const int                              renderWalls = base.getRenderWalls();
    SceneLoader::Scene&                    scene       = base.getScene();
    if((renderWalls == 3) || (renderWalls == 4))
    {
        for(size_t i = 0; i < rb.size(); i++)
        {
            const PBD::VertexData&      vd   = rb[i]->getGeometry().getVertexData();
            const PBD::IndexedFaceMesh& mesh = rb[i]->getGeometry().getMesh();
            if((renderWalls == 3) || (!scene.boundaryModels[i]->isWall))
            {
                float* col = &scene.boundaryModels[i]->color[0];
                if(!scene.boundaryModels[i]->isWall)
                {
                    base.meshShaderBegin(col);
                    pbdWrapper.drawMesh(vd, mesh, 0, col);
                    base.meshShaderEnd();
                }
                else
                {
                    base.meshShaderBegin(col);
                    pbdWrapper.drawMesh(vd, mesh, 0, col);
                    base.meshShaderEnd();
                }
            }
        }
    }

    pbdWrapper.renderTriangleModels();
    pbdWrapper.renderTetModels();
    pbdWrapper.renderConstraints();
    pbdWrapper.renderBVH();
}

void initBoundaryData()
{
    std::string         base_path = FileSystem::getFilePath(base.getSceneFile());
    SceneLoader::Scene& scene     = base.getScene();
    const bool          useCache  = base.getUseParticleCaching();

    for(unsigned int i = 0; i < scene.boundaryModels.size(); i++)
    {
        std::vector<Vector3r> boundaryParticles;
        if(scene.boundaryModels[i]->samplesFile != "")
        {
            string particleFileName = base_path + "/" + scene.boundaryModels[i]->samplesFile;
            PartioReaderWriter::readParticles(particleFileName, Vector3r::Zero(), Matrix3r::Identity(), scene.boundaryModels[i]->scale[0], boundaryParticles);
        }

        // Cache sampling
        std::string                            mesh_base_path   = FileSystem::getFilePath(scene.boundaryModels[i]->meshFile);
        std::string                            mesh_file_name   = FileSystem::getFileName(scene.boundaryModels[i]->meshFile);
        std::string                            scene_path       = FileSystem::getFilePath(base.getSceneFile());
        std::string                            scene_file_name  = FileSystem::getFileName(base.getSceneFile());
        string                                 cachePath        = scene_path + "/" + mesh_base_path + "/Cache";
        string                                 particleFileName = FileSystem::normalizePath(cachePath + "/" + scene_file_name + "_" + mesh_file_name + "_" + std::to_string(i) + ".bgeo");

        PBD::SimulationModel&                  model       = pbdWrapper.getSimulationModel();
        PBD::SimulationModel::RigidBodyVector& rigidBodies = model.getRigidBodies();
        PBDRigidBody*                          rb          = new PBDRigidBody(rigidBodies[i]);
        PBD::RigidBodyGeometry&                geo         = rigidBodies[i]->getGeometry();
        PBD::IndexedFaceMesh&                  mesh        = geo.getMesh();
        PBD::VertexData&                       vd          = geo.getVertexData();

        if(scene.boundaryModels[i]->samplesFile == "")
        {
            bool foundCacheFile = false;
            if(useCache)
            {
                foundCacheFile = PartioReaderWriter::readParticles(particleFileName, Vector3r::Zero(), Matrix3r::Identity(), 1.0, boundaryParticles);
                if(foundCacheFile)
                    std::cout << "Loaded cached boundary sampling: " << particleFileName << "\n";
            }

            if(!useCache || !foundCacheFile)
            {
                std::cout << "Surface sampling of " << scene.boundaryModels[i]->meshFile << "\n";
                START_TIMING("Poisson disk sampling");
                PoissonDiskSampling sampling;
                sampling.sampleMesh(mesh.numVertices(), &vd.getPosition(0), mesh.numFaces(), mesh.getFaces().data(), scene.particleRadius, 10, 1, boundaryParticles);
                STOP_TIMING_AVG;

                // Cache sampling
                if(useCache && (FileSystem::makeDir(cachePath) == 0))
                {
                    std::cout << "Save particle sampling: " << particleFileName << "\n";
                    PartioReaderWriter::writeParticles(particleFileName, (unsigned int)boundaryParticles.size(), boundaryParticles.data(), NULL, scene.particleRadius);
                }
            }
            // transform back to local coordinates
            for(unsigned int j = 0; j < boundaryParticles.size(); j++)
                boundaryParticles[j] = rb->getRotation().transpose() * (boundaryParticles[j] - rb->getPosition());
        }
        base.getSimulationMethod().model.addRigidBodyObject(rb, static_cast<unsigned int>(boundaryParticles.size()), &boundaryParticles[0]);
    }
    updateBoundaryParticles(true);
}

void updateBoundaryParticles(const bool forceUpdate = false)
{
    SceneLoader::Scene& scene    = base.getScene();
    const unsigned int  nObjects = base.getSimulationMethod().model.numberOfRigidBodyParticleObjects();
    for(unsigned int i = 0; i < nObjects; i++)
    {
        FluidModel::RigidBodyParticleObject* rbpo = base.getSimulationMethod().model.getRigidBodyParticleObject(i);
        RigidBodyObject*                     rbo  = rbpo->m_rigidBody;
        if(rbo->isDynamic() || forceUpdate)
        {
#pragma omp parallel default(shared)
            {
#pragma omp for schedule(static)
                for(int j = 0; j < (int)rbpo->numberOfParticles(); j++)
                {
                    rbpo->m_x[j] = rbo->getRotation() * rbpo->m_x0[j] + rbo->getPosition();
                    rbpo->m_v[j] = rbo->getAngularVelocity().cross(rbpo->m_x[j] - rbo->getPosition()) + rbo->getVelocity();
                }
            }
        }
    }
}

void updateBoundaryForces()
{
    Real                h        = TimeManager::getCurrent()->getTimeStepSize();
    SceneLoader::Scene& scene    = base.getScene();
    const unsigned int  nObjects = base.getSimulationMethod().model.numberOfRigidBodyParticleObjects();
    for(unsigned int i = 0; i < nObjects; i++)
    {
        FluidModel::RigidBodyParticleObject* rbpo = base.getSimulationMethod().model.getRigidBodyParticleObject(i);
        RigidBodyObject*                     rbo  = rbpo->m_rigidBody;
        if(rbo->isDynamic())
        {
            ((PBDRigidBody*)rbo)->updateTimeStepSize();
            Vector3r force, torque;
            force.setZero();
            torque.setZero();

            for(int j = 0; j < (int)rbpo->numberOfParticles(); j++)
            {
                force  += rbpo->m_f[j];
                torque += (rbpo->m_x[j] - rbo->getPosition()).cross(rbpo->m_f[j]);
                rbpo->m_f[j].setZero();
            }
            rbo->addForce(force);
            rbo->addTorque(torque);
        }
    }
}
