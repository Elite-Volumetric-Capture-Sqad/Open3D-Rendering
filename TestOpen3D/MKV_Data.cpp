#include "MKV_Data.h"
#include "open3d/io/sensor/azure_kinect/MKVMetadata.h"
#include "open3d/io/sensor/azure_kinect/K4aPlugin.h"
#include "ErrorLogger.h"

#include "AdditionalUtilities.h"

#include <fstream>
#include <turbojpeg.h>

using namespace MKV_Rendering;

MKV_Data* MKV_Data::main_camera_data = nullptr;

void MKV_Data::Calibrate()
{
    if (k4a_result_t::K4A_RESULT_SUCCEEDED !=
        k4a_playback_open(mkv_file.c_str(), &handle))
    {
        ErrorLogger::LOG_ERROR("Failed to open file: " + mkv_file, true);
    }

    if (k4a_result_t::K4A_RESULT_SUCCEEDED !=
        k4a_playback_get_record_configuration(handle, &record_config))
    {
        ErrorLogger::LOG_ERROR("Failed to get record configuration from: " + mkv_file, true);
    }

    switch (record_config.wired_sync_mode)
    {
    case K4A_WIRED_SYNC_MODE_MASTER:
        if (main_camera_data == nullptr)
        {
            main_camera_data = this;
            std::cout << mkv_file << " set as main camera" << std::endl;
        }
        else
        {
            ErrorLogger::LOG_ERROR("Conflict between " + main_camera_data->mkv_file + " and " + mkv_file + " over main camera", true);
        }
        break;
    case K4A_WIRED_SYNC_MODE_SUBORDINATE:
        std::cout << mkv_file << " set as subordinate camera" << std::endl;
        break;
    default:
        ErrorLogger::LOG_ERROR("Bad record configuration on: " + mkv_file, true);
    }
}

void MKV_Data::GetPlaybackDataRaw()
{
    std::vector<uint8_t> playback_data;

    size_t data_len = 0;

    k4a_playback_get_raw_calibration(handle, nullptr, &data_len);

    playback_data.resize(data_len);

    std::cout << "Data size: " << data_len << std::endl;

    switch (k4a_playback_get_raw_calibration(handle, &playback_data[0], &data_len))
    {
    case k4a_buffer_result_t::K4A_BUFFER_RESULT_TOO_SMALL:
        ErrorLogger::LOG_ERROR("Buffer was to small in: " + mkv_file, true);
        break;
    case k4a_buffer_result_t::K4A_BUFFER_RESULT_FAILED:
        ErrorLogger::LOG_ERROR("Failed to make playback calibration: " + mkv_file, true);
        break;
    default:
        break;
    }

    if (k4a_result_t::K4A_RESULT_SUCCEEDED !=
        k4a_calibration_get_from_raw(
            (char*)playback_data.data(), playback_data.size() + 1,
            k4a_depth_mode_t::K4A_DEPTH_MODE_NFOV_UNBINNED,
            k4a_color_resolution_t::K4A_COLOR_RESOLUTION_1080P, &calibration
        ))
    {
        ErrorLogger::LOG_ERROR("Failed to turn raw data into calibration: " + mkv_file, true);
    }
}

void MKV_Data::GetIntrinsicTensor()
{
    auto params = calibration.color_camera_calibration.intrinsics.parameters.param;

    auto focalX = params.fx;
    auto focalY = params.fy;
    auto principleX = params.cx;
    auto principleY = params.cy;

    intrinsic_t = open3d::core::Tensor::Init<double>({
        {params.fx, 0, params.cx},
        {0, params.fy, params.cy},
        {0, 0, 1}
        });
}

void MKV_Data::GetExtrinsicTensor()
{
    std::string file_contents = "";
    std::ifstream calib_file = std::ifstream(calibration_file);

    while (!calib_file.eof())
    {
        std::string single_content = "";
        std::getline(calib_file, single_content);
        single_content += " ";

        file_contents.append(single_content);
    }

    Eigen::Vector3d translation;
    Eigen::Matrix3d r_mat_3 = Eigen::Matrix3d::Identity();

    std::vector<std::string> extrinsic_data;
    SplitString(file_contents, extrinsic_data, ' ');

    for (int j = 0; j < 3; ++j)
    {
        translation[j] = std::stof(extrinsic_data[j]);
    }

    for (int j = 0; j < 9; ++j)
    {
        r_mat_3(j / 3, j % 3) = std::stof(extrinsic_data[j + 3]);
    }

    Eigen::Matrix4d final_mat = Eigen::Matrix4d::Identity();

    final_mat.block<3, 3>(0, 0) = r_mat_3;
    final_mat.block<3, 1>(0, 3) = r_mat_3 * translation;

    final_mat = final_mat.inverse();

    extrinsic_t = open3d::core::eigen_converter::EigenMatrixToTensor(final_mat);
}

void MKV_Data::ConvertBGRAToRGB(open3d::geometry::Image& bgra, open3d::geometry::Image& rgb)
{
    if (bgra.bytes_per_channel_ != 1) {
        ErrorLogger::LOG_ERROR("BGRA input image must have 1 byte per channel.", true);
    }
    if (rgb.bytes_per_channel_ != 1) {
        ErrorLogger::LOG_ERROR("RGB output image must have 1 byte per channel.", true);
    }
    if (bgra.num_of_channels_ != 4) {
        ErrorLogger::LOG_ERROR("BGRA input image must have 4 channels.", true);
    }
    if (rgb.num_of_channels_ != 3) {
        ErrorLogger::LOG_ERROR("RGB output image must have 3 channels.", true);
    }
    if (bgra.width_ != rgb.width_ || bgra.height_ != rgb.height_) {
        ErrorLogger::LOG_ERROR(
            "BGRA input image and RGB output image have different dimensions.", 
            true);
    }

#ifdef _WIN32
#pragma omp parallel for schedule(static)
#else
#pragma omp parallel for collapse(3) schedule(static)
#endif
    for (int v = 0; v < bgra.height_; ++v) {
        for (int u = 0; u < bgra.width_; ++u) {
            for (int c = 0; c < 3; ++c) {
                *rgb.PointerAt<uint8_t>(u, v, c) =
                    *bgra.PointerAt<uint8_t>(u, v, 2 - c);
            }
        }
    }
}

std::shared_ptr<open3d::geometry::RGBDImage> MKV_Data::DecompressCapture()
{
    static std::shared_ptr<open3d::geometry::Image> color_buffer = nullptr;
    static std::shared_ptr<open3d::geometry::RGBDImage> rgbd_buffer = nullptr;

    if (color_buffer == nullptr) {
        color_buffer = std::make_shared<open3d::geometry::Image>();
    }
    if (rgbd_buffer == nullptr) {
        rgbd_buffer = std::make_shared<open3d::geometry::RGBDImage>();
    }

    k4a_image_t k4a_color = k4a_capture_get_color_image(*capture);
    k4a_image_t k4a_depth = k4a_capture_get_depth_image(*capture);
    if (k4a_color == nullptr || k4a_depth == nullptr) {
        ErrorLogger::LOG_ERROR("Capture at " + std::to_string(_timestamp) + " empty, skipping");
        return nullptr;
    }

    /* Process color */
    if (K4A_IMAGE_FORMAT_COLOR_MJPG !=
        k4a_image_get_format(k4a_color)) {
        ErrorLogger::LOG_ERROR("Unexpected image format at " + std::to_string(_timestamp) + ". The stream may have been corrupted.");
        return nullptr;
    }

    int width = k4a_image_get_width_pixels(k4a_color);
    int height = k4a_image_get_height_pixels(k4a_color);

    /* resize */
    rgbd_buffer->color_.Prepare(width, height, 3, sizeof(uint8_t));
    color_buffer->Prepare(width, height, 4, sizeof(uint8_t));

    tjhandle tjHandle;
    tjHandle = tjInitDecompress();
    if (0 !=
        tjDecompress2(tjHandle, k4a_image_get_buffer(k4a_color),
            static_cast<unsigned long>(
                k4a_image_get_size(k4a_color)),
            color_buffer->data_.data(), width, 0 /* pitch */, height,
            TJPF_BGRA, TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE)) {
        ErrorLogger::LOG_ERROR("Failed to decompress color image at " + std::to_string(_timestamp) + ".");
        return nullptr;
    }
    tjDestroy(tjHandle);
    ErrorLogger::EXECUTE(
        "Converting Image Type from BGRA to RGB", 
        this, 
        &MKV_Data::ConvertBGRAToRGB, *color_buffer, rgbd_buffer->color_);

    /* transform depth to color plane */
    k4a_image_t k4a_transformed_depth = nullptr;
    if (transform) {
        rgbd_buffer->depth_.Prepare(width, height, 1, sizeof(uint16_t));
        k4a_image_create_from_buffer(
            K4A_IMAGE_FORMAT_DEPTH16, width, height,
            width * sizeof(uint16_t), rgbd_buffer->depth_.data_.data(),
            width * height * sizeof(uint16_t), NULL, NULL,
            &k4a_transformed_depth);
        if (K4A_RESULT_SUCCEEDED !=
            k4a_transformation_depth_image_to_color_camera(
                transform, k4a_depth, k4a_transformed_depth)) {
            ErrorLogger::LOG_ERROR("Failed to transform depth frame to color frame at " + std::to_string(_timestamp) + ".", true);
            return nullptr;
        }
    }
    else {
        rgbd_buffer->depth_.Prepare(
            k4a_image_get_width_pixels(k4a_depth),
            k4a_image_get_height_pixels(k4a_depth), 1,
            sizeof(uint16_t));
        memcpy(rgbd_buffer->depth_.data_.data(),
            k4a_image_get_buffer(k4a_depth),
            k4a_image_get_size(k4a_depth));
    }

    /* process depth */
    k4a_image_release(k4a_color);
    k4a_image_release(k4a_depth);
    if (transform) {
        k4a_image_release(k4a_transformed_depth);
    }

    return rgbd_buffer;
}

MKV_Data::MKV_Data(std::string mkv_file, std::string calibration_file)
{
    this->mkv_file = mkv_file;
    this->calibration_file = calibration_file;

    ErrorLogger::EXECUTE("Calibrate Camera", this, &MKV_Data::Calibrate);

    ErrorLogger::EXECUTE("Retrieve Playback Data", this, &MKV_Data::GetPlaybackDataRaw);

    ErrorLogger::EXECUTE("Create Intrinsic Tensor", this, &MKV_Data::GetIntrinsicTensor);
    ErrorLogger::EXECUTE("Create Extrinsic Tensor", this, &MKV_Data::GetExtrinsicTensor);

    transform = k4a_transformation_create(&calibration);

    ErrorLogger::EXECUTE("Create Extrinsic Tensor", this, &MKV_Data::CycleCaptureForwards);
}

MKV_Data::~MKV_Data()
{
    if (main_camera_data == this)
    {
        main_camera_data = nullptr;
    }

    if (capture != nullptr)
    {
        k4a_capture_release(*capture);
        delete capture;
    }

    k4a_transformation_destroy(transform);
    k4a_playback_close(handle);
}

uint64_t MKV_Data::GetCaptureTimestamp()
{
    uint64_t min_timestamp = -1;
    k4a_image_t images[3];
    images[0] = k4a_capture_get_color_image(*capture);
    images[1] = k4a_capture_get_depth_image(*capture);
    images[2] = k4a_capture_get_ir_image(*capture);

    for (int i = 0; i < 3; i++)
    {
        if (images[i] != NULL)
        {
            uint64_t timestamp = k4a_image_get_device_timestamp_usec(images[i]);
            if (timestamp < min_timestamp)
            {
                min_timestamp = timestamp;
            }
            k4a_image_release(images[i]);
            images[i] = NULL;
        }
    }

    return min_timestamp;
}

void MKV_Data::CycleCaptureForwards()
{
    if (capture == nullptr)
    {
        capture = new k4a_capture_t();
    }
    else
    {
        k4a_capture_release(*capture);
    }

    *capture = NULL;

    switch (k4a_playback_get_next_capture(handle, capture))
    {
    case k4a_stream_result_t::K4A_STREAM_RESULT_EOF:
        ErrorLogger::LOG_ERROR("Stream has reached EOF on: " + mkv_file, true);
        break;
    case k4a_stream_result_t::K4A_STREAM_RESULT_FAILED:
        ErrorLogger::LOG_ERROR("Stream failed on: " + mkv_file, true);
        break;
    default:
        _timestamp = ErrorLogger::EXECUTE("Retrieving Capture Timestamp", this, &MKV_Data::GetCaptureTimestamp);
        break;
    }
}

void MKV_Data::CycleCaptureBackwards()
{
    if (capture == nullptr)
    {
        capture = new k4a_capture_t();
    }
    else
    {
        k4a_capture_release(*capture);
    }

    *capture = NULL;

    switch (k4a_playback_get_previous_capture(handle, capture))
    {
    case k4a_stream_result_t::K4A_STREAM_RESULT_EOF:
        ErrorLogger::LOG_ERROR("Stream has reached EOF on: " + mkv_file, true);
        break;
    case k4a_stream_result_t::K4A_STREAM_RESULT_FAILED:
        ErrorLogger::LOG_ERROR("Stream failed on: " + mkv_file, true);
        break;
    default:
        _timestamp = ErrorLogger::EXECUTE("Retrieving Capture Timestamp", this, &MKV_Data::GetCaptureTimestamp);
        break;
    }
}

void MKV_Data::SeekToTime(uint64_t time)
{
    if (capture == nullptr)
    {
        capture = new k4a_capture_t();
    }
    else
    {
        k4a_capture_release(*capture);
    }

    *capture = NULL;

    if (k4a_result_t::K4A_RESULT_SUCCEEDED !=
        k4a_playback_seek_timestamp(handle, time, k4a_playback_seek_origin_t::K4A_PLAYBACK_SEEK_BEGIN))
    {
        ErrorLogger::LOG_ERROR("Problem seeking timestamp on: " + mkv_file, true);
    }

    switch (k4a_playback_get_next_capture(handle, capture))
    {
    case k4a_stream_result_t::K4A_STREAM_RESULT_EOF:
        ErrorLogger::LOG_ERROR("Stream has reached EOF on: " + mkv_file, true);
        break;
    case k4a_stream_result_t::K4A_STREAM_RESULT_FAILED:
        ErrorLogger::LOG_ERROR("Stream failed on: " + mkv_file, true);
        break;
    default:
        _timestamp = ErrorLogger::EXECUTE("Retrieving Capture Timestamp", this, &MKV_Data::GetCaptureTimestamp);
        break;
    }
}

open3d::core::Tensor MKV_Rendering::MKV_Data::GetIntrinsic()
{
    return intrinsic_t;
}

open3d::core::Tensor MKV_Rendering::MKV_Data::GetExtrinsic()
{
    return extrinsic_t;
}

std::shared_ptr<open3d::geometry::RGBDImage> MKV_Data::GetFrameRGBD()
{
    bool valid_frame = false;

    std::shared_ptr<open3d::geometry::RGBDImage> rgbd;

    while (!valid_frame)
    {
        rgbd = ErrorLogger::EXECUTE("Decompressing Capture", this, &MKV_Data::DecompressCapture);

        valid_frame = (rgbd != nullptr);

        if (!valid_frame)
        {
            ErrorLogger::EXECUTE("Cycling Through Bad Capture", this, &MKV_Data::CycleCaptureForwards);
        }
    }

    return rgbd;
}
