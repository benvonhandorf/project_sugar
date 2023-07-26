#include "ArducamTOFCamera.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#include <chrono>
#include <iostream>
#include "gst_target.hpp"

// MAX_DISTANCE value modifiable  is 2 or 4
#define MAX_DISTANCE 2
using namespace Arducam;

cv::Mat matRotateClockWise180(cv::Mat src)
{
    if (src.empty())
    {
        std::cerr << "RorateMat src is empty!";
    }

    flip(src, src, 0);
    flip(src, src, 1);
    return src;
}

void getPreview(uint8_t *preview_ptr, float *phase_image_ptr, float *amplitude_image_ptr)
{
    auto len = 240 * 180;
    for (auto i = 0; i < len; i++)
    {
        uint8_t mask = *(amplitude_image_ptr + i) > 30 ? 254 : 0;
        float depth = ((1 - (*(phase_image_ptr + i) / MAX_DISTANCE)) * 255);
        uint8_t pixel = depth > 255 ? 255 : depth;
        *(preview_ptr + i) = pixel & mask;
    }
}

int main() {
    Gst gst;

    ArducamTOFCamera tof;
    ArducamFrameBuffer *frame;
    if (tof.open(Connection::CSI, 1)){
        std::cerr<<"initialization failed"<<std::endl;
        exit(-1);
    }

    if (tof.start()){
        std::cerr<<"Failed to start camera"<<std::endl;
        exit(-1);
    }
    //  Modify the range also to modify the MAX_DISTANCE
    tof.setControl(CameraCtrl::RANGE,MAX_DISTANCE);
    CameraInfo tofFormat = tof.getCameraInfo();

    std::cout << "Camera Info:" << tofFormat.width << ", " << tofFormat.height << std::endl;

    uint32_t frame_count = 0;

    std::chrono::high_resolution_clock::time_point time_beg = std::chrono::high_resolution_clock::now();    

    float *depth_ptr;
    float *amplitude_ptr;
    uint8_t *preview_ptr = new uint8_t[tofFormat.height * tofFormat.width];

    while(true) {
        frame = tof.requestFrame(200);

        if (frame != nullptr)
        {
            frame_count++;

            depth_ptr = (float *)frame->getData(FrameType::DEPTH_FRAME);
            amplitude_ptr = (float *)frame->getData(FrameType::AMPLITUDE_FRAME);
            getPreview(preview_ptr, depth_ptr, amplitude_ptr);

            cv::Mat result_frame(tofFormat.height, tofFormat.width, CV_8U, preview_ptr);
            cv::Mat depth_frame(tofFormat.height, tofFormat.width, CV_32F, depth_ptr);
            cv::Mat amplitude_frame(tofFormat.height, tofFormat.width, CV_32F, amplitude_ptr);

            depth_frame = matRotateClockWise180(depth_frame);
            result_frame = matRotateClockWise180(result_frame);
            amplitude_frame = matRotateClockWise180(amplitude_frame);

            cv::applyColorMap(result_frame, result_frame, cv::COLORMAP_JET);
            amplitude_frame.convertTo(amplitude_frame, CV_8U, 255.0 / 1024, 0);

            if((frame_count % 100) == 0) {
                std::chrono::high_resolution_clock::time_point time_end = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::ratio<1, 1>> duration_s = time_end - time_beg;

                double fps = ((double) frame_count) / duration_s.count();

                std::cout << "fps:" << fps << std::endl;
            }
        }

        tof.releaseFrame(frame);
    }

    if (tof.stop())
        exit(-1);
    tof.close();
    return 0;
}