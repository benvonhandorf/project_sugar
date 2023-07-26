#pragma once

#include <gst/gst.h>

class Gst
{
public:
    Gst();

    void start();

private:
    GstElement *pipeline = nullptr;
    GstElement *appsrc = nullptr;
    GstBus *bus = nullptr;
    GstMessage *msg = nullptr;
};