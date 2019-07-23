#pragma once
#include <ceres/ceres.h>
#include <igl/point_mesh_squared_distance.h>
#include <igl/signed_distance.h>

#include <GeneralMesh/GeneralMesh.h>
#include "SMPLWrapper.h"

class AbsoluteDistanceBase : public ceres::CostFunction
{
public:
    enum ParameterType{
        BASE, 
        TRANSLATION, 
        SHAPE, 
        POSE
    };
    enum DistanceType {
        IN_DIST, 
        OUT_DIST, 
        BOTH_DIST
    };

    AbsoluteDistanceBase(SMPLWrapper*, GeneralMesh *, 
        ParameterType parameter = BASE, DistanceType dist_type = BOTH_DIST, 
        double pruning_threshold = 100.);
    ~AbsoluteDistanceBase();

    // parameters[0] <-> this->parameter_type_
    // Main idea for point-to-surface distance jacobian: 
    // Gradient for each vertex correspondes to the distance from this vertex to the input mesh.
    virtual bool Evaluate(double const* const* parameters,
        double* residuals,
        double** jacobians) const;

protected:
    struct DistanceResult {
        Eigen::MatrixXd verts;
        Eigen::MatrixXd verts_normals;
        std::vector<Eigen::MatrixXd> jacobian;
        // libigl output
        Eigen::VectorXd signedDists; 
        Eigen::VectorXi closest_face_ids; 
        Eigen::MatrixXd closest_points;
        Eigen::MatrixXd normals_for_sign;
    };

    // TODO put result in static member
    std::unique_ptr<DistanceResult> calcDistance(double const * parameter, bool with_jacobian) const;

    void fillJac(const DistanceResult& distance_res, const double* residuals, double * jacobian) const;
    void fillTranslationJac(const DistanceResult& distance_res, const double* residuals, double * jacobian) const;

    // 
    template<typename Row1, typename Row2>
    inline double residual_elem_(const double signed_dist, 
        const Row1 vertex_normal, const Row2 input_normal) const
    {
        if (dist_evaluation_type_ == IN_DIST && signed_dist > 0     // is outside, want inside
            || dist_evaluation_type_ == OUT_DIST && signed_dist < 0 // is inside, want outside
            || abs(signed_dist) > pruning_threshold_                // too far
            || vertex_normal.dot(input_normal) <= 0)                // looks the wrong way. Check last, as it's most expensive
        {
            return 0;
        }

        return abs(signed_dist);
    }

    // Jac values are set to zero whenever the residual is zero/close to zero
    template<typename Row1, typename Row2, typename Row3>
    inline double jac_elem_(const Row1&& vertex,
        const Row2&& closest_input_point,
        double abs_dist,
        const Row3&& grad) const
    {
        double jac_entry = abs_dist < 1e-5
            ? 0
            : (vertex - closest_input_point).dot(grad) / abs_dist;
        return jac_entry;
    }

    inline double translation_jac_elem_(const double vert_coord,
        const double input_coord, double abs_dist) const
    {
        double jac_entry = abs_dist < 1e-5
            ? 0
            : (vert_coord - input_coord) / abs_dist;
        return jac_entry;
    }

    GeneralMesh * toMesh_;
    SMPLWrapper * smpl_;
    double pruning_threshold_;

    // instance type
    ParameterType parameter_type_;
    DistanceType dist_evaluation_type_;

    // last evaluated result
    //DistanceResult last_result_;
};
