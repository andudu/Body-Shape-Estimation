#include "SMPLWrapper.h"

int SMPLWrapper::joints_parents_[JOINTS_NUM];

SMPLWrapper::SMPLWrapper(char gender, const std::string path, const bool pose_blendshapes)
    : use_pose_blendshapes_(pose_blendshapes)
{
    // set the info
    if (gender != 'f' && gender != 'm') 
    {
        std::string message("Wrong gender supplied: ");
        message += gender;
        throw std::invalid_argument(message.c_str());
    }
    this->gender_ = gender;

    // !!!! expects a pre-defined file structure
    general_path_ = path + "/";
    gender_path_ = general_path_ + gender + "_smpl/";

    readTemplate_();
    readJointMat_();
    readPoseStiffnessMat_();
    readJointNames_();
    readShapes_();
    if (use_pose_blendshapes_)
        readPoseBlendshapes_();
    readWeights_();
    readHierarchy_();

    joint_locations_template_ = calcJointLocations();
    fillVertsNeighbours_();

    // initilize the model intermediate values
    calcModel();
}

SMPLWrapper::~SMPLWrapper()
{
}

void SMPLWrapper::rotateLimbToDirection(const std::string joint_name, const E::Vector3d& direction)
{
    assert(SMPLWrapper::SPACE_DIM == 3 && "rotateLimbToDirection() can only be used in 3D world");

    std::cout << "----- Setting Bone Direction for joint " << joint_name << " -----" << std::endl
        << "To direction \n" << direction << std::endl;
    
    int joint_id;
    try { 
        joint_id = joint_names_.at(joint_name); 
        if (joint_id == 0) // root
            throw std::out_of_range("SMPLWrapper::ERROR::use specialized method to set up root");
        if (joint_name == "LowBack" || joint_name == "MiddleBack" || joint_name == "TopBack")
            throw std::out_of_range("SMPLWrapper::ERROR::use specialized method to set up back twist");
    }
    catch (std::out_of_range& e)
    {
        std::cout << "SMPLWRapper::rotateLimbToDirection was supplied with incorrect joint" << std::endl;
        throw e;
    }

    int child_id;
    for (int i = 0; i < JOINTS_NUM; i++)
    {
        if (joints_parents_[i] == joint_id)
        {
            child_id = i;
            break;
        }
    }
    
    std::cout << "SMPL Joint pair " << joint_id << " -> " << child_id << std::endl;

    // get default bone direction; it also updates joint_global_transform_
    E::MatrixXd joint_locations = calcJointLocations_(nullptr, nullptr, &state_.pose);
    E::Vector3d default_dir =
        (joint_locations.row(child_id) - joint_locations.row(joint_id)).transpose();

    std::cout << "Default direction \n" << default_dir << std::endl;

    // skip calculations for zero vectors
    if (direction.norm() * default_dir.norm() > 0.0)
    {
        E::Vector3d axis = angle_axis_(default_dir, direction);
        assignJointGlobalRotation_(joint_id, axis, fk_transforms_);
    }

    // sanity check
    // TODO add efficiency flag
    E::MatrixXd new_joint_locations = calcJointLocations_(nullptr, nullptr, &state_.pose);
    E::Vector3d new_dir = (new_joint_locations.row(child_id) - new_joint_locations.row(joint_id)).transpose();
    std::cout << "Difference with the target " << std::endl
        << new_dir.normalized() - direction.normalized() << std::endl;

    E::Vector3d fact_axis = angle_axis_(default_dir, new_dir);
    std::cout << "Fact Turned Angle: " << fact_axis.norm() * 180 / 3.1415 << std::endl;

    // recalculate model with updated parameters
    calcModel();
}

void SMPLWrapper::rotateRoot(const E::Vector3d& body_up, const E::Vector3d& body_left_to_right)
{
    assert(SMPLWrapper::SPACE_DIM == 3 && "rotateRoot() can only be used in 3D world");

    std::cout << "----- Setting Root Rotation -----" << std::endl
        << "With UP (Y) to \n" << body_up << std::endl
        << "With Right (X) to \n" << body_left_to_right << std::endl;

    E::Vector3d default_Y(0., 1., 0.);
    E::Vector3d rotation_match_body_up = angle_axis_(default_Y, body_up);

    std::cout << "Vertical Angle-axis rotation with angle_for_up " << rotation_match_body_up.norm() * 180 / 3.1415
        << "\n" << rotation_match_body_up << std::endl;
    
    E::Vector3d X_updated = rotate_by_angle_axis_(E::Vector3d(1., 0., 0), rotation_match_body_up);
    E::Vector3d Y_matched = rotate_by_angle_axis_(default_Y, rotation_match_body_up);
    
    // use rotation around Y + projection instead of the full vector
    // to gurantee the Y stays where it is and hips are matched as close as possible
    E::Vector3d left_to_right_projected = body_left_to_right - body_left_to_right.dot(Y_matched) * Y_matched;
    E::Vector3d cross_product = X_updated.cross(left_to_right_projected);
    int sin_sign = cross_product.dot(Y_matched) >= 0 ? 1 : -1;
    double angle = atan2(
        sin_sign * cross_product.norm(), // sin
        X_updated.dot(left_to_right_projected));   // cos

    E::Vector3d rotation_match_hips = angle * Y_matched;

    E::Vector3d combined_rotation = combine_two_angle_axis_(rotation_match_body_up, rotation_match_hips);

    std::cout << "Combined rotation with angle " << combined_rotation.norm() * 180 / 3.1415
        << "\n" << combined_rotation << std::endl;

    assignJointGlobalRotation_(0, combined_rotation, fk_transforms_);

    // recalculate model with updated parameters
    calcModel();
}

void SMPLWrapper::twistBack(const E::Vector3d& shoulder_dir)
{
    assert(SMPLWrapper::SPACE_DIM == 3 && "rotateRoot() can only be used in 3D world");

    std::cout << "----- Setting shoulders -----" << std::endl
        << "To direction \n" << shoulder_dir << std::endl;

    // get default bone direction; it also updates joint_global_transform_
    E::MatrixXd joint_locations = calcJointLocations_(nullptr, nullptr, &state_.pose);

    int Rshoulder_id = joint_names_.at("RShoulder");
    int Lshoulder_id = joint_names_.at("LShoulder");

    E::Vector3d default_dir =
        (joint_locations.row(Lshoulder_id) - joint_locations.row(Rshoulder_id)).transpose();

    std::cout << "Default direction \n" << default_dir << std::endl;

    E::Vector3d rotation = angle_axis_(default_dir, shoulder_dir);
    double angle = rotation.norm();
    E::Vector3d axis = rotation.normalized();

    std::cout << "Angle-axis rotation with angle " << angle * 180 / 3.1415
        << "\n" << rotation << std::endl;

    // divide between the back joints
    assignJointGlobalRotation_(joint_names_.at("LowBack"), axis * angle / 3, fk_transforms_);

    updateJointsFKTransforms_(state_.pose, joint_locations_template_);
    assignJointGlobalRotation_(joint_names_.at("MiddleBack"), axis * angle / 3, fk_transforms_);

    updateJointsFKTransforms_(state_.pose, joint_locations_template_);
    assignJointGlobalRotation_(joint_names_.at("TopBack"), axis * angle / 3, fk_transforms_);

    // recalculate model with updated parameters
    calcModel();
}

void SMPLWrapper::translateTo(const E::VectorXd & center_point)
{
    E::MatrixXd verts = calcModel();
    E::VectorXd mean_point = verts.colwise().mean();

    state_.translation = center_point - mean_point;

    // recalculate model with updated parameters
    calcModel();
}

void SMPLWrapper::loadParametersFromFile(const std::string filename)
{
    std::ifstream in(filename);
    std::string trash;

    // out << "Translation [ " << std::endl;
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');   // skips one line
    for (int i = 0; i < SMPLWrapper::SPACE_DIM; i++)
        in >> state_.translation[i] >> trash;
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    // out << "Pose params [ \n";
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    for (int i = 0; i < SMPLWrapper::JOINTS_NUM; i++)
    {
        for (int j = 0; j < SMPLWrapper::SPACE_DIM; j++)
        {
            in >> state_.pose(i, j) >> trash;
        }
    }
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    // out << "Shape (betas) params [ " << std::endl;
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    for (int i = 0; i < SMPLWrapper::SHAPE_SIZE; i++)
        in >> state_.shape[i] >> trash;

    // No need to read joints locations
    in.close();

    // recalculate model with updated parameters
    calcModel();
}

E::MatrixXd SMPLWrapper::calcModel(
    const E::VectorXd * translation, 
    const ERMatrixXd * pose,
    const E::VectorXd * shape,
    const ERMatrixXd * displacement,
    E::MatrixXd * pose_jac, E::MatrixXd * shape_jac, E::MatrixXd * displacement_jac)
{
    // assignment won't work without cast
    E::MatrixXd verts = verts_template_normalized_;

    if (shape != nullptr)
        shapeSMPL_(*shape, verts, shape_jac);

    if (pose != nullptr)
    {
        // will be displaced inside poseSMPL_ method
        poseSMPL_(*pose, verts, displacement, pose_jac);

        // should be updated for the given pose
        // TODO: add the use of pre-computed LBS Matrices 
        if (shape_jac != nullptr)
            for (int i = 0; i < SMPLWrapper::SHAPE_SIZE; ++i)
                poseSMPL_(*pose, shape_jac[i], displacement);  // Pose needs recalculation because joint positions at T are new
        if (displacement_jac != nullptr)
            for (int axis = 0; axis < SPACE_DIM; axis++)
            {
                displacement_jac[axis] = E::MatrixXd::Zero(VERTICES_NUM, SPACE_DIM);
                displacement_jac[axis].col(axis).setOnes();
                poseSMPL_(*pose, displacement_jac[axis], nullptr, nullptr, true); // no pose recalculation
            }
        // verts are displaced and posed
    }
    else if (displacement != nullptr)
    {
        verts = verts + *displacement;
    }

    if (translation != nullptr)
        translate_(*translation, verts);

    return verts;
}

E::MatrixXd SMPLWrapper::calcVertexNormals(const E::MatrixXd * verts)
{
    E::MatrixXd normals;
    igl::per_vertex_normals(*verts, faces_, normals);

    return normals;
}

E::MatrixXd SMPLWrapper::calcModel()
{
    return calcModel(&state_.translation, &state_.pose, &state_.shape, &state_.displacements);
}

E::MatrixXd SMPLWrapper::calcJointLocations()
{
    return calcJointLocations_(&state_.translation, &state_.shape, &state_.pose);
}

void SMPLWrapper::saveToObj(const std::string filename) 
{
    saveToObj_(&state_.translation, &state_.pose, &state_.shape, nullptr, filename);
}

void SMPLWrapper::saveWithDisplacementToObj(const std::string filename)
{
    saveToObj_(&state_.translation, &state_.pose, &state_.shape, &state_.displacements, filename);
}

void SMPLWrapper::savePosedOnlyToObj(const std::string filename) 
{
    saveToObj_(&state_.translation, &state_.pose, nullptr, nullptr, filename);
}

void SMPLWrapper::saveShapedOnlyToObj(const std::string filename) 
{
    saveToObj_(&state_.translation, nullptr, &state_.shape, nullptr, filename);
}

void SMPLWrapper::saveShapedWithDisplacementToObj(const std::string filename)
{
    saveToObj_(&state_.translation, nullptr, &state_.shape, &state_.displacements, filename);
}

void SMPLWrapper::logParameters(const std::string filename)
{
    std::ofstream out(filename);

    out << "Translation [ " << std::endl;
    for (int i = 0; i < SMPLWrapper::SPACE_DIM; i++)
        out << state_.translation[i] << " , ";
    out << std::endl << "]" << std::endl;

    out << "Pose params [ \n";
    for (int i = 0; i < SMPLWrapper::JOINTS_NUM; i++)
    {
        for (int j = 0; j < SMPLWrapper::SPACE_DIM; j++)
        {
            out << state_.pose(i, j) << " , ";
        }
        out << std::endl;
    }
    out << "]" << std::endl;

    out << "Shape (betas) params [ " << std::endl;
    for (int i = 0; i < SMPLWrapper::SHAPE_SIZE; i++)
        out << state_.shape[i] << " , ";
    out << std::endl << "]" << std::endl;

    out << "Joints locations for posed and shaped model [" << std::endl;
    out << calcJointLocations() << std::endl;
    out << "]" << std::endl;

    out.close();
}

/// PRIVATE

void SMPLWrapper::readTemplate_()
{
    std::string file_name = gender_path_ + gender_ + "_shapeAv.obj";

    bool success = igl::readOBJ(file_name, verts_template_, faces_);
    if (!success)
    {
        std::string message("Abort: Could not read SMPL template at ");
        message += file_name;
        throw std::invalid_argument(message.c_str());
    }

    E::VectorXd mean_point = verts_template_.colwise().mean();
    verts_template_normalized_ = verts_template_.rowwise() - mean_point.transpose();
}

void SMPLWrapper::readJointMat_()
{
    std::string file_name(this->gender_path_);
    file_name += this->gender_;
    file_name += "_joints_mat.txt";

    // copy from Meekyong code example
    std::fstream inFile;
    inFile.open(file_name, std::ios_base::in);
    int joints_n, verts_n;
    inFile >> joints_n;
    inFile >> verts_n;
    // Sanity check
    if (joints_n != SMPLWrapper::JOINTS_NUM || verts_n != SMPLWrapper::VERTICES_NUM)
        throw std::invalid_argument("Joint matrix info (number of joints and vertices) is incompatible with the model");

    this->jointRegressorMat_.resize(joints_n, verts_n);
    for (int i = 0; i < joints_n; i++)
        for (int j = 0; j < verts_n; j++)
            inFile >> this->jointRegressorMat_(i, j);

    inFile.close();
}

void SMPLWrapper::readPoseStiffnessMat_()
{
    std::string stiffness_filename = general_path_ + "stiffness.txt";

    std::fstream inFile;
    inFile.open(stiffness_filename, std::ios_base::in);
    constexpr int NON_ROOT_POSE_SIZE = POSE_SIZE - SPACE_DIM;
    int rows, cols;
    inFile >> rows;
    inFile >> cols;
    // Sanity check
    if (rows != cols)
        throw std::invalid_argument("Striffness matrix is not a square matrix");
    if (rows != SMPLWrapper::POSE_SIZE - SMPLWrapper::SPACE_DIM)
        throw std::invalid_argument("Striffness matrix size doesn't match the number of non-root pose parameters");

    // To make matrix applicable to full pose vector
    pose_stiffness_.resize(SMPLWrapper::POSE_SIZE, SMPLWrapper::POSE_SIZE);
    for (int i = 0; i < SMPLWrapper::SPACE_DIM; i++)
        for (int j = 0; j < SMPLWrapper::POSE_SIZE; j++)
            pose_stiffness_(i, j) = pose_stiffness_(j, i) = 0.;

    // Now read from file
    for (int i = SMPLWrapper::SPACE_DIM; i < SMPLWrapper::POSE_SIZE; i++)
        for (int j = SMPLWrapper::SPACE_DIM; j < SMPLWrapper::POSE_SIZE; j++)
            inFile >> pose_stiffness_(i, j);

    inFile.close();
}

void SMPLWrapper::readJointNames_()
{
    std::string file_name(this->general_path_);
    file_name += "joint_names.txt";

    std::fstream inFile;
    inFile.open(file_name, std::ios_base::in);
    int joints_n;
    inFile >> joints_n;
    // Sanity check
    if (joints_n != JOINTS_NUM)
    {
        throw std::invalid_argument("Number of joint names specified doesn't match current SMPLWrapper settings");
    }

    std::string joint_name;
    int jointId;
    for (int i = 0; i < joints_n; i++)
    {
        inFile >> joint_name;
        inFile >> jointId;
        joint_names_.insert(DictEntryInt(joint_name, jointId));
    }

    inFile.close();
}

void SMPLWrapper::readShapes_()
{
    std::string file_path = gender_path_ + gender_ + "_blendshape/shape";

    Eigen::MatrixXi fakeFaces(SMPLWrapper::VERTICES_NUM, SMPLWrapper::SPACE_DIM);

    for (int i = 0; i < SMPLWrapper::SHAPE_SIZE; i++)
    {
        std::string file_name(file_path);
        file_name += std::to_string(i);
        file_name += ".obj";

        igl::readOBJ(file_name, shape_diffs_[i], fakeFaces);

        shape_diffs_[i] -= verts_template_;
    }
}

void SMPLWrapper::readPoseBlendshapes_()
{
    std::string file_path = gender_path_ + gender_ + "_pose_blendshapes/Pose";

    Eigen::MatrixXi fakeFaces(SMPLWrapper::VERTICES_NUM, SMPLWrapper::SPACE_DIM);

    for (int i = 0; i < SMPLWrapper::POSE_BLENDSHAPES_NUM; i++)
    {
        std::string file_name(file_path);
        std::string id_str = std::to_string(i);
        file_name += std::string(3 - id_str.size(), '0') + id_str;
        file_name += ".obj";

        igl::readOBJ(file_name, pose_diffs_[i], fakeFaces);

        pose_diffs_[i] -= verts_template_;
    }
}

void SMPLWrapper::readWeights_()
{
    std::string file_name(this->gender_path_);
    file_name += this->gender_;
    file_name += "_weight.txt";

    std::fstream inFile;
    inFile.open(file_name, std::ios_base::in);
    int joints_n, verts_n;
    inFile >> joints_n;
    inFile >> verts_n;
    // Sanity check
    if (joints_n != SMPLWrapper::JOINTS_NUM || verts_n != SMPLWrapper::VERTICES_NUM)
        throw std::invalid_argument("Weights info (number of joints and vertices) is incompatible with the model");

    std::vector<E::Triplet<double>> tripletList;
    tripletList.reserve(verts_n * SMPLWrapper::WEIGHTS_BY_VERTEX);     // for faster filling performance
    double tmp;
    for (int i = 0; i < verts_n; i++)
    {
        for (int j = 0; j < joints_n; j++)
        {
            inFile >> tmp;
            if (tmp > 0.00001)  // non-zero weight
                tripletList.push_back(E::Triplet<double>(i, j, tmp));
        }
    }
    this->weights_.resize(verts_n, joints_n);
    this->weights_.setFromTriplets(tripletList.begin(), tripletList.end());

#ifdef DEBUG
    std::cout << "Weight sizes " << this->weights_.outerSize() << " " << this->weights_.innerSize() << std::endl;
#endif // DEBUG

    inFile.close();
}

void SMPLWrapper::readHierarchy_()
{
    std::string file_name(this->general_path_);
    file_name += "jointsHierarchy.txt";

    std::fstream inFile;
    inFile.open(file_name, std::ios_base::in);
    int joints_n;
    inFile >> joints_n;
    // Sanity check
    if (joints_n != SMPLWrapper::JOINTS_NUM)
    {
        throw std::invalid_argument("Number of joints in joints hierarchy info is incompatible with the model");
    }
    
    int tmpId;
    for (int j = 0; j < joints_n; j++)
    {
        inFile >> tmpId;
        inFile >> joints_parents_[tmpId];
    }

    inFile.close();
}

void SMPLWrapper::fillVertsNeighbours_()
{
    for (int face_id = 0; face_id < faces_.rows(); face_id++)
    {
        for (int corner_id = 0; corner_id < faces_.cols(); corner_id++)
        {
            int vert_id = faces_(face_id, corner_id);
            for (int shift = 1; shift < faces_.cols(); shift++)
            {
                int neighbour_vert_id = faces_(face_id, (corner_id + shift) % faces_.cols());
                // add if new
                if (std::find(verts_neighbours_[vert_id].begin(), verts_neighbours_[vert_id].end(), neighbour_vert_id)
                    == verts_neighbours_[vert_id].end())
                {
                    verts_neighbours_[vert_id].push_back(neighbour_vert_id);
                }
            }
        }
    }
}

void SMPLWrapper::saveToObj_(const E::VectorXd* translation, const ERMatrixXd * pose,
 const E::VectorXd* shape,
    const ERMatrixXd* displacements, const std::string filename)
{
    E::MatrixXd verts = calcModel(translation, pose, shape, displacements);

    igl::writeOBJ(filename, verts, faces_);
}

E::Vector3d SMPLWrapper::angle_axis_(const E::Vector3d& from, const E::Vector3d& to)
{
    assert(SMPLWrapper::SPACE_DIM == 3 && "angle_axis_() can only be used in 3D world");

    if (from.norm() * to.norm() > 0.0)
    {
        E::Vector3d axis = from.cross(to);
        double sin_a = axis.norm() / (from.norm() * to.norm());
        double cos_a = from.dot(to) / (from.norm() * to.norm());
        double angle = atan2(sin_a, cos_a);

        axis.normalize();
        axis = angle * axis;
        return axis;
    }
    return E::Vector3d(0, 0, 0);
}

E::Vector3d SMPLWrapper::rotate_by_angle_axis_(const E::Vector3d& vector, const E::Vector3d& angle_axis_rotation)
{
    double angle = angle_axis_rotation.norm();
    E::Vector3d axis = angle_axis_rotation.normalized();

    // Rodrigues' rotation formula
    // https://en.wikipedia.org/wiki/Rodrigues%27_rotation_formula
    E::Vector3d rotated_vec =
        cos(angle) * vector
        + sin(angle) * axis.cross(vector)
        + (1 - cos(angle)) * axis.dot(vector) * axis;

    return rotated_vec;
}

E::Vector3d SMPLWrapper::combine_two_angle_axis_(const E::Vector3d& first, const E::Vector3d& second)
{
    double angle_first = first.norm();
    E::Vector3d axis_first = first.normalized();
    double angle_second = second.norm();
    E::Vector3d axis_second = second.normalized();

    // Rodrigues' Formula 
    // https://math.stackexchange.com/questions/382760/composition-of-two-axis-angle-rotations
    // get axis
    E::Vector3d axis_sin_scaled =
        cos(angle_first / 2) * sin(angle_second / 2) * axis_second
        + sin(angle_first / 2) * cos(angle_second / 2) * axis_first
        + sin(angle_first / 2) * sin(angle_second / 2) * second.cross(first);
    E::Vector3d axis = axis_sin_scaled.normalized();

    // get angle
    double angle_half_sin = axis_sin_scaled.norm();
    double angle_half_cos = cos(angle_first / 2) * cos(angle_second / 2)
        - axis_first.dot(axis_second) * sin(angle_first / 2) * sin(angle_second / 2);

    double angle = 2 * atan2(angle_half_sin, angle_half_cos);

    return angle * axis;
}

void SMPLWrapper::assignJointGlobalRotation_(int joint_id, E::VectorXd rotation, 
    const EHomoCoordMatrix(&fk_transform)[SMPLWrapper::JOINTS_NUM])
{
    Eigen::Vector3d rotation_local;
    if (joint_id > 0)
    {
        Eigen::MatrixXd joint_inverse_rotation =
            fk_transform[joint_id].block(0, 0, SPACE_DIM, SPACE_DIM).transpose();

        rotation_local = joint_inverse_rotation * rotation;
    }
    else
    {
        rotation_local = rotation;
    }
    
    state_.pose.row(joint_id) = rotation_local;
}

void SMPLWrapper::shapeSMPL_(const E::VectorXd& shape, E::MatrixXd &verts, E::MatrixXd* shape_jac)
{
#ifdef DEBUG
    std::cout << "shape (analytic)" << std::endl;
#endif // DEBUG
    for (int i = 0; i < SHAPE_SIZE; i++)
    {
        verts += shape[i] * shape_diffs_[i];
    }

    if (shape_jac != nullptr)
    {
        for (int i = 0; i < SHAPE_SIZE; i++)
        {
            shape_jac[i] = shape_diffs_[i];
        }
    }
}

void SMPLWrapper::poseSMPL_(const ERMatrixXd& pose, E::MatrixXd & verts,
    const ERMatrixXd *displacement, E::MatrixXd * pose_jac, bool use_previous_pose_matrix)
{
    // TODO make sure we don't end up with empty pose jac when reusing pose
    if (!use_previous_pose_matrix)
    {
        // ! Impostant: don't use displacements to obtain joint_locations. 
        // jointRegressor was only trained on the model data
        joint_locations_ = jointRegressorMat_ * verts;
        updateJointsFKTransforms_(pose, joint_locations_, pose_jac != nullptr);
    }

    E::MatrixXd joints_global_transform = extractLBSJointTransformFromFKTransform_(
        fk_transforms_, joint_locations_,
        &fk_derivatives_, pose_jac);

    // Apply pose blendshapes
    if (use_pose_blendshapes_)
    {
        addPoseBlendshapes_(local_rotations_, verts);
        // get blendshape deritatives w.r.t. every pose parameter
        if (pose_jac != nullptr && !use_previous_pose_matrix)   // if the pose is reused, we can use previous derivatives
        {
            calcPoseBlendshapesJac_(local_rotations_jac_, blendshapes_derivatives_);
        }

    }

    // LBS Matrix
    E::SparseMatrix<double> LBSMat;
    if (displacement != nullptr)
        LBSMat = getLBSMatrix_(verts + *displacement);
    else
        LBSMat = getLBSMatrix_(verts);

    // displaced and posed
    verts = LBSMat * joints_global_transform;

    if (pose_jac != nullptr)
    {
        // pose_jac[pose_component] at this point has the same structure as jointsTransformation
        for (int pose_component = 0; pose_component < SMPLWrapper::POSE_SIZE; ++pose_component)
        {
            // Rotational component
            pose_jac[pose_component].applyOnTheLeft(LBSMat);

            // Pose blendshapes component
            if (use_pose_blendshapes_)
            {
                E::SparseMatrix<double> blendshape_LBSMat = getLBSMatrix_(blendshapes_derivatives_[pose_component]);
                pose_jac[pose_component] += blendshape_LBSMat * joints_global_transform;
            }
        }
    }
}

void SMPLWrapper::translate_(const E::VectorXd& translation, E::MatrixXd & verts)
{
    verts.rowwise() += translation.transpose();

    // Jac w.r.t. translation is identity: dv_i / d_tj == 1 
}

void SMPLWrapper::addJointPoseBlendshape_(const int blendshape_id_offset, const E::MatrixXd & coeff, E::MatrixXd & verts)
{
    for (int col = 0; col < coeff.cols(); col++)
    {
        for (int row = 0; row < coeff.rows(); row++)
        {
            int blendshape_id = blendshape_id_offset + row * coeff.cols() + col;
            if (blendshape_id >= POSE_BLENDSHAPES_NUM)
                throw std::out_of_range("Error::applyPoseBlendshapes::requested non-existing blendshape id");
            verts += coeff(row, col) * pose_diffs_[blendshape_id];
        }
    }
}

void SMPLWrapper::addPoseBlendshapes_(const E::MatrixXd local_rotations_[SMPLWrapper::JOINTS_NUM], E::MatrixXd & verts)
{
    // no pose blendshapes for root
    for (int joint = 1; joint < JOINTS_NUM; joint++)
    {
        // substact T-pose rotation
        E::MatrixXd coeff = local_rotations_[joint] - E::MatrixXd::Identity(SPACE_DIM, SPACE_DIM);

        addJointPoseBlendshape_((joint - 1) * SPACE_DIM * SPACE_DIM, coeff, verts);
    }
}

void SMPLWrapper::calcPoseBlendshapesJac_(const E::MatrixXd local_rotations_jac_[SMPLWrapper::POSE_SIZE],
    E::MatrixXd * blendshapes_jac)
{
    // initialize jac w.r.t. root to zero 
    for (int dim = 0; dim < SPACE_DIM; dim++)
    {
        blendshapes_jac[0 + dim].setZero(VERTICES_NUM, SPACE_DIM);
    }

    // the rest of the joints
    for (int joint = 1; joint < JOINTS_NUM; joint++)
    {
        int blendshape_id_offset = (joint - 1) * SPACE_DIM * SPACE_DIM;
        for (int dim = 0; dim < SPACE_DIM; dim++)
        {
            int param_id = joint * SPACE_DIM + dim;
            blendshapes_jac[param_id].setZero(VERTICES_NUM, SPACE_DIM);

            // apply pose blendshapes with rotations
            addJointPoseBlendshape_(blendshape_id_offset,
                local_rotations_jac_[param_id],         // 
                blendshapes_jac[param_id]);
        }
    }
}

E::MatrixXd SMPLWrapper::calcJointLocations_(const E::VectorXd* translation,
    const E::VectorXd* shape, const ERMatrixXd * pose)
{
    E::MatrixXd joint_locations;

    if (joint_locations_template_.size() > 0)
    {
        joint_locations = joint_locations_template_;
    }
    else
    {
        joint_locations = jointRegressorMat_ * verts_template_normalized_;
    }

    if (shape != nullptr)
    {
        E::MatrixXd verts = verts_template_normalized_;
        shapeSMPL_(*shape, verts);
        joint_locations = jointRegressorMat_ * verts;
    }

    if (pose != nullptr)
    {
        updateJointsFKTransforms_(*pose, joint_locations);
        joint_locations = extractJointLocationFromFKTransform_(fk_transforms_);
    }

    if (translation != nullptr)
    {
        translate_(*translation, joint_locations);
    }

    return joint_locations;
}

E::MatrixXd SMPLWrapper::extractJointLocationFromFKTransform_(
    const EHomoCoordMatrix(&fk_transform)[SMPLWrapper::JOINTS_NUM])
{
    // Go over the fk_transform and gather joint locations
    E::MatrixXd joints_locations(JOINTS_NUM, SPACE_DIM);
    for (int j = 0; j < JOINTS_NUM; j++)
    {
        // translation info is in the last column
        joints_locations.row(j) = fk_transform[j].block(0, SPACE_DIM, SPACE_DIM, 1).transpose();
    }

    return joints_locations;
}

E::MatrixXd SMPLWrapper::extractLBSJointTransformFromFKTransform_(
    const EHomoCoordMatrix(&fk_transform)[SMPLWrapper::JOINTS_NUM], 
    const E::MatrixXd & t_pose_joints_locations,
    const E::MatrixXd(*FKDerivatives)[SMPLWrapper::JOINTS_NUM][SMPLWrapper::POSE_SIZE],
    E::MatrixXd * jacsTotal)
{
    E::MatrixXd joints_transform(HOMO_SIZE * SMPLWrapper::JOINTS_NUM, SMPLWrapper::SPACE_DIM);

    // utils vars: not to recreate them on each loop iteration
    E::MatrixXd inverse_t_pose_translate;
    E::MatrixXd tmpPointGlobalTransform;
    E::MatrixXd tmpPointGlobalJac;

    if (jacsTotal != nullptr)
        for (int i = 0; i < SMPLWrapper::POSE_SIZE; ++i)
            jacsTotal[i].setZero(HOMO_SIZE * SMPLWrapper::JOINTS_NUM, SMPLWrapper::SPACE_DIM);

    // Go over the fk_transform_ matrix and create LBS-compatible matrix
    for (int j = 0; j < JOINTS_NUM; j++)
    {
        // inverse is needed to transform verts coordinates to local coordinate system
        inverse_t_pose_translate = get3DTranslationMat_(-t_pose_joints_locations.row(j));
        tmpPointGlobalTransform = fk_transform[j] * inverse_t_pose_translate;

        joints_transform.block(HOMO_SIZE * j, 0, HOMO_SIZE, SMPLWrapper::SPACE_DIM)
            = tmpPointGlobalTransform.transpose().leftCols(SMPLWrapper::SPACE_DIM);

        // and fill corresponding jac format
        if (jacsTotal != nullptr && FKDerivatives != nullptr)
        {
            // jac w.r.t current joint rotation coordinates
            for (int dim = 0; dim < SMPLWrapper::SPACE_DIM; ++dim)
            {
                tmpPointGlobalJac = 
                    (*FKDerivatives)[j][j * SMPLWrapper::SPACE_DIM + dim] * inverse_t_pose_translate;
                jacsTotal[j * SMPLWrapper::SPACE_DIM + dim].block(j * HOMO_SIZE, 0, HOMO_SIZE, SMPLWrapper::SPACE_DIM)
                    = tmpPointGlobalJac.transpose().leftCols(SMPLWrapper::SPACE_DIM);
            }

            // jac w.r.t. ancessors rotation coordinates         
            for (int parent_dim = 0; parent_dim < (joints_parents_[j] + 1) * SMPLWrapper::SPACE_DIM; ++parent_dim)
            {
                if ((*FKDerivatives)[joints_parents_[j]][parent_dim].size() > 0)
                {
                    tmpPointGlobalJac = (*FKDerivatives)[j][parent_dim] * inverse_t_pose_translate;
                    jacsTotal[parent_dim].block(j * HOMO_SIZE, 0, HOMO_SIZE, SMPLWrapper::SPACE_DIM)
                        = tmpPointGlobalJac.transpose().leftCols(SMPLWrapper::SPACE_DIM);
                }
            }
        }
    }

    return joints_transform;
}

void SMPLWrapper::updateJointsFKTransforms_(
    const ERMatrixXd & pose, const E::MatrixXd & t_pose_joints_locations, bool calc_derivatives)
{
#ifdef DEBUG
    std::cout << "global transform (analytic)" << std::endl;
#endif // DEBUG

    // uses functions that assume input in 3D (see below)
    assert(SMPLWrapper::SPACE_DIM == 3 && "The function can only be used in 3D world");

    // TODO make more elegant local rotation and local jacobian assertions

    // root as special case
    fk_transforms_[0] = get3DLocalTransformMat_(pose.row(0), t_pose_joints_locations.row(0));
    local_rotations_[0] = fk_transforms_[0].block(0, 0, SPACE_DIM, SPACE_DIM); // remember for pose blendshapes
    if (calc_derivatives)
    {
        get3DLocalTransformJac_(pose.row(0), fk_transforms_[0], fk_derivatives_[0]);
        for (int dim = 0; dim < SPACE_DIM; dim++)
            local_rotations_jac_[dim] = fk_derivatives_[0][dim].block(0, 0, SPACE_DIM, SPACE_DIM);
    }

    E::MatrixXd localTransform, localTransformJac[SMPLWrapper::SPACE_DIM];
    for (int joint_id = 1; joint_id < SMPLWrapper::JOINTS_NUM; joint_id++)
    {
        localTransform = get3DLocalTransformMat_(pose.row(joint_id),
            t_pose_joints_locations.row(joint_id) - t_pose_joints_locations.row(joints_parents_[joint_id]));
        local_rotations_[joint_id] = localTransform.block(0, 0, SPACE_DIM, SPACE_DIM);

        // Forward Kinematics Formula
        fk_transforms_[joint_id] = fk_transforms_[joints_parents_[joint_id]] * localTransform;

        if (calc_derivatives)
        {
            get3DLocalTransformJac_(pose.row(joint_id), localTransform, localTransformJac);

            // jac w.r.t current joint rot coordinates
            for (int dim = 0; dim < SMPLWrapper::SPACE_DIM; ++dim)
            {
                fk_derivatives_[joint_id][joint_id * SMPLWrapper::SPACE_DIM + dim] = 
                    fk_transforms_[joints_parents_[joint_id]] * localTransformJac[dim];
                local_rotations_jac_[joint_id * SMPLWrapper::SPACE_DIM + dim] =
                    localTransformJac[dim].block(0, 0, SPACE_DIM, SPACE_DIM);
            }

            // jac w.r.t. ancessors rotation coordinates         
            for (int j = 0; j < (joints_parents_[joint_id] + 1) * SMPLWrapper::SPACE_DIM; ++j)
            {
                if (fk_derivatives_[joints_parents_[joint_id]][j].size() > 0)
                {
                    fk_derivatives_[joint_id][j] = 
                        fk_derivatives_[joints_parents_[joint_id]][j] * localTransform;
                }
            }
        }

    }

    // now the fk_* are updated
}

E::MatrixXd SMPLWrapper::get3DLocalTransformMat_(const E::Vector3d & jointAxisAngleRotation, const E::Vector3d & jointToParentDist)
{
    // init
    E::MatrixXd localTransform;
    localTransform.setIdentity(4, 4);   // in homogenious coordinates
    localTransform.block(0, 3, 3, 1) = jointToParentDist; // g(0)

    // prepare the info
    const E::Vector3d& w = jointAxisAngleRotation;
    double norm = w.norm();
    E::Matrix3d w_skew;
    w_skew <<
        0, -w[2], w[1],
        w[2], 0, -w[0],
        -w[1], w[0], 0;
    w_skew /= norm;
    E::Matrix3d exponent = E::Matrix3d::Identity();

    if (norm > 0.0001)  // don't waste computations on zero joint movement
    {
        // apply Rodrigues formula
        exponent += w_skew * sin(norm) + w_skew * w_skew * (1. - cos(norm));
        localTransform.block(0, 0, 3, 3) = exponent;
    }

    return localTransform;
}

void SMPLWrapper::get3DLocalTransformJac_(const E::Vector3d & jointAxisAngleRotation,
    const E::MatrixXd & transform_mat, E::MatrixXd* local_transform_jac_out)
{
    for (int i = 0; i < SPACE_DIM; ++i)
    {
        local_transform_jac_out[i].setZero(4, 4);
        // For the default pose transformation g(0)
        local_transform_jac_out[i].col(SPACE_DIM) = transform_mat.col(SPACE_DIM);   
        // local_transform_jac_out[i](3, 3) = 0;   
        // (3, 3) should be 1 to account for the positions of the previous and subsequent joints in jac calculation
    }

    const E::Vector3d& w = jointAxisAngleRotation;
    double norm = w.norm();

    if (norm > 0.0001)
    {
        E::Matrix3d w_skew;
        w_skew <<
            0, -w[2], w[1],
            w[2], 0, -w[0],
            -w[1], w[0], 0;
        w_skew /= norm;

        E::MatrixXd rot_mat = transform_mat.block(0, 0, SPACE_DIM, SPACE_DIM);

        for (int i = 0; i < SPACE_DIM; ++i)
        {
            // compact formula from https://arxiv.org/pdf/1312.0788.pdf
            E::Vector3d cross = 
                w.cross((E::Matrix3d::Identity() - rot_mat).col(i))
                / (norm * norm);
            E::Matrix3d cross_skew;
            cross_skew <<
                0, -cross[2], cross[1],
                cross[2], 0, -cross[0],
                -cross[1], cross[0], 0;

            local_transform_jac_out[i].block(0, 0, 3, 3) =
                (w_skew * w[i] / norm + cross_skew) * rot_mat;
        }
    }
    else // zero case 
    {
        local_transform_jac_out[0].block(0, 0, 3, 3) <<
            0, 0, 0,
            0, 0, -1,
            0, 1, 0;
        local_transform_jac_out[1].block(0, 0, 3, 3) <<
            0, 0, 1,
            0, 0, 0,
            -1, 0, 0;
        local_transform_jac_out[2].block(0, 0, 3, 3) <<
            0, -1, 0,
            1, 0, 0,
            0, 0, 0;
    }

}

E::MatrixXd SMPLWrapper::get3DTranslationMat_(const E::Vector3d & translationVector)
{
    E::MatrixXd translation;
    translation.setIdentity(4, 4);  // in homogenious coordinates
    translation.block(0, 3, 3, 1) = translationVector;

    return translation;
}

E::SparseMatrix<double> SMPLWrapper::getLBSMatrix_(const E::MatrixXd & verts) const
{
    const int dim = SMPLWrapper::SPACE_DIM;
    const int nVerts = SMPLWrapper::VERTICES_NUM;
    const int nJoints = SMPLWrapper::JOINTS_NUM;  // Number of joints
#ifdef DEBUG
    std::cout << "LBSMat: start (analytic)" << std::endl;
#endif // DEBUG
    // +1 goes for homogenious coordinates
    E::SparseMatrix<double> LBSMat(nVerts, (dim + 1) * nJoints);
    std::vector<E::Triplet<double>> LBSTripletList;
    LBSTripletList.reserve(nVerts * (dim + 1) * SMPLWrapper::WEIGHTS_BY_VERTEX);     // for faster filling performance

    // go over non-zero weight elements
    for (int k = 0; k < this->weights_.outerSize(); ++k)
    {
        for (E::SparseMatrix<double>::InnerIterator it(this->weights_, k); it; ++it)
        {
            double weight = it.value();
            int idx_vert = it.row();
            int idx_joint = it.col();
            // premultiply weigths by vertex homogenious coordinates
            for (int idx_dim = 0; idx_dim < dim; idx_dim++)
                LBSTripletList.push_back(E::Triplet<double>(idx_vert, idx_joint * (dim + 1) + idx_dim, weight * verts(idx_vert, idx_dim)));
            LBSTripletList.push_back(E::Triplet<double>(idx_vert, idx_joint * (dim + 1) + dim, weight));
        }
    }
    LBSMat.setFromTriplets(LBSTripletList.begin(), LBSTripletList.end());

    return LBSMat;
}

SMPLWrapper::State::State()
{
    pose.setZero(JOINTS_NUM, SPACE_DIM);
    shape.setZero(SHAPE_SIZE);
    translation.setZero(SPACE_DIM);
    displacements.setZero(VERTICES_NUM, SPACE_DIM);
}
