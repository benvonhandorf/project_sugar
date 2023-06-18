#!/bin/bash

sudo systemctl restart nvargus-daemon

gst-launch-1.0 nvarguscamerasrc sensor-id=0 ! 'video/x-raw(memory:NVMM), width=(int)1280, height=(int)720, format=(string)NV12, framerate=(fraction)30/1' ! nvv4l2h264enc maxperf-enable=1 bitrate=1000000 ! h264parse ! rtspclientsink location="rtsp://localhost:8554/camera0"