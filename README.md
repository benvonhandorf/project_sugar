# project_sugar
Real time hummingbird detection and notification

## Goals ##
Given that:
- Hummingbirds are awesome
- I can't sit and watch my hummingbird feeders all day

The goals of this project are to:
- Monitor my hummingbird feeders - Sugar Cam
- Alert me when hummingbirds are detected
- Capture time-lapse videos of the hummingbirds - Sugar Cam
- Allow at-a-glance viewing of the feeders - Sugar Window
- Gather data about the presence of hummingbirds based on time of day, weather, etc.

## Limiting Factors ##
I should be able to provide a lot of this functionality with hardware on hand.  Where possible easy availability will outweigh general applicability.  The goal isn't to provide an all-in-one hummingbird monitoring package for others but to possibly provide some interesting parts that could be used.

## Architecture ##
To achieve these goals there are going to be a number of sub-projects, all within this repository.

### Cameras ###
This will be a set of 1-n cameras monitoring the various hummingbird feeders.  

Initially a Raspberry Pi Zero, 1st gen Raspberry Pi camera module and an appropriate [Motion Project](https://motion-project.github.io/index.html) configuration will provide an MJPEG stream as input but this may expand to include other input methods in the future.

### Sugar Window ###
Unfortunately I can't run to the feeders every time there is a hummingbird, so the view of the feeders will be provided by some display, preferably wall-mounted.  

Due to availability, version one will be based on an [Android Things Development Kit](https://developer.android.com/things/hardware/imx7d) that I have on hand.

### Monitoring and Recognition ###
This section is still under research.  The intention is to use machine learning vision algorithims (possibly [YOLO](https://pjreddie.com/darknet/yolo/)) to monitor the stream from one or more Sugar Cam and detect the presence of Hummmingbirds.  This data would then flow into the notification system.

