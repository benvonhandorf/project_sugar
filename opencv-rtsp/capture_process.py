import datetime
import cv2
from multiprocessing import Process, Queue


class CaptureProcess(Process):
    def __init__(self, queue: Queue, stream_url: str, camera_name: str, frame_width: int, frame_height: int, frame_rate: int):
        Process.__init__(self)

        self.queue = queue

        self.stream_url = stream_url
        self.camera_name = camera_name
        self.frame_width = frame_width
        self.frame_height = frame_height
        self.frame_rate = frame_rate

        self.record_until = None
        self.video_writer = None

    def run(self):
        print(f'{self.camera_name}: Thread is starting - {self.stream_url}')

        self.video_capture = cv2.VideoCapture(self.stream_url)

        if self.video_capture.isOpened():
            frame_size = (self.frame_width, self.frame_height)

            while True:
                if not self.queue.empty():
                    print(f'{self.camera_name}: Queue check - {datetime.datetime.now().timestamp()}')
                    new_value = self.queue.get_nowait()

                    if new_value is not None:
                        print(f'{self.camera_name}: Process timestamp {new_value}')
                        if self.record_until is None:
                            filename = f'{self.camera_name}-{datetime.datetime.now().isoformat(sep="_", timespec="seconds")}.mkv'

                            print(f'{self.camera_name}: Capturing to {filename} - {self.frame_rate} - {frame_size}')

                            self.video_writer = cv2.VideoWriter(filename, cv2.VideoWriter_fourcc(*'h264'), self.frame_rate, frame_size)

                        self.record_until = new_value

                ret, frame = self.video_capture.read()

                if ret and self.video_writer:
                    self.video_writer.write(frame)

                if self.video_writer and self.record_until < datetime.datetime.now().timestamp():
                    print(f'{self.camera_name}: Completing')
                    self.video_writer.release()
                    self.video_writer = None
                    self.record_until = None

            self.video_capture.release()
            
        print(f'{self.camera_name}: Thread is ending')