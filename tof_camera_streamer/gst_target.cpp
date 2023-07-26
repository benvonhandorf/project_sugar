
#include "gst_target.hpp"
#include <iostream>
#include <gst/gstdebugutils.h>

Gst::Gst() {
    gst_init(NULL, NULL);
    GError *error = NULL;

    // this->pipeline = gst_parse_launch(
    //         "appsrc ! video/x-raw,format=RGB,width=240,height=180 ! autovideoconvert ! nvv4l2h264enc ! video/x-h264,level=(string)4 ! h264parse ! rtspclientsink location=\"rtsp://localhost:8554/camera1\"",
    //         &error);

    this->pipeline = gst_pipeline_new("manual-pipeline");

    this->appsrc = gst_element_factory_make("appsrc", "source");

    GstElement *element = nullptr;

    if(error) {
        std::cerr << "Unable to parse pipeline";
        std::cerr << error-> message;
        g_error_free(error);
    }

    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(this->pipeline), GST_DEBUG_GRAPH_SHOW_ALL, "tof_camera");
    std::cout << "Pipeline created";
}

void Gst::start() {
    gst_element_set_state(this->pipeline, GST_STATE_PLAYING);
}