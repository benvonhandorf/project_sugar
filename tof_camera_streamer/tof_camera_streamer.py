import sys
import cv2
import numpy as np
import ArducamDepthCamera as ac
import time
import frame_util

MAX_DISTANCE = 2

amplitude_stream_url = 'rtsp://littlerascal:8554/depth_amplitude'
depth_stream_url = 'rtsp://littlerascal:8554/depth_depth'
stream_url = 'rtsp://littlerascal:8554/depth'

# URL_PREFIX = 'appsrc ! videoconvert ! nvv4l2h264enc ! video/x-h264,level=(string)4 ! h264parse ! queue ! '
# URL_PREFIX = 'appsrc ! video/x-raw,format=RGB,width=240,height=180 ! autovideoconvert ! omxh264enc ! rtspclientsink location='
URL_PREFIX = 'appsrc ! fakesink'
FPS = 30
FRAME_SIZE = (240,180)

rng = np.random.default_rng()

def write_grayscale_image(video_writer, image_grayscale):
    temp_image = rng.random(size=(240, 180))
    temp_image = temp_image * 256
    temp_image = temp_image.astype(np.uint8)
    colormapped_image = cv2.applyColorMap(temp_image, cv2.COLORMAP_VIRIDIS)
    # temp_image = np.empty_like(image_grayscale)

    # cv2.normalize(image_grayscale, temp_image, alpha=0, beta=255, norm_type=cv2.NORM_MINMAX, dtype=cv2.CV_8U)
    
    # temp_image[temp_image < 0] = 0
    # np.nan_to_num(temp_image)

    # temp_image = temp_image * (255/temp_image.max())
    # print(f'{np.nanmax(temp_image)} {np.nanmin(temp_image)}')

    # temp_image = temp_image.astype(np.uint8)
    # temp_image = cv2.cvtColor(temp_image, cv2.COLOR_GRAY2RGB)

    video_writer.write(colormapped_image)

def transform_buffer(buffer):
    np.fliplr(buffer)
    np.flipud(buffer)


if __name__ == "__main__":
    cam = ac.ArducamCamera()
    if cam.init(ac.TOFConnect.CSI,0) != 0 :
        print("initialization failed")
        exit(-1)

    if cam.start(ac.TOFOutput.DEPTH) != 0 :
        print("Failed to start camera")
        exit(-1)

    cam.setControl(ac.TOFControl.RANG, MAX_DISTANCE)

    four_cc = cv2.VideoWriter_fourcc(*'MJPG')

    # amplitude_writer = cv2.VideoWriter(URL_PREFIX + amplitude_stream_url + " ", four_cc, FPS, FRAME_SIZE, isColor=False)
    # depth_writer = cv2.VideoWriter(URL_PREFIX + depth_stream_url + " ", four_cc, FPS, FRAME_SIZE, isColor=False)
    # stream_writer = cv2.VideoWriter(URL_PREFIX + stream_url, four_cc, FPS, FRAME_SIZE, isColor=True)
    stream_writer = cv2.VideoWriter(URL_PREFIX, four_cc, FPS, FRAME_SIZE, isColor=True)

    if not stream_writer.isOpened():
        print(f'Video writer is not opened')
        exit(-1)

    # background_depth_buffer = np.loadtxt("background_depth.csv", delimiter=',')

    count = 0
    last_timestamp = time.time()

    while True:
        frame = cam.requestFrame(200)

        if frame != None:
            depth_buf = frame.getDepthData()
            amplitude_buf = frame.getAmplitudeData()
            cam.releaseFrame(frame)

            transform_buffer(depth_buf)
            transform_buffer(amplitude_buf)

            normalized_depth = frame_util.normalize_depth(depth_buf, amplitude_buf, MAX_DISTANCE)

            # write_grayscale_image(depth_writer, depth_buf)
            # write_grayscale_image(amplitude_writer, amplitude_buf)
            write_grayscale_image(stream_writer, normalized_depth)

            # difference_depth = np.subtract(normalized_depth, background_depth_buffer)

            # np.savetxt("difference_depth.csv", difference_depth, delimiter=',')

            # difference_depth = np.nan_to_num(difference_depth)

            # sum = np.sum(difference_depth)

            # print(f'Delta Sum: {sum}')

        else:
            print("Unable to acquire frame")

        count = count + 1

        if count == 100:
            count = 0

            now = time.time()

            diff = now - last_timestamp

            fps = 100 / diff

            print(f'{fps} fps.')

            last_timestamp = now