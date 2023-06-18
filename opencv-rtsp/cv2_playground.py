import cv2
import numpy as np
import time

if __name__ == "__main__":
    # source = cv2.VideoCapture('filesrc location=/home/benvh/Downloads/PXL_20220826_005105598.mp4 ! decodebin ! videoconvert ! appsink', cv2.CAP_GSTREAMER)
    source = cv2.VideoCapture('videotestsrc ! video/x-raw,width=240,height=180,format=BGR,framerate=30/1 ! videoconvert ! appsink', cv2.CAP_GSTREAMER)
    # source = cv2.VideoCapture('rtspsrc location="rtsp://littlerascal:8554/picam01" protocols=tcp ! video/x-raw,width=240,height=180,format=BGR,framerate=30/1 ! videoconvert ! appsink', cv2.CAP_GSTREAMER)

    four_cc = 0 # cv2.VideoWriter_fourcc(*'MJPG')
    sink = cv2.VideoWriter('appsrc ! video/x-raw,format=BGR,width=240,height=180 ! videoconvert ! x264enc ! h264parse ! autovideosink ', cv2.CAP_GSTREAMER, four_cc, 30, (240,180))
    # sink = cv2.VideoWriter('appsrc ! video/x-raw,format=BGR,width=240,height=180 ! videoconvert ! nvh264enc ! video/x-h264,level=4 ! h264parse ! autovideosink', cv2.CAP_GSTREAMER, four_cc, 30, (240,180))
    # sink = cv2.VideoWriter('appsrc ! video/x-raw,format=BGRA,width=240,height=180 ! nvh264enc ! video/x-h264,level=4 ! h264parse ! rtspclientsink location=rtsp://littlerascal:8554/test_stream', cv2.CAP_GSTREAMER, four_cc, 30, (240,180))

    if not source.isOpened():
        print('Source failed to open')
        exit(0)

    # if not sink.isOpened():
    #     print('Sink failed to open')
    #     exit(0)

    count = 0
    last_timestamp = time.time()

    while True:
        ret, frame = source.read()

        if not ret:
            print('Empty frame')
            break

        converted_frame = frame # cv2.cvtColor(frame, cv2.COLOR_BGR2BGRA)

        # print(f'{frame.dtype} -> {converted_frame.dtype}')
        # print(f'{frame.shape} -> {converted_frame.shape}')
    
        sink.write(converted_frame)

        print(f'Wrote frame')


    source.release()
    sink.release()

    exit(0)


    #     count = count + 1

    #     if count == 100:
    #         count = 0

    #         now = time.time()

    #         diff = now - last_timestamp

    #         fps = 100 / diff

    #         print(f'{fps} fps.')

    #         last_timestamp = now

    # source.release()
    # sink.release()