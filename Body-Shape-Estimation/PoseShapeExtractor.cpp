#include "PoseShapeExtractor.h"

// init statics
int PoseShapeExtractor::iteration_viewer_counter_;
VertsVector* PoseShapeExtractor::iteration_outputs_to_viz_;
std::shared_ptr <SMPLWrapper> PoseShapeExtractor::smpl_to_viz_;
std::shared_ptr <GeneralMesh> PoseShapeExtractor::input_to_viz_;

PoseShapeExtractor::PoseShapeExtractor(const std::string& smpl_model_path, const std::string& logging_path)
    : smpl_model_path_(smpl_model_path), logging_base_path_(logging_path), optimizer_config_()
{
    smpl_ = nullptr;
    input_ = nullptr;
    logger_ = nullptr;
    openpose_ = nullptr;
    initialization_type_ = NO_INITIALIZATION;

    // default parameter values -- supported by experiments I performed
    cameras_distance_ = 4.5f;
    num_cameras_ = 7;
    cameras_elevation_ = 0.0;

    optimizer_ = std::make_shared<ShapeUnderClothOptimizer>(nullptr, nullptr);

    // as glog is used by class members
    google::InitGoogleLogging("PoseShapeExtractor");
}

PoseShapeExtractor::~PoseShapeExtractor()
{}

void PoseShapeExtractor::setupInitialization(InitializationType type, const std::string path)
{
    initialization_type_ = type;
    switch (type)
    {
    case PoseShapeExtractor::NO_INITIALIZATION:
        break;
    case PoseShapeExtractor::OPENPOSE:
        openpose_model_path_ = path;
        break;
    case PoseShapeExtractor::FILE:
        smpl_file_initilization_path_ = path;
        break;
    default:
        throw std::invalid_argument("PoseShapeExtractor:ERROR:Unknown initialization type");
        break;
    }
}

void PoseShapeExtractor::setupNewExperiment(std::shared_ptr<GeneralMesh> input, const std::string experiment_name)
{
    input_ = std::move(input);
    logger_ = std::make_shared<CustomLogger>(logging_base_path_, experiment_name + "_" + input_->getNameWithGroup());

    // for convenience
    input_->saveNormalizedMesh(logger_->getLogFolderPath());

    // update tools
    openpose_ = nullptr;
    char input_gender = convertInputGenderToChar_(*input_.get());

    smpl_ = std::make_shared<SMPLWrapper>(input_gender, smpl_model_path_);
}

void PoseShapeExtractor::setupNewDistplacementRegExperiment(std::shared_ptr<GeneralMesh> input, 
    double l2_weight, double smoothing_weight, const std::string experiment_name)
{
    optimizer_config_.displacement_reg_weight = l2_weight;
    optimizer_config_.displacement_smoothing_weight = smoothing_weight;

    setupNewExperiment(std::move(input),
        experiment_name + "_" + std::to_string(l2_weight)+"_" + std::to_string(smoothing_weight));
}

void PoseShapeExtractor::setupNewShapePruningExperiment(std::shared_ptr<GeneralMesh> input, double threshold, const std::string experiment_name)
{
    optimizer_config_.shape_prune_threshold = threshold;

    setupNewExperiment(std::move(input),
        experiment_name + "_" + std::to_string(threshold));
}

void PoseShapeExtractor::setupNewInnerVertsParamsExperiment(std::shared_ptr<GeneralMesh> input, 
    double inner_weight, double threshold, double gm_saturation, const std::string experiment_name)
{
    optimizer_config_.in_verts_scaling_weight = inner_weight;
    optimizer_config_.shape_prune_threshold = threshold;
    optimizer_config_.gm_saturation_threshold = gm_saturation;

    setupNewExperiment(std::move(input),
        experiment_name + "_" + std::to_string(inner_weight) + "_" + std::to_string(threshold) + "_" + std::to_string(gm_saturation));
}

void PoseShapeExtractor::setupNewPoseRegExperiment(std::shared_ptr<GeneralMesh> input, double weight, const std::string experiment_name)
{
    optimizer_config_.pose_reg_weight = weight;

    setupNewExperiment(std::move(input),
        experiment_name + "_" + std::to_string(weight));
}

void PoseShapeExtractor::setupNewCameraExperiment(std::shared_ptr<GeneralMesh> input, 
    double distance, int n_cameras, double elevation, const std::string experiment_name)
{
    cameras_distance_ = distance;
    num_cameras_ = n_cameras;
    cameras_elevation_ = elevation;
    setupNewExperiment(std::move(input),
        experiment_name + "_n_" + std::to_string(n_cameras) 
        + "_dist_" + std::to_string((int)(distance*10)) 
        + "_Y_" + std::to_string((int)(elevation * 10)));
}

std::shared_ptr<SMPLWrapper> PoseShapeExtractor::runExtraction()
{
    if (input_ == nullptr)
        throw std::runtime_error("PoseShapeExtractor:ERROR:You asked to run extraction before setting up the experiment.");

    switch (initialization_type_)
    {
    case PoseShapeExtractor::NO_INITIALIZATION:
        break;

    case PoseShapeExtractor::OPENPOSE:
        takePhotos_();

        estimateInitialPoseWithOP_();
        smpl_->savePosedOnlyToObj(logger_->getOpenPoseGuessesPath() + "/smpl_op_posed.obj");
        smpl_->logParameters(logger_->getOpenPoseGuessesPath() + "/smpl_op_posed_params.txt");
        break;

    case PoseShapeExtractor::FILE:
        smpl_->loadParametersFromFile(smpl_file_initilization_path_);
        break;

    default:
        throw std::runtime_error("PoseShapeExtractor:ERROR:Unknown initialization type");
        break;
    }
    
    runPoseShapeOptimization_();

    logger_->saveFinalModel(*smpl_);
    if (save_iteration_results_)
        logger_->saveIterationsSMPLObjects(*smpl_, iteration_outputs_);

    return smpl_;
}

void PoseShapeExtractor::setCollectIntermediateResults(bool collect)
{
    collect_iteration_results_ = collect;

    // enforce true if the save flag is on 
    if (save_iteration_results_)
        collect_iteration_results_ = save_iteration_results_;
}

void PoseShapeExtractor::viewCameraSetupForPhotos()
{
    if (input_ == nullptr)
    {
        throw std::invalid_argument("PoseShapeExtractor: need some input specified to show the scene. Sorry 0:)");
    }

    Photographer photographer(input_.get());

    photoSetUp_(photographer);
    
    photographer.viewScene();
}

void PoseShapeExtractor::viewFinalResult(bool withOpenPoseKeypoints)
{
    igl::opengl::glfw::Viewer viewer;
    igl::opengl::glfw::imgui::ImGuiMenu menu;
    viewer.plugins.push_back(&menu);

    Eigen::MatrixXd verts = smpl_->calcModel();

    viewer.data().set_mesh(verts, smpl_->getFaces());

    // display corresponding input points
    Eigen::VectorXd sqrD; Eigen::MatrixXd closest_points; Eigen::VectorXi closest_face_ids;
    igl::point_mesh_squared_distance(verts, input_->getNormalizedVertices(), input_->getFaces(), sqrD,
        closest_face_ids, closest_points);
    viewer.data().add_edges(verts, closest_points, Eigen::RowVector3d(1., 0., 0.));

    if (withOpenPoseKeypoints && initialization_type_ == PoseShapeExtractor::OPENPOSE)
    {
        if (openpose_ == nullptr)
        {
            throw std::runtime_error("PoseShapeExtractor Visualization: openpose keypoints are "
                " unavalible for Visualization. Run extraction first.");
        }
        Eigen::MatrixXd op_keypoints = openpose_->getKeypoints();
        op_keypoints = op_keypoints.block(0, 0, op_keypoints.rows(), 3);

        viewer.data().set_points(op_keypoints, Eigen::RowVector3d(1., 1., 0.));
    }
    else if (withOpenPoseKeypoints && initialization_type_ != PoseShapeExtractor::OPENPOSE)
        std::cout << "Warning::PoseShapeExtractor Visualization: "
            " OpenPose not used, no keypoints are displayed" << std::endl;
    
    viewer.launch();
}

void PoseShapeExtractor::viewIteratoinProcess()
{
    if (collect_iteration_results_ && iteration_outputs_.size() > 0)
    {
        // fill satic vars to be used in visualization
        iteration_outputs_to_viz_ = &iteration_outputs_;
        smpl_to_viz_ = smpl_;
        input_to_viz_ = input_;

        igl::opengl::glfw::Viewer viewer;
        igl::opengl::glfw::imgui::ImGuiMenu menu;
        viewer.plugins.push_back(&menu);

        iteration_viewer_counter_ = 0;
        viewer.callback_key_down = &visualizeIterationKeyDown_;
        viewer.callback_pre_draw = &visualizeIterationPreDraw_;
        viewer.core().is_animating = false;
        viewer.core().animation_max_fps = 24.;
        std::cout << "Press [space] to toggle animation or [Shift+F] to see the final result." << std::endl;
        viewer.launch();
    }
    else
    {
        std::cout << "PoseShapeExtractor: "
            << "I skipped visualization since iteration results were not collected." << std::endl;
    }
}

/// Private: ///

void PoseShapeExtractor::photoSetUp_(Photographer& photographer)
{
    const double pi = 3.141592653589793238463;

    // put cameras all around the target
    double circle_segment = 2. * pi / num_cameras_;
    double shift = 0.2;     // for fun. On a particular input, 5 cam + zero shift openpose faled miserably

    for (int i = 0; i < num_cameras_; i++)
    {
        photographer.addCameraToPosition(
            cos(shift + circle_segment * i), cameras_elevation_, sin(shift + circle_segment * i),
            cameras_distance_);
    }
}

void PoseShapeExtractor::takePhotos_()
{
    std::cout << "PoseShapeExtractor: I'm taking photos of the input!" << std::endl;

    Photographer photographer(input_.get());

    photoSetUp_(photographer);

    photographer.renderToImages(logger_->getPhotosFolderPath());
    photographer.saveImageCamerasParamsCV(logger_->getPhotosFolderPath());
}

void PoseShapeExtractor::estimateInitialPoseWithOP_()
{
    std::cout << "PoseShapeExtractor: I'm estimating the pose with OpenPose!" << std::endl;
    if (openpose_ == nullptr)
    {
        openpose_ = std::make_shared<OpenPoseWrapper>(logger_->getPhotosFolderPath(),
            logger_->getPhotosFolderPath(), num_cameras_,
            logger_->getOpenPoseGuessesPath(),
            openpose_model_path_);
    }
    
    openpose_->runPoseEstimation();

    // Map OpenPose pose to SMPL /////
    logger_->startRedirectCoutToFile("mapping_process_info.txt");
    openpose_->mapToSmpl(*smpl_);
    logger_->endRedirectCoutToFile();
}

void PoseShapeExtractor::runPoseShapeOptimization_()
{
    // update the data in the oprimizer in case it changed
    optimizer_->setNewInput(input_);
    optimizer_->setNewSMPLModel(smpl_);
    optimizer_->setConfig(optimizer_config_);

    std::cout << "Starting optimization...\n";

    double expetiment_param = 0.;
    
    logger_->startRedirectCoutToFile("optimization.txt");
    std::cout << "Input file: " << input_->getName() << std::endl;

    iteration_outputs_.clear();
    if (collect_iteration_results_)
    {
        optimizer_->findOptimalSMPLParameters(&iteration_outputs_);
    }
    else
    {
        optimizer_->findOptimalSMPLParameters(nullptr);
    }
    
    logger_->endRedirectCoutToFile();

    // by this time, SMPLWrapper should have optimized model  
    std::cout << "Optimization finished!\n";
}

char PoseShapeExtractor::convertInputGenderToChar_(const GeneralMesh& input)
{
    char gender;
    switch (input.getGender())
    {
    case GeneralMesh::FEMALE:
        gender = 'f';
        break;
    case GeneralMesh::MALE:
        gender = 'm';
        break;
    default:
        gender = 'u';
    }

    return gender;
}

bool PoseShapeExtractor::visualizeIterationPreDraw_(igl::opengl::glfw::Viewer & viewer)
{
    if (viewer.core().is_animating && iteration_viewer_counter_ < iteration_outputs_to_viz_->size())
    {
        viewer.data().clear();
        Eigen::MatrixXi faces = smpl_to_viz_->getFaces();

        viewer.data().set_mesh((*iteration_outputs_to_viz_)[iteration_viewer_counter_], faces);
        viewer.core().align_camera_center((*iteration_outputs_to_viz_)[iteration_viewer_counter_], faces);

        iteration_viewer_counter_++;
    }
    else if (viewer.core().is_animating && iteration_viewer_counter_ >= iteration_outputs_to_viz_->size())
    {
        viewer.core().is_animating = false;
        iteration_viewer_counter_ = 0;
        std::cout << "You can start the animation again by pressing [space]" << std::endl;
    }
    return false;
}

bool PoseShapeExtractor::visualizeIterationKeyDown_(igl::opengl::glfw::Viewer & viewer, unsigned char key, int modifier)
{
    if (key == ' ')
    {
        viewer.core().is_animating = !viewer.core().is_animating;
    }
    else if (key == 'F')
    {
        std::cout << "[Shift+F] pressed: Showing the final result."
            << "Press [space] to go back to animation mode." << std::endl;

        viewer.core().is_animating = false;

        // visualizing the final result only
        viewer.data().clear();
        const Eigen::MatrixXi& faces = smpl_to_viz_->getFaces();
        Eigen::MatrixXd& verts = (*iteration_outputs_to_viz_)[iteration_outputs_to_viz_->size() - 1];
        viewer.data().set_mesh(verts, faces);

        Eigen::VectorXd sqrD;
        Eigen::MatrixXd closest_points;
        Eigen::VectorXi closest_face_ids;
        igl::point_mesh_squared_distance(verts,
            input_to_viz_->getNormalizedVertices(), input_to_viz_->getFaces(), sqrD,
            closest_face_ids, closest_points);

        viewer.data().add_edges(verts, closest_points, Eigen::RowVector3d(1., 0., 0.));

        // visualize joint locations
        Eigen::MatrixXd finJointLocations = smpl_to_viz_->calcJointLocations();
        for (int i = 0; i < finJointLocations.rows(); ++i)
        {
            for (int j = 0; j < SMPLWrapper::SPACE_DIM; ++j)
            {
                finJointLocations(i, j) += smpl_to_viz_->getStatePointers().translation[j];
            }
        }
        viewer.data().add_points(finJointLocations, Eigen::RowVector3d(1., 1., 0.));
    }
    return false;
}
