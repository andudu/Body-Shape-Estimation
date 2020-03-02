#pragma once
// Class incapsulates the process of the shape estimation under clothing
// Uses libigl for visualization
// Uses smart pointers from C++11 standard

// need to include first, because it uses Windows.h
#include "CustomLogger.h"

#include <iostream>
#include <string>
#include <memory>

#include <igl/opengl/glfw/Viewer.h>
#include <igl/opengl/glfw/imgui/ImGuiMenu.h>
#include <igl/point_mesh_squared_distance.h>

#include <GeneralMesh/GeneralMesh.h>
#include <Photographer/Photographer.h>
#include "SMPLWrapper.h"
#include "ShapeUnderClothOptimizer.h"
#include "OpenPoseWrapper.h"

using VertsVector = std::vector<Eigen::MatrixXd>;

class PoseShapeExtractor
{
public:
    enum InitializationType
    {
        NO_INITIALIZATION, 
        OPENPOSE, 
        FILE
    };

    PoseShapeExtractor(const std::string& smpl_model_path, const std::string& logging_path = "");
    ~PoseShapeExtractor();

    // pass path to openpose model or to smpl parameters file in accordance with the type
    void setupInitialization(InitializationType type, const std::string path = "");

    void setupNewExperiment(std::shared_ptr<GeneralMesh> input, const std::string experiment_name = "");
    void setupNewDistplacementRegExperiment(std::shared_ptr<GeneralMesh> input, 
        double l2_weight, double smoothing_weight, const std::string experiment_name = "");
    void setupNewShapePruningExperiment(std::shared_ptr<GeneralMesh> input,
        double threshold, const std::string experiment_name = "");
    void setupNewInnerVertsParamsExperiment(std::shared_ptr<GeneralMesh> input,
        double inner_weight, double gm_saturation, const std::string experiment_name = "");
    void setupNewPoseRegExperiment(std::shared_ptr<GeneralMesh> input,
        double weight, const std::string experiment_name = "");
    void setupNewCameraExperiment(std::shared_ptr<GeneralMesh> input, 
        double distance, int n_cameras, double elevation, const std::string experiment_name = "");

    std::shared_ptr<SMPLWrapper> getEstimatedModel() { return smpl_; }
    std::shared_ptr<SMPLWrapper> runExtraction();

    void setSaveIntermediateResults(bool save) { save_iteration_results_ = collect_iteration_results_ = save; };
    void setCollectIntermediateResults(bool collect);

    void viewCameraSetupForPhotos();
    void viewFinalResult(bool withOpenPoseKeypoints = false);
    void viewIteratoinProcess();

private:
    // returns number of pictures taken
    void photoSetUp_(Photographer& photographer);
    void takePhotos_();
    
    void estimateInitialPoseWithOP_();
    void runPoseShapeOptimization_();

    char convertInputGenderToChar_(const GeneralMesh& input);

    // visulization
    static bool visualizeIterationPreDraw_(igl::opengl::glfw::Viewer & viewer);
    static bool visualizeIterationKeyDown_(igl::opengl::glfw::Viewer& viewer, unsigned char key, int modifier);

    // state
    std::shared_ptr<GeneralMesh> input_;
    std::shared_ptr<SMPLWrapper> smpl_;
    const std::string smpl_model_path_;
    InitializationType initialization_type_;
    std::string smpl_file_initilization_path_;

    // tools
    std::shared_ptr<OpenPoseWrapper> openpose_;
    std::string openpose_model_path_;
    std::shared_ptr<ShapeUnderClothOptimizer> optimizer_;
    std::shared_ptr<CustomLogger> logger_;
    const std::string logging_base_path_;

    // for photographer
    double cameras_distance_;
    double cameras_elevation_;
    int num_cameras_;

    // for optimizer
    double optimizer_shape_reg_weight_;
    double optimizer_shape_prune_threshold_;
    double optimizer_pose_reg_weight_;
    double optimizer_displacement_reg_weight_;
    double optimizer_displacement_smoothing_weight_;
    double optimizer_gm_saturation_;
    double optimizer_in_verts_scaling_;

    // for visulaization
    VertsVector iteration_outputs_;
    bool save_iteration_results_ = false;
    bool collect_iteration_results_ = false;

    static int iteration_viewer_counter_;
    static VertsVector* iteration_outputs_to_viz_;
    static std::shared_ptr <SMPLWrapper> smpl_to_viz_;
    static std::shared_ptr <GeneralMesh> input_to_viz_;
};

