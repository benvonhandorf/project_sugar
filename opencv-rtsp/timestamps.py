import cv2
from time import sleep

def get_timestamp_for_stream_url(stream_url: str):
    video_capture = cv2.VideoCapture(stream_url)

    if video_capture.isOpened():
        fps = video_capture.get(cv2.CAP_PROP_FPS)
        timestamps = [video_capture.get(cv2.CAP_PROP_POS_MSEC)]

    print(fps)
    print(timestamps)

    video_capture.release()

STREAM_URLS = ["rtsp://littlerascal:8554/polecat", "rtsp://littlerascal:8554/picam2"]
def main():
    print('Starting')

    for stream_url in STREAM_URLS:
        print(stream_url)
        get_timestamp_for_stream_url(stream_url)

    print('Complete')

if __name__ == '__main__':
    main()