from multiprocessing import Process
import cv2
from time import sleep

class CaptureProcess(Process):
    def __init__(self, stream_url: str, camera_name: str, frame_width: int, frame_height: int, frame_rate: int):
        Process.__init__(self)

        self.stream_url = stream_url
        self.camera_name = camera_name
        self.frame_width = frame_width
        self.frame_height = frame_height
        self.frame_rate = frame_rate

    def run(self):
        self.video_capture = cv2.VideoCapture(self.stream_url)

        if self.video_capture.isOpened():
            frame_size = (self.frame_width, self.frame_height)

            filename = f'{self.camera_name}.mkv'

            print(f'frame_size: {frame_size}')

            self.video_writer = cv2.VideoWriter(filename, cv2.VideoWriter_fourcc(*'H264'), self.frame_rate, frame_size)

            seconds_to_record = 10
            frames_to_capture = int(seconds_to_record * self.frame_rate)

            while frames_to_capture > 0:
                ret, frame = self.video_capture.read()

                if ret:
                    print(f'Frame captured, {frames_to_capture} remaining.')
                    self.video_writer.write(frame)
                    frames_to_capture -= 1

            self.video_writer.release()
            self.video_capture.release()
            
        print('Thread is ending')

CAMERAS = [
        { 'name': 'picam2', 'url': 'rtsp://littlerascal:8554/picam2', 'frame_width': 1280, 'frame_height': 720, 'frame_rate': 30 },
        # { 'name': 'polecat', 'url': 'rtsp://littlerascal:8554/polecat', 'frame_width': 1280, 'frame_height': 720, 'frame_rate': 30 }
    ]

def create_thread_for_camera(camera):
    capture_thread = CaptureProcess(camera['url'], camera['name'], camera['frame_width'], camera['frame_height'], camera['frame_rate'])

    capture_thread.daemon = True
    capture_thread.start()

    return capture_thread

def main():

    camera_threads = list(map(create_thread_for_camera, CAMERAS))
    
    for camera_thread in camera_threads:
        camera_thread.join()

    print('Complete')

if __name__ == '__main__':
    print('Starting')
    main()