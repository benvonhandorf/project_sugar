import sys
import cv2
import numpy as np
import ArducamDepthCamera as ac
import time
import frame_util

MAX_DISTANCE = 2.0

amplitude_stream_url = 'rtsp://localhost:8554/camera1/depth_amplitude'
#depth_stream_url = 'rtsp://localhost:8554/camera1/depth_depth'
depth_stream_url = 'rtsp://localhost:8554/camera1'

# URL_PREFIX = 'appsrc ! video/x-raw,format=BGR,width=240,height=180 ! filesink location=raw.vid'
# stream_url = ''

URL_PREFIX = 'appsrc ! video/x-raw,format=BGR,width=240,height=180 ! videoconvert ! video/x-raw,format=RGBA ! nvvidconv ! video/x-raw(memory:NVMM) ! nvv4l2h264enc ! video/x-h264,level=(string)4 ! h264parse ! rtspclientsink location='
# URL_PREFIX = 'appsrc ! video/x-raw,format=BGR,width=240,height=180 ! autovideoconvert ! avenc_mjpeg ! rtpjpegpay ! rtspclientsink location='
# URL_PREFIX = 'appsrc ! video/x-raw,format=BGR,width=240,height=180 ! videoconvert ! video/x-raw,format=I420 ! '
# URL_PREFIX = 'appsrc ! video/x-raw,format=BGR,width=240,height=180 ! videoconvert ! video/x-raw,format=I420 ! avenc_mjpeg ! jpegparse ! rtpjpegpay ! rtspclientsink location='
stream_url = 'rtsp://localhost:8554/camera1'

FPS = 30
FRAME_SIZE = (240,180)

rng = np.random.default_rng()

def clamp_amplitude(amplitude_buf, clamp_value = 500):
    amplitude_buf[amplitude_buf > clamp_value] = clamp_value


def write_grayscale_image(video_writer, ranged_depth_data: np.ndarray, max_range: float):

    temp_image = np.empty_like(ranged_depth_data)

    temp_image = (temp_image / max_range) * 255

    np.nan_to_num(temp_image, max_range)

    temp_image = temp_image.astype(dtype=np.uint8)

    # temp_image = temp_image * (255/temp_image.max())
    # print(f'{np.nanmax(temp_image)} {np.nanmin(temp_image)}')

    # print(f'{temp_image.dtype} {temp_image.shape}')
    
    colormapped_image = cv2.cvtColor(temp_image, cv2.COLOR_GRAY2BGR)

    virdis_image = cv2.applyColorMap(colormapped_image, cv2.COLORMAP_VIRIDIS)

    # print(f'{colormapped_image.shape}')

    video_writer.write(virdis_image)

    return True # np.average(temp_image) < 60


AVERAGE_WINDOW = 15

amplitude_averaging_data = np.full((AVERAGE_WINDOW, 180, 240), np.nan, dtype=np.float32)

current_element = 0

mask_data = None

def add_average_amplitude(amplitude_buf):
    global current_element

    amplitude_averaging_data[current_element] = amplitude_buf
    current_element += 1

    if current_element >= AVERAGE_WINDOW:
        current_element = 0


def build_mask(amplitude_averaging_data, depth_data, cutoff_value = 60):
    amplitude_average = np.average(amplitude_averaging_data, axis=0)

    mask_data = np.full(amplitude_average.shape, np.nan, dtype=np.float32)

    mask_data = np.where(amplitude_average > cutoff_value, depth_data, np.nan)

    return mask_data

def apply_mask(depth_data, mask_data, inset_range: float = 0.15):
    # depth_data = np.where((mask_data == np.nan) , depth_data, np.nan)
    #| (mask_data > depth_data)
    #| ((mask_data - depth_data) > inset_range)

    return depth_data


if __name__ == "__main__":
    cam = ac.ArducamCamera()
    if cam.init(ac.TOFConnect.CSI,1) != 0 :
        print("initialization failed")
        exit(-1)

    if cam.start(ac.TOFOutput.DEPTH) != 0 :
        print("Failed to start camera")
        exit(-1)

    cam.setControl(ac.TOFControl.RANG, int(MAX_DISTANCE))

    stream_writer = cv2.VideoWriter(URL_PREFIX + stream_url, cv2.CAP_GSTREAMER, FPS, FRAME_SIZE, isColor=True)

    if not stream_writer.isOpened():
        print(f'Video writer is not opened')
        exit(-1)

    # background_depth_buffer = np.loadtxt("background_depth.csv", delimiter=',')

    print('Building background mask')

    for x in range(0, AVERAGE_WINDOW):
        frame = cam.requestFrame(200)

        if frame != None:
            depth_buf = frame.getDepthData()
            amplitude_buf = frame.getAmplitudeData()
            cam.releaseFrame(frame)

            clamp_amplitude(amplitude_buf)

            add_average_amplitude(amplitude_buf)

            time.sleep(0.50)

    mask_data = build_mask(amplitude_averaging_data, depth_buf)

    np.savetxt("depth_mask.csv", mask_data, delimiter=',')

    print(f'Mask complete')

    count = 0
    last_timestamp = time.time()

    while True:
        frame = cam.requestFrame(200)

        if frame != None :
            depth_buf = frame.getDepthData()
            amplitude_buf = frame.getAmplitudeData()
            cam.releaseFrame(frame)

            if depth_buf.shape != (180, 240) or amplitude_buf.shape != (180, 240):
                print(f'Invalid frame: {depth_buf.shape} {amplitude_buf.shape}')
                continue

            # if count == 0:
            #     np.savetxt("running_depth.csv", depth_buf, delimiter=',')
            #     np.savetxt("running_amplitude.csv", amplitude_buf, delimiter=',')

            clamp_amplitude(amplitude_buf)

            add_average_amplitude(amplitude_buf)

            amplitude_average = np.average(amplitude_averaging_data, axis=0)

            filtered_depth = frame_util.filter_depth_with_amplitude(depth_buf, amplitude_average, 60)

            masked_depth_data = apply_mask(filtered_depth, mask_data)

            ranged_depth = frame_util.convert_depth_to_meters(masked_depth_data, MAX_DISTANCE)

            if not write_grayscale_image(stream_writer, ranged_depth, MAX_DISTANCE):
                # Weird data, capture it.

                np.savetxt("suspect_depth.csv", depth_buf, delimiter=',')
                np.savetxt("amplitude_average.csv", amplitude_average, delimiter=',')

                print(f'Data captured.  {np.average(amplitude_average)} against {np.average(amplitude_averaging_data)}')

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


    stream_writer.release()
    cam.close()