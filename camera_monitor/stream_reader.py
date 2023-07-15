from multiprocessing import Process, Queue
from stream_configuration import StreamConfiguration
from frame_buffer import FrameBuffer
import logging
from time import perf_counter
from time import sleep
import cv2
import common

class StreamReader(Process):
    def __init__(self, stream_configuration:StreamConfiguration, frame_buffer: FrameBuffer, outgoing_queue: Queue):
        Process.__init__(self)

        self.frame_buffer = frame_buffer
        self.stream_configuration = stream_configuration
        self.capture_source = None
        self.timestamp = perf_counter()
        self.outgoing_queue = outgoing_queue
        self.consecutive_resets = 0

    def build_capture_source(self):
        self.logger.info(f'Capture Source:{self.stream_configuration.source}')

        if self.capture_source:
            self.capture_source.release()
            sleep(1)

        self.timestamp = perf_counter()
        self.frames = 0

        self.capture_source = cv2.VideoCapture(self.stream_configuration.source)

        new_ts = perf_counter()
        duration = new_ts - self.timestamp
        self.timestamp = new_ts
        self.logger.info(f'Capture Source created: {duration:.3f}s')

    def run(self):
        self.logger = common.get_logger('StreamReader')

        while True:
            try:
                self.build_capture_source()
                
                while self.capture_source.isOpened():
                    ret, frame = self.capture_source.read()

                    if ret:
                        self.frame_buffer.append(frame)

                        self.frames += 1

                        if self.consecutive_resets > 0 or self.frames == 1:
                            self.consecutive_resets = 0
                            self.outgoing_queue.put({'type': 'health', 'module': 'reader', 'status': 'healthy'})

                        if self.frames == 500:
                            new_ts = perf_counter()
                            duration = new_ts - self.timestamp
                            self.timestamp = new_ts
                            fps = self.frames / duration
                            
                            self.logger.info(f'{fps:.3f} fps read.')

                            self.frames = 0
                    else:
                        self.logger.warn(f'Frame read failed.  Restarting capture.')

                        self.build_capture_source()

                        self.consecutive_resets += 1

                        self.outgoing_queue.put({'type': 'health', 'module': 'reader', 'status': 'restart', 'consecutive_restarts': self.consecutive_resets})

                self.logger.warn(f'Capture source is closed.')
            except Exception as e:
                self.logger.error(f'Exception: {e}')



    
