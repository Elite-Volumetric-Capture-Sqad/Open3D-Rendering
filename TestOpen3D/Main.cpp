#include "CameraManager.h"
#include "MKV_Data.h"
#include "Image_Data.h"
#include "ErrorLogger.h"
#include "VoxelGridData.h"
#include "AdditionalUtilities.h"

#include "open3d/io/sensor/azure_kinect/K4aPlugin.h"
#include "open3d/Open3D.h"
#include "open3d/io/sensor/azure_kinect/MKVMetadata.h"
#include "open3d/geometry/RGBDImage.h"
#include "ColorMapOptimizer.h"


#include "AlembicWriter.h"

#include <k4a/k4a.h>
#include <k4arecord/record.h>
#include <fstream>
#include <json/json.h>
#include <memory>
#include <turbojpeg.h>


#include <chrono>
#include <uvpCore.hpp>

using namespace open3d;
using namespace open3d::core;
using namespace open3d::io;
using namespace Eigen;
using namespace uvpcore;

namespace MKV_Rendering {
    void WriteOBJ(std::string filename, std::string filepath, open3d::geometry::TriangleMesh* mesh)
    {
        std::ofstream writer;

        if (filepath != "")
        {
            std::filesystem::create_directories(filepath);

            writer.open(filepath + "/" + filename);
        }
        else
        {
            writer.open(filename);
        }

        for (int i = 0; i < mesh->vertices_.size(); ++i)
        {
            auto vert = mesh->vertices_[i];

            writer << "v " << vert.x() << " " << vert.y() << " " << vert.z() << "\n";
        }

        for (int i = 0; i < mesh->triangle_uvs_.size(); ++i)
        {
            auto uv = mesh->triangle_uvs_[i];

            writer << "vt " << uv.x() << " " << uv.y() << "\n";
        }

        for (int i = 0; i < mesh->vertex_normals_.size(); ++i)
        {
            auto norm = mesh->vertex_normals_[i];

            writer << "vn " << norm.x() << " " << norm.y() << " " << norm.z() << "\n";
        }

        for (int i = 0; i < mesh->triangles_.size(); ++i)
        {
            auto tri = mesh->triangles_[i];

            writer << "f " <<
                (tri.x() + 1) << "/" << (tri.x() + 1) << "/" << (tri.x() + 1) << " " <<
                (tri.y() + 1) << "/" << (tri.y() + 1) << "/" << (tri.y() + 1) << " " <<
                (tri.z() + 1) << "/" << (tri.z() + 1) << "/" << (tri.z() + 1) << "\n";
        }

        writer.close();
    }

    template<class T>
    void DrawObject(T& object_to_draw)
    {
        std::vector<std::shared_ptr<const geometry::Geometry>> to_draw;

        auto object_ptr = std::make_shared<T>(
            object_to_draw);

        to_draw.push_back(object_ptr);

        visualization::DrawGeometries(to_draw);
    }

    void DrawMesh(geometry::TriangleMesh &object_to_draw)
    {
        std::vector<std::shared_ptr<const geometry::Geometry>> to_draw;

        auto object_ptr = std::make_shared<geometry::TriangleMesh>(
            object_to_draw);

        to_draw.push_back(object_ptr);
        visualization::DrawGeometries(to_draw);
    }

    void DrawImage(geometry::Image& object_to_draw)
    {
        std::vector<std::shared_ptr<const geometry::Geometry>> to_draw;

        auto object_ptr = std::make_shared<geometry::Image>(
            object_to_draw);

        to_draw.push_back(object_ptr);

        visualization::DrawGeometries(to_draw);
    }

    std::vector<Alembic::Abc::float32_t> double3ToAlembic(std::vector<Eigen::Vector3d> source) {
        std::vector<Alembic::Abc::float32_t> result;
        result.resize(source.size()*3); //source stores each element as  (x,y,z) while result stores it sequently

        #pragma omp parallel
        #pragma omp for
        for (int i = 0; i < source.size(); i++) {
            result[i * 3 + 0] = source[i].x();
            result[i * 3 + 1] = source[i].y();
            result[i * 3 + 2] = source[i].z();
        }

        return result;
    }

    std::vector<Alembic::Abc::float32_t> double3ToAlembicNegate(std::vector<Eigen::Vector3d> source) {
        std::vector<Alembic::Abc::float32_t> result;
        result.resize(source.size() * 3); //source stores each element as  (x,y,z) while result stores it sequently

        #pragma omp parallel
        #pragma omp for
        for (int i = 0; i < source.size(); i++) {
            result[i * 3 + 0] = source[i].x();
            result[i * 3 + 1] = source[i].y();
            result[i * 3 + 2] = source[i].z();
        }

        return result;
    }

    std::vector<Alembic::Abc::C3f> toAlembicColour(std::vector<Eigen::Vector3d> source) {
        std::vector<Alembic::Abc::C3f> result;
        result.resize(source.size());
        for (int i = 0; i < source.size(); i++) {
            result[i].x = source[i].x();
            result[i].y = source[i].y();
            result[i].z = source[i].z();
        }

        return result;
    }

    std::vector< Alembic::Abc::float32_t> toAlembicUVs(std::vector<Eigen::Vector2d> source) {
        std::vector<Alembic::Abc::float32_t> result;
        result.resize(source.size()*2);
        for (int i = 0; i < source.size(); i++) {
            result[i*2+0] = source[i].x();
            result[i*2+1] = source[i].y();
        }

        return result;
    }
    void saveMesh(geometry::TriangleMesh& object_to_draw, AlembicWriter& alembicWriter) {
        AlembicMeshData meshData;

        auto start = std::chrono::steady_clock::now();
        meshData.vertices = double3ToAlembic(object_to_draw.vertices_);
        meshData.numVerts = meshData.vertices.size() /3;
        
        meshData.numIndicies = object_to_draw.triangles_.size() * 3;
        meshData.indicies.resize(meshData.numIndicies);
        
        
        

        #pragma omp parallel
        #pragma omp for
        for (int i = 0; i < meshData.numIndicies/3; i++) {
            meshData.indicies[i * 3 + 0] = object_to_draw.triangles_[i].x();
            meshData.indicies[i * 3 + 1] = object_to_draw.triangles_[i].z();
            meshData.indicies[i * 3 + 2] = object_to_draw.triangles_[i].y();
        }

        meshData.numCounts = meshData.numIndicies / 3;
        meshData.counts.resize(meshData.numCounts);

        #pragma omp parallel
        #pragma omp for
        for (int i = 0; i < meshData.numCounts; i++) {
            meshData.counts[i] = 3;
        }

        meshData.normals = double3ToAlembicNegate(object_to_draw.vertex_normals_);
        meshData.numNormals = meshData.normals.size() / 3;

        meshData.uvs = toAlembicUVs(object_to_draw.triangle_uvs_);
        meshData.numUvs = object_to_draw.triangle_uvs_.size();

        meshData.vertexColours = toAlembicColour(object_to_draw.vertex_colors_);

        alembicWriter.saveFrame(meshData);

    }


    //Currently skipping frames for some reason
    void CreateImageArrayFromMKV(MKV_Data* data, std::string color_destination_folder, std::string depth_destination_folder, int max_output_images)
    {
        bool next_capture = true;

        int iter = 0;

        std::filesystem::create_directories(color_destination_folder);
        std::filesystem::create_directories(depth_destination_folder);

        while (next_capture && iter < max_output_images)
        {
            auto rgbd_image = data->GetFrameRGBD();

            std::string num = GetNumberFixedLength(iter, 8);

            std::string timestamp = std::to_string(data->GetTimestampCached());

            open3d::io::WriteImageToPNG(color_destination_folder + "/color_" + num + "_" + timestamp + ".png", rgbd_image->color_);
            open3d::io::WriteImageToPNG(depth_destination_folder + "/depth_" + num + "_" + timestamp + ".png", rgbd_image->depth_);

            next_capture = data->CycleCaptureForwards();

            ++iter;
        }
    }

    void SaveJSON(MKV_Data* data, std::string json_destination_folder_and_path)
    {
        data->WriteIntrinsics(json_destination_folder_and_path);
    }

    void CopyCalibration(std::string calibration_filename, std::string source_folder, std::string destination_folder)
    {
        std::filesystem::copy_file(source_folder + "/" + calibration_filename + ".log", destination_folder + "/" + calibration_filename + ".log", std::filesystem::copy_options::overwrite_existing);
    }

    void SaveMKVDataForImages(int max_output, std::string mkv_folder_path, std::string image_folder_path, 
        std::string intrinsics_filename, std::string calibration_filename, std::string color_folder_name, std::string depth_folder_name, double FPS)
    {
        auto directories = GetDirectories(mkv_folder_path);

        int iter = 0;

        for (auto dir : directories)
        {
            MKV_Data data(dir, "", "");

            std::string new_dir = image_folder_path + "/FramesCam" + GetNumberFixedLength(iter, 8);

            CreateImageArrayFromMKV(&data, new_dir + "/" + color_folder_name, new_dir + "/" + depth_folder_name, max_output);
            SaveJSON(&data, new_dir + "/" + intrinsics_filename);
            CopyCalibration(calibration_filename, dir, new_dir);

            std::ofstream structure_file;
            structure_file.open(new_dir + "/.structure");

            structure_file << "Version 0" << std::endl;
            structure_file << "Type image" << std::endl;
            structure_file << "Color " << color_folder_name << std::endl;
            structure_file << "Depth " << depth_folder_name << std::endl;
            structure_file << "Intrinsics_Json " << intrinsics_filename << std::endl;
            structure_file << "Calibration_File " << calibration_filename << std::endl;

            if (FPS > 0)
            {
                structure_file << "FPS " << std::to_string(FPS) << std::endl;
            }

            ++iter;
        }
    }


    void alembicCode() {
        std::string mkv_root_folder = "Kinect Test 1";
        std::string images_root_folder = "Kinect Test 2";
        std::string structure_file_name = ".structure";

        double FPS = 0;// 30;


        CameraManager cm(mkv_root_folder, structure_file_name);

        uint64_t lowTime = cm.GetHighestTimestamp();


        //CameraManager cm(images_root_folder, structure_file_name);
        //CameraManager cm(mkv_root_folder, structure_file_name);
        //CameraManager cm(images_root_folder, structure_file_name);

        
        //vgd.voxel_size = 9.f / 512.f;

        //uint64_t timestamp = 10900000; //Approximately 11 seconds in
        

        AlembicWriter alembicWriter("outputData/timeSample2ElectricBoogalo.abc", "Hogue", 1.0f, (1.0f / 30.0f));
        VoxelGridData vgd; //Edit values to toy with voxel grid settings
        vgd.voxel_size = 9.0f / 512.0f;
        uint64_t timestamp = 10900000; //Approximately 11 seconds in
        int i = 0;
        while (cm.CycleAllCamerasForward()) {


            auto alembicMesh = cm.GetMesh(&vgd);

            auto legacyMesh = alembicMesh.ToLegacyTriangleMesh();
            auto stitchedImage = cm.CreateUVMapAndTexture(&legacyMesh);

            //open3d::io::WriteImageToPNG("outputData/texture" + std::to_string(i) +".png", *stitchedImage);
            if (&alembicMesh != nullptr) {
                saveMesh(legacyMesh, alembicWriter);
            }
            i++;
        }
        uint64_t highTime = cm.GetHighestTimestamp();

        alembicWriter.setTimeSampling((float)lowTime / 1000000.0f, (float)(highTime - lowTime) / ((float)i * 1000000.0f));
    }
    void refactored_code_test()
    {
        std::string mkv_root_folder = "Kinect Test 1";
        std::string images_root_folder = "Kinect Test 2";
        std::string structure_file_name = ".structure";

        double FPS = 0;// 30;

        UvpOperationInputT inputUVP;

        //Use this to create a set of folders that are usable to construct a voxel grid from images instead of mkvs. No further setup should be required for them.
        //SaveMKVDataForImages(99999999, mkv_root_folder, images_root_folder, "intrinsic", "calib", "COLOR", "DEPTH", FPS);
        //
        //return;

        //The one for images currently has an incorrect FPS value due to the CreateImageArrayFromMKV function above
        //Also the extrinsics are broken in it

        
        
        CameraManager cm(mkv_root_folder, structure_file_name);

        

        
        //CameraManager cm(images_root_folder, structure_file_name);
        //CameraManager cm(mkv_root_folder, structure_file_name);
        //CameraManager cm(images_root_folder, structure_file_name);

        VoxelGridData vgd; //Edit values to toy with voxel grid settings
        //vgd.voxel_size = 9.f / 512.f;

        //uint64_t timestamp = 10900000; //Approximately 11 seconds in
        uint64_t timestamp = 7900000; //Approximately 8 seconds in
        //auto old_grid = ErrorLogger::EXECUTE("Get Old voxel Grid", &cm, &CameraManager::GetOldVoxelGrid, &vgd);
        //
        //std::cout << old_grid.HasVoxels() << std::endl;
        //std::cout << old_grid.HasColors() << std::endl;
        //
        //DrawObject(old_grid);
        //
        //return;

        auto vg = ErrorLogger::EXECUTE(
            "Generate Voxel Grid", &cm, &CameraManager::GetVoxelGridAtTimestamp, &vgd, timestamp
        );

        Eigen::Projective3d transformation = Eigen::Projective3d::Identity();

        transformation.translate(Eigen::Vector3d(0, 0, -3));

        camera::PinholeCameraIntrinsic intrinsic = camera::PinholeCameraIntrinsic(
            camera::PinholeCameraIntrinsicParameters::PrimeSenseDefault);

        auto focal_length = intrinsic.GetFocalLength();
        auto principal_point = intrinsic.GetPrincipalPoint();
        auto intrinsic_tensor = Tensor::Init<double>(
            { {focal_length.first, 0, principal_point.first},
             {0, focal_length.second, principal_point.second},
             {0, 0, 1} });

        auto result = vg.RayCast(
            intrinsic_tensor, eigen_converter::EigenMatrixToTensor(transformation.inverse().matrix()),
            cm.GetImageWidth(), cm.GetImageHeight(),
            vgd.depth_scale, 0.1f, vgd.depth_max, 3.0f, 
            t::geometry::TSDFVoxelGrid::SurfaceMaskCode::ColorMap
        );

        auto toDraw = t::geometry::Image(result[t::geometry::TSDFVoxelGrid::SurfaceMaskCode::ColorMap]).ToLegacyImage();

        DrawObject(toDraw);

        return;





        auto mesh = ErrorLogger::EXECUTE(
            "Generate Mesh", &cm, &CameraManager::GetMeshAtTimestamp, &vgd, timestamp
        );

        auto mesh_legacy = std::make_shared<geometry::TriangleMesh>(mesh.ToLegacyTriangleMesh());

        //mesh_legacy = mesh_legacy->FilterSmoothSimple(5);
        //mesh_legacy = mesh_legacy->FilterSmoothLaplacian(25, 0.3);
        //mesh_legacy = mesh_legacy->FilterSmoothTaubin(25);

        auto stitched_image = ErrorLogger::EXECUTE(
            "Generate Stitched Image And UVs", &cm, &CameraManager::CreateUVMapAndTextureAtTimestamp, &(*mesh_legacy), timestamp
        );

        DrawObject(*mesh_legacy);

        open3d::io::WriteImageToPNG("StitchedImageTest.png", *stitched_image);
        

        //DrawMesh(*mesh_legacy);
        //return;

  
        //ErrorLogger::EXECUTE("Test Error Logging", &cm, &CameraManager::MakeAnErrorOnPurpose, true);
    }

    
}


//Not used currently
void PrintHelp() {
    using namespace open3d;

    PrintOpen3DVersion();
    // clang-format off
    utility::LogInfo("Usage:");
    utility::LogInfo(">    <executable_name> [mkv_and_calibration_folder] [mesh_name]");
    utility::LogInfo("     Takes a folder containing several .mkv and .log files, and produces a mesh from the frames");
    utility::LogInfo("     [options]");
    utility::LogInfo("     --voxel_size [=0.0058 (m)]");
    utility::LogInfo("     --depth_scale [=1000.0]");
    utility::LogInfo("     --depth_max [=3.0]");
    utility::LogInfo("     --sdf_trunc [=0.04]");
    utility::LogInfo("     --device [CPU:0]");
    // clang-format on
    utility::LogInfo("");
}

int render_kinect(int argc, char** argv)
{
    if (argc == 1 || utility::ProgramOptionExists(argc, argv, "--help") ||
        argc < 4) {
        PrintHelp();
        return 1;
    }

    using MaskCode = t::geometry::TSDFVoxelGrid::SurfaceMaskCode;

    return 0;
}

int main() {
    ErrorLogger::EXECUTE("Refactored Code Test", &MKV_Rendering::refactored_code_test);

    
    return 0;
}